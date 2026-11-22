/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "update_storage.h"
#include "nightshade.h"
#include "update_payload.h"
#include "update_release.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz/miniz.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

typedef struct
{
    char name[MT_PATH_MAX];
    mz_uint index;
    uint64_t size, begin, end;
    bool directory, listed;
    unsigned char hash[32];
} Entry;
static bool fail(char *e, size_t n, const char *why)
{
    if (e && n)
        snprintf(e, n, "%s", why);
    return false;
}
static bool add(uint64_t *total, uint64_t n)
{
    if (UINT64_MAX - *total < n)
        return false;
    *total += n;
    return true;
}
static bool paths(const char *installed, char root[MT_PATH_MAX], char work[MT_PATH_MAX],
                  char journal[MT_PATH_MAX], struct dr_save_entry *scope)
{
    if (!mt_canonical(installed, root) || !mt_path(work, root, ".eclipse-update") ||
        !mt_path(journal, work, "transaction"))
        return false;
    *scope = (struct dr_save_entry){
        .kind = DR_SAVE_DIRECTORY, .scope = DR_SAVE_GAME, .policy = DR_SAVE_READ_WRITE};
    strcpy(scope->path, root);
    return true;
}
/* The workspace is app-owned and never an archive destination. Do not traverse links,
 * and distinguish enumeration failures from EOF before declaring recovery complete. */
static bool clear(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return false;
    DIR *d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    for (;;)
    {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e)
        {
            if (errno)
                ok = false;
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[MT_PATH_MAX];
        if (!mt_path(p, path, e->d_name) || lstat(p, &st))
        {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode))
            ok = clear(p);
        else if (S_ISREG(st.st_mode))
            ok = !unlink(p);
        else
            ok = false;
        if (!ok)
            break;
    }
    if (closedir(d))
        ok = false;
    return ok && !rmdir(path) && mt_sync_parent(path);
}
bool update_storage_recover(const char *installed, bool *restart, char *error, size_t size)
{
    char root[MT_PATH_MAX], work[MT_PATH_MAX], journal[MT_PATH_MAX];
    struct dr_save_entry scope;
    struct stat st;
    *restart = false;
    if (!paths(installed, root, work, journal, &scope))
        return fail(error, size, "Invalid installation path");
    if (lstat(work, &st))
    {
        if (errno != ENOENT)
            return fail(error, size, "Cannot inspect update recovery");
        /* A previous cleanup may have removed the workspace but failed its final
         * parent fsync. Certify that absence durably on every recovery retry. */
        if (!lstat(root, &st))
        {
            if (!S_ISDIR(st.st_mode))
                return false;
            if (mt_sync_dir(root))
                return true;
            /* A mounted read-only app has no outstanding journal here and cannot stage writes. */
            struct statvfs fs;
            if (statvfs(root, &fs) == 0 && (fs.f_flag & ST_RDONLY))
                return true;
            return fail(error, size, "Cannot finish checking the installed app folder");
        }
        return errno == ENOENT;
    }
    *restart = true;
    if (!S_ISDIR(st.st_mode) || !mt_recover(journal, &scope, 1, NULL, error, size) || !clear(work))
        return fail(error, size, "Update recovery incomplete; keep recovery files and retry");
    return true;
}
static int compare(const void *a, const void *b)
{
    return strcasecmp(((const Entry *)a)->name, ((const Entry *)b)->name);
}
static int extent_compare(const void *a, const void *b)
{
    uint64_t x = ((const Entry *)a)->begin, y = ((const Entry *)b)->begin;
    return x < y ? -1 : x > y;
}
static bool extent(mz_zip_archive *z, const mz_zip_archive_file_stat *s, Entry *e)
{
    unsigned char h[30];
    e->begin = s->m_local_header_ofs;
    e->end = e->begin;
    if (e->begin >= z->m_central_directory_file_ofs ||
        z->m_pRead(z->m_pIO_opaque, e->begin, h, sizeof(h)) != sizeof(h) ||
        memcmp(h, "PK\003\004", 4))
        return false;
    uint64_t header = 30u + h[26] + ((unsigned)h[27] << 8) + h[28] + ((unsigned)h[29] << 8);
    if (!add(&e->end, header) || !add(&e->end, s->m_comp_size))
        return false;
    /* Data descriptors are verified by miniz; include their minimal extent here. */
    if (h[6] & 8)
    {
        unsigned char signature[4];
        if (z->m_pRead(z->m_pIO_opaque, e->end, signature, 4) != 4)
            return false;
        uint64_t descriptor =
            (s->m_comp_size >= UINT32_MAX || s->m_uncomp_size >= UINT32_MAX) ? 20 : 12;
        if (!memcmp(signature, "PK\007\010", 4))
            descriptor += 4;
        if (!add(&e->end, descriptor))
            return false;
    }
    return e->end <= z->m_central_directory_file_ofs;
}
static bool name_valid(const char *p)
{
    if (!mt_relative(p) || p[0] == '.')
        return false;
    for (const unsigned char *c = (const unsigned char *)p; *c; ++c)
        if (*c < 32 || *c == 127 || *c == '\\' || *c == ':')
            return false;
    /* Shared transaction staging suffix is reserved, including nested components. */
    return !strstr(p, ".eclipse-stage");
}
static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static bool manifest(mz_zip_archive *z, Entry *entries, mz_uint count)
{
    Entry key = {0};
    strcpy(key.name, UPDATE_PAYLOAD_FILE);
    Entry *m = bsearch(&key, entries, count, sizeof(*entries), compare);
    if (!m || m->directory || m->size > 16u * 1024u * 1024u ||
        m->size < sizeof(UPDATE_PAYLOAD_MAGIC) - 1)
        return false;
    size_t length = 0;
    char *data = mz_zip_reader_extract_to_heap(z, m->index, &length, 0);
    if (!data || length != m->size)
    {
        free(data);
        return false;
    }
    char *text = malloc(length + 1);
    if (!text)
    {
        free(data);
        return false;
    }
    memcpy(text, data, length);
    text[length] = 0;
    free(data);
    bool ok = strlen(text) == length &&
              !strncmp(text, UPDATE_PAYLOAD_MAGIC, sizeof(UPDATE_PAYLOAD_MAGIC) - 1);
    char *p = text + sizeof(UPDATE_PAYLOAD_MAGIC) - 1;
    while (ok && *p)
    {
        char *end = strchr(p, '\n');
        if (!end || end - p < 68)
        {
            ok = false;
            break;
        }
        *end = 0;
        unsigned char hash[32];
        for (int i = 0; i < 32; ++i)
        {
            int a = hex(p[i * 2]), b = hex(p[i * 2 + 1]);
            if (a < 0 || b < 0)
            {
                ok = false;
                break;
            }
            hash[i] = (unsigned char)((a << 4) | b);
        }
        if (!ok || p[64] != ' ')
        {
            ok = false;
            break;
        }
        char *number = p + 65, *tail = number;
        uint64_t bytes = 0;
        while (*tail >= '0' && *tail <= '9')
        {
            unsigned digit = (unsigned)(*tail++ - '0');
            if (bytes > (UINT64_MAX - digit) / 10)
            {
                ok = false;
                break;
            }
            bytes = bytes * 10 + digit;
        }
        if (!ok || tail == number || *tail != ' ' || strlen(tail + 1) >= sizeof(key.name))
        {
            ok = false;
            break;
        }
        strcpy(key.name, tail + 1);
        Entry *e = bsearch(&key, entries, count, sizeof(*entries), compare);
        ok = e && e != m && !e->directory && !e->listed && !strcmp(e->name, key.name) &&
             e->size == bytes;
        if (ok)
        {
            e->listed = true;
            memcpy(e->hash, hash, 32);
        }
        p = end + 1;
    }
    for (mz_uint i = 0; ok && i < count; ++i)
        if (!entries[i].directory && entries + i != m && !entries[i].listed)
            ok = false;
    for (size_t i = 0; ok && i < sizeof(update_payload_required) / sizeof(*update_payload_required);
         ++i)
    {
        strcpy(key.name, update_payload_required[i]);
        Entry *e = bsearch(&key, entries, count, sizeof(*entries), compare);
        ok = e && e->listed && e->size && !strcmp(key.name, e->name);
    }
    free(text);
    return ok;
}
static size_t extract(void *ctx, mz_uint64 offset, const void *bytes, size_t n)
{
    int fd = *(int *)ctx;
    (void)offset;
    size_t at = 0;
    while (at < n)
    {
        ssize_t w = write(fd, (const char *)bytes + at, n - at);
        if (w <= 0)
            return at;
        at += (size_t)w;
    }
    return at;
}
typedef struct
{
    const MtObserver *outer;
    bool committed;
} Observe;
static bool cancelled(void *ctx)
{
    return mt_cancelled(((Observe *)ctx)->outer);
}
static void progress(void *ctx, MtPhase phase, uint64_t done, uint64_t total)
{
    Observe *o = ctx;
    if (phase == MT_COMMITTED)
        o->committed = true;
    if (o->outer && o->outer->progress)
        o->outer->progress(o->outer->context, phase, done, total);
}
bool update_storage_apply(const char *zip, const char *installed, const char *expected_version,
                          const MtObserver *observer, char *error, size_t es)
{
    if (!maintenance_acquire(MAINT_UPDATE))
        return fail(error, es, "Waiting for game and save maintenance to finish");
    char root[MT_PATH_MAX], work[MT_PATH_MAX], journal[MT_PATH_MAX], stage[MT_PATH_MAX];
    struct dr_save_entry scope;
    struct stat st;
    bool ok =
        paths(installed, root, work, journal, &scope) && !lstat(root, &st) && S_ISDIR(st.st_mode);
    bool workspace = false, restart = false;
    mz_zip_archive z = {0};
    Entry *entries = NULL;
    MtTarget *targets = NULL;
    const char *why = "Invalid or incomplete update archive";
    if (!ok)
    {
        why = "Cannot find the installed application";
        goto finish;
    }
    if (!lstat(work, &st) || errno != ENOENT)
    {
        ok = false;
        why = "Update recovery must finish before retry";
        goto finish;
    }
    ok = mz_zip_reader_init_file(&z, zip, 0);
    if (!ok)
        goto finish;
    mz_uint count = mz_zip_reader_get_num_files(&z);
    ok = count > 0 && count <= MT_FILES_MAX;
    if (!ok)
        goto finish;
    entries = calloc(count, sizeof(*entries));
    targets = calloc(count, sizeof(*targets));
    ok = entries && targets;
    if (!ok)
    {
        why = "Not enough memory to verify update";
        goto finish;
    }
    uint64_t payload = 0, originals = 0, needed = 0;
    uint32_t files = 0;
    for (mz_uint i = 0; ok && i < count; ++i)
    {
        mz_zip_archive_file_stat s;
        ok = mz_zip_reader_file_stat(&z, i, &s) && !s.m_is_encrypted && s.m_is_supported &&
             mz_zip_reader_get_filename(&z, i, NULL, 0) <= sizeof(s.m_filename);
        if (!ok)
            break;
        const char *p = s.m_filename;
        const size_t prefix = sizeof(NP_TITLE_ID "/") - 1;
        if (strncmp(p, NP_TITLE_ID "/", prefix))
        {
            ok = false;
            break;
        }
        p += prefix;
        if (!extent(&z, &s, entries + i))
        {
            ok = false;
            break;
        }
        if (!*p && s.m_is_directory)
        {
            strcpy(entries[i].name, NP_TITLE_ID "/");
            entries[i].directory = true;
            continue;
        }
        size_t len = strlen(p);
        if (s.m_is_directory && len && p[len - 1] == '/')
            --len;
        if (!len || len >= sizeof(entries[i].name))
        {
            ok = false;
            break;
        }
        memcpy(entries[i].name, p, len);
        entries[i].name[len] = 0;
        entries[i].index = i;
        entries[i].size = s.m_uncomp_size;
        entries[i].directory = s.m_is_directory;
        unsigned type = (s.m_external_attr >> 16) & 0170000;
        ok = name_valid(entries[i].name) &&
             (!type || type == (s.m_is_directory ? 0040000 : 0100000)) &&
             s.m_local_header_ofs < z.m_archive_size &&
             s.m_comp_size <= z.m_archive_size - s.m_local_header_ofs;
        if (!ok || s.m_is_directory)
            continue;
        ok = add(&payload, s.m_uncomp_size);
    }
    if (!ok)
    {
        ok = false;
        goto finish;
    }
    qsort(entries, count, sizeof(*entries), extent_compare);
    for (mz_uint i = 1; i < count; ++i)
        if (entries[i].begin < entries[i - 1].end)
        {
            ok = false;
            break;
        }
    if (!ok)
        goto finish;
    qsort(entries, count, sizeof(*entries), compare);
    for (mz_uint i = 0; ok && i < count; ++i)
    {
        Entry *e = entries + i;
        if (i)
        {
            Entry *prev = entries + i - 1;
            size_t n = strlen(prev->name);
            ok = strcasecmp(prev->name, e->name) != 0 &&
                 (prev->directory || strncasecmp(prev->name, e->name, n) || e->name[n] != '/');
        }
        if (!ok || e->directory)
            continue;
        MtTarget *t = targets + files++;
        t->scope = scope;
        ok = mt_path(t->target, root, e->name) && mt_scope_contains(&scope, t->target);
        t->mode =
            !strcmp(e->name, "eboot.bin") || !strncmp(e->name, "sce_module/", 11) ? 0777 : 0644;
        if (ok && !lstat(t->target, &st))
            ok = S_ISREG(st.st_mode) && st.st_size >= 0 && add(&originals, (uint64_t)st.st_size);
        else if (ok)
            ok = errno == ENOENT;
    }
    ok = ok && manifest(&z, entries, count);
    if (ok)
    {
        Entry key = {0};
        strcpy(key.name, "sce_sys/param.json");
        Entry *param = bsearch(&key, entries, count, sizeof(*entries), compare);
        size_t length = 0;
        void *json = NULL;
        ok = param && param->size <= 1024u * 1024u;
        if (ok)
            json = mz_zip_reader_extract_to_heap(&z, param->index, &length, 0);
        ok = ok && json && release_payload_identity(json, length, NP_TITLE_ID, expected_version);
        free(json);
    }
    /* ZIP already occupies its filesystem. Space for full extraction, adjacent stages,
     * rollback originals and a second original during rollback, plus bounded metadata.
     * Save recovery copies already consumed space and are reflected in f_bavail. */
    ok = ok && add(&needed, payload) && add(&needed, payload) && add(&needed, originals) &&
         add(&needed, originals) && add(&needed, (uint64_t)count * (sizeof(MtTarget) + 512u)) &&
         add(&needed, 16u * 1024u * 1024u);
    if (!ok)
        goto finish;
    if (!mt_space(root, needed))
    {
        ok = false;
        why = "Not enough space for update staging and rollback";
        goto finish;
    }
    if (!mz_zip_validate_archive(&z, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY))
    {
        ok = false;
        goto finish;
    }
    ok = mt_mkdirs(work);
    workspace = ok;
    ok = ok && mt_path(stage, work, "payload") && mt_mkdirs(stage);
    uint32_t at = 0;
    for (mz_uint i = 0; ok && i < count; ++i)
    {
        Entry *e = entries + i;
        if (e->directory)
            continue;
        MtTarget *t = targets + at;
        char name[32];
        snprintf(name, sizeof(name), "%u", at);
        ok = mt_path(t->source, stage, name);
        if (!ok)
            break;
        int fd = open(t->source, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        ok = fd >= 0 && !mt_cancelled(observer) &&
             mz_zip_reader_extract_to_callback(&z, e->index, extract, &fd, 0);
        if (fd >= 0)
        {
            if (fsync(fd))
                ok = false;
            if (close(fd))
                ok = false;
        }
        unsigned char hash[32];
        uint64_t actual;
        ok = ok && mt_hash(t->source, hash, &actual) && actual == e->size &&
             (!e->listed || !memcmp(hash, e->hash, 32)) && mt_sync_parent(t->source);
        ++at;
        if (observer && observer->progress)
            observer->progress(observer->context, MT_PREPARING, at, files);
    }
    if (!ok)
    {
        why = "Could not stage and verify every update file";
        goto finish;
    }
    /* All bytes and CRCs checked. The running executable is the last rename. */
    for (uint32_t i = 0; i < files; ++i)
        if (!strcmp(targets[i].target + strlen(root) + 1, "eboot.bin"))
        {
            MtTarget executable = targets[i];
            memmove(targets + i, targets + i + 1, (files - i - 1) * sizeof(*targets));
            targets[files - 1] = executable;
            break;
        }
    Observe observed = {observer, false};
    MtObserver tx = {cancelled, progress, &observed};
    ok = mt_transaction(journal, targets, files, &tx, error, es);
    restart = ok || observed.committed;
    if (!ok)
    {
        why = "Update failed; original installation restored. Try again";
        if (!mt_recover(journal, &scope, 1, NULL, error, es))
        {
            maintenance_recovery_finish(MAINT_RECOVER_UPDATE, false);
            workspace = false; /* preserve authoritative evidence, never clean failed recovery */
            why = "Update recovery pending; retry recovery before playing";
        }
        else
        {
            /* Recovery after the durable commit only retries cleanup; the new
             * installation remains installed and still requires a restart. */
            ok = observed.committed;
        }
    }
finish:
    if (z.m_pState)
        mz_zip_reader_end(&z);
    free(entries);
    free(targets);
    if (workspace && !clear(work))
    {
        ok = false;
        maintenance_recovery_finish(MAINT_RECOVER_UPDATE, false);
        why = "Update cleanup pending; retry recovery before playing";
    }
    maintenance_release(MAINT_UPDATE, restart);
    return ok ? true : fail(error, es, why);
}
