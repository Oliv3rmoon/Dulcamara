/*
 * Dulcamara - a link's ZIP checked, unpacked and installed (links.h, docs/game-links.md). Every entry is checked
 * before anything is written; members are inflated through a callback in pieces, so neither the archive nor a member
 * is held in memory; the game is unpacked into a hidden staging folder and renamed into games/ under a name nothing has.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if !defined(__PROSPERO__)
#define _XOPEN_SOURCE 700
#endif
#define _DARWIN_C_SOURCE 1
#include "link_internal.h"
#include "maintenance.h"

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
#if defined(__PROSPERO__)
#include <sys/mount.h>
#endif
#include <unistd.h>

#define NAME_MAX_BYTES 255
#define CENTRAL_MAX (64u << 20) /* the central directory miniz reads into memory */
#define PROGRESS_EVERY (4ull << 20)

typedef struct
{
    char *name; /* relative to the game's folder (the top folder taken off), '/'-separated */
    mz_uint index;
    uint64_t size, packed, begin, end;
    uint32_t crc;
    bool directory, skip;
} Entry;

typedef struct
{
    int fd;
    const LinkObserver *observer;
    uint64_t written, limit;
    uint64_t *unpacked, total, next_report;
    bool too_big, cancelled, no_space, io;
} Out;

static size_t read_at(void *opaque, mz_uint64 offset, void *buffer, size_t n)
{
    const int fd = *(const int *)opaque;
    size_t at = 0;
    while (at < n)
    {
        const ssize_t got = pread(fd, (char *)buffer + at, n - at, (off_t)(offset + at));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        at += (size_t)got;
    }
    return at;
}

static uint32_t le16(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
static uint32_t le32(const unsigned char *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}
static uint64_t le64(const unsigned char *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* The entry count and central directory size the end records declare, read before miniz allocates for them: a bomb
 * by count is refused without reading its directory. False when no end record is found (not a ZIP). */
static bool declared(int fd, uint64_t size, uint64_t *count, uint64_t *central)
{
    unsigned char tail[22 + 65535];
    const size_t n = size < sizeof(tail) ? (size_t)size : sizeof(tail);
    if (n < 22 || read_at(&fd, size - n, tail, n) != n)
        return false;
    for (size_t i = n - 22 + 1; i-- > 0;)
    {
        if (memcmp(tail + i, "PK\005\006", 4))
            continue;
        *count = le16(tail + i + 10);
        *central = le32(tail + i + 12);
        const uint64_t at = size - n + i;
        unsigned char locator[20], end64[56];
        if (at >= 20 && read_at(&fd, at - 20, locator, 20) == 20 && !memcmp(locator, "PK\006\007", 4) &&
            le64(locator + 8) <= size - 56 && read_at(&fd, le64(locator + 8), end64, 56) == 56 &&
            !memcmp(end64, "PK\006\006", 4))
        {
            *count = le64(end64 + 32);
            *central = le64(end64 + 40);
        }
        return true;
    }
    return false;
}

static bool add_to(uint64_t *total, uint64_t n)
{
    if (UINT64_MAX - *total < n)
        return false;
    *total += n;
    return true;
}

/* Where an entry's bytes are in the archive (update_storage.c's check): overlapping entries are a bomb's shape. */
static bool extent(mz_zip_archive *z, const mz_zip_archive_file_stat *s, Entry *e)
{
    unsigned char h[30];
    e->begin = s->m_local_header_ofs;
    e->end = e->begin;
    if (e->begin >= z->m_central_directory_file_ofs || z->m_pRead(z->m_pIO_opaque, e->begin, h, sizeof(h)) != 30 ||
        memcmp(h, "PK\003\004", 4))
        return false;
    const uint64_t header = 30u + le16(h + 26) + le16(h + 28);
    if (!add_to(&e->end, header) || !add_to(&e->end, s->m_comp_size))
        return false;
    if (h[6] & 8)
    {
        unsigned char signature[4];
        if (z->m_pRead(z->m_pIO_opaque, e->end, signature, 4) != 4)
            return false;
        uint64_t descriptor = (s->m_comp_size >= UINT32_MAX || s->m_uncomp_size >= UINT32_MAX) ? 20 : 12;
        if (!memcmp(signature, "PK\007\010", 4))
            descriptor += 4;
        if (!add_to(&e->end, descriptor))
            return false;
    }
    return e->end <= z->m_central_directory_file_ofs;
}

static int by_begin(const void *a, const void *b)
{
    const uint64_t x = ((const Entry *)a)->begin, y = ((const Entry *)b)->begin;
    return x < y ? -1 : x > y;
}

/* by name, ignoring case, with '/' before every other character: a folder's entries come straight after it */
static int name_order(const char *a, const char *b)
{
    for (;; ++a, ++b)
    {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        x = x == '/' ? 1 : x >= 'A' && x <= 'Z' ? x + 32 : x;
        y = y == '/' ? 1 : y >= 'A' && y <= 'Z' ? y + 32 : y;
        if (x != y || !x)
            return x - y;
    }
}

static int by_name(const void *a, const void *b)
{
    return name_order(((const Entry *)a)->name, ((const Entry *)b)->name);
}

/* A name an entry may have: relative, no "." or ".." or empty parts, no backslash, colon or control character, each
 * part under 256 bytes. why says what is wrong. */
static bool name_safe(const char *name, const char **why)
{
    if (name[0] == '/')
        return *why = "an absolute path", false;
    for (const unsigned char *c = (const unsigned char *)name; *c; ++c)
    {
        if (*c == '\\')
            return *why = "a backslash in a name", false;
        if (*c == ':')
            return *why = "a drive letter or a colon in a name", false;
        if (*c < 0x20 || *c == 0x7f)
            return *why = "a control character in a name", false;
    }
    if (!mt_relative(name))
        return *why = "a \"..\" or an empty part in a path", false;
    for (const char *p = name; *p;)
    {
        const size_t part = strcspn(p, "/");
        if (part > NAME_MAX_BYTES)
            return *why = "a name over 255 bytes", false;
        p += part + (p[part] == '/');
    }
    return true;
}

/* A folder name from text: no '/', '\', ':' or control characters, no '.' or space at either end, at most
 * LINKS_FOLDER_MAX bytes on a whole UTF-8 character. */
static void folder_name(const char *text, char *out, size_t size)
{
    char clean[LINKS_FOLDER_MAX + 1];
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c && o < LINKS_FOLDER_MAX; ++c)
        clean[o++] = (*c < 0x20 || *c == 0x7f || *c == '/' || *c == '\\' || *c == ':' || *c == '"' || *c == '*' ||
                      *c == '?' || *c == '<' || *c == '>' || *c == '|')
                         ? '_'
                         : (char)*c;
    clean[o] = '\0';
    if (o == LINKS_FOLDER_MAX && ((unsigned char)clean[o - 1] & 0x80))
    {
        /* a lead byte left without all of its character */
        size_t start = o - 1;
        while (start > 0 && ((unsigned char)clean[start] & 0xC0) == 0x80)
            --start;
        const unsigned char lead = (unsigned char)clean[start];
        const size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (o - start < need)
            clean[start] = '\0';
    }
    char *p = clean;
    while (*p == '.' || *p == ' ')
        ++p;
    size_t n = strlen(p);
    while (n && (p[n - 1] == '.' || p[n - 1] == ' '))
        p[--n] = '\0';
    snprintf(out, size, "%s", n ? p : "Downloaded game");
}

static bool all_zero(const void *data, size_t n)
{
    static const unsigned char zeros[4096];
    const unsigned char *p = data;
    for (; n >= sizeof(zeros); p += sizeof(zeros), n -= sizeof(zeros))
        if (memcmp(p, zeros, sizeof(zeros)))
            return false;
    return !n || !memcmp(p, zeros, n);
}

static size_t on_bytes(void *context, mz_uint64 offset, const void *data, size_t n)
{
    Out *o = context;
    if (offset != o->written)
        return o->io = true, 0;
    if (n > o->limit - o->written)
        return o->too_big = true, 0; /* more than the entry declares: refused, never written */
    if (link_cancelled(o->observer))
        return o->cancelled = true, 0;
    if (n >= 4096 && all_zero(data, n))
    {
        /* a hole: what an all-zero piece is anyway, and no blocks for it (the size is set at the end) */
        if (lseek(o->fd, (off_t)n, SEEK_CUR) < 0)
            return o->io = true, 0;
    }
    else
        for (size_t at = 0; at < n;)
        {
            const ssize_t w = write(o->fd, (const char *)data + at, n - at);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
            {
                if (errno == ENOSPC || errno == EDQUOT || errno == EFBIG)
                    o->no_space = true;
                else
                    o->io = true;
                return 0;
            }
            at += (size_t)w;
        }
    o->written += n;
    *o->unpacked += n;
    if (*o->unpacked >= o->next_report)
    {
        o->next_report = *o->unpacked + PROGRESS_EVERY;
        link_progress(o->observer, LINK_STAGE_UNPACK, *o->unpacked, o->total);
    }
    return n;
}

/* every folder from base down to base/relative, made when missing; something else in the way is an error */
static bool make_parents(const char *base, const char *relative, bool last_too)
{
    char path[MT_PATH_MAX];
    const size_t start = strlen(base) + 1;
    if (snprintf(path, sizeof(path), "%s/%s", base, relative) >= (int)sizeof(path))
        return false;
    for (char *p = path + start;; ++p)
    {
        if (*p != '/' && *p)
            continue;
        if (!*p && !last_too)
            return true;
        const char c = *p;
        *p = '\0';
        struct stat st;
        if (mkdir(path, 0755) && (errno != EEXIST || lstat(path, &st) || !S_ISDIR(st.st_mode)))
            return false;
        if (!c)
            return true;
        *p = c;
    }
}

/* fsync each folder below and at path */
static bool sync_tree(const char *path, int depth)
{
    if (depth > 128)
        return false;
    DIR *d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    for (struct dirent *e; ok && (e = readdir(d));)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char child[MT_PATH_MAX];
        struct stat st;
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child) || lstat(child, &st))
            ok = false;
        else if (S_ISDIR(st.st_mode))
            ok = sync_tree(child, depth + 1);
    }
    closedir(d);
    return ok && mt_sync_dir(path);
}

static uint64_t free_bytes(const char *path)
{
#if defined(__PROSPERO__)
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct statfs fs;
    memset(&fs, 0, sizeof(fs));
    extern int link_unpack_ps5_fstatfs(int, struct statfs *) __asm__("_fstatfs");
    int rc = link_unpack_ps5_fstatfs(fd, &fs);
    close(fd);
    return rc == 0 && fs.f_bavail > 0 ? (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize : 0;
#else
    struct statvfs s;
    if (statvfs(path, &s))
        return 0;
    return (uint64_t)s.f_bavail * (s.f_frsize ? s.f_frsize : s.f_bsize);
#endif
}

bool link_unpack(const char *games, const char *staging, const char *zip, const char *tag, const char *name_hint,
                 const LinkObserver *observer, LinkResult *r)
{
    bool ok = false;
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    Entry *entries = NULL;
    mz_uint count = 0;
    char work[MT_PATH_MAX] = "", folder[LINKS_FOLDER_MAX + 1], stage[MT_PATH_MAX];
    bool made_work = false;

    link_progress(observer, LINK_STAGE_CHECK, 0, 0);
    const int fd = open(zip, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode))
    {
        link_fail(r, LINK_IO, "The download can't be read.");
        goto done;
    }
    const uint64_t archive_size = (uint64_t)st.st_size;
    uint64_t declared_count = 0, central = 0;
    if (!declared(fd, archive_size, &declared_count, &central))
    {
        link_fail(r, LINK_FORMAT, "The file is not a ZIP archive, or it is incomplete.");
        goto done;
    }
    if (declared_count > LINKS_MAX_ENTRIES || central > CENTRAL_MAX)
    {
        link_fail(r, LINK_UNSAFE, "Refused: the ZIP has more than %u entries (or a huge directory); it looks like a "
                                  "ZIP bomb.", LINKS_MAX_ENTRIES);
        goto done;
    }
    int io_fd = fd;
    z.m_pRead = read_at;
    z.m_pIO_opaque = &io_fd;
    if (!mz_zip_reader_init(&z, archive_size, 0))
    {
        const mz_zip_error e = mz_zip_get_last_error(&z);
        if (e == MZ_ZIP_UNSUPPORTED_MULTIDISK)
            link_fail(r, LINK_FORMAT, "This is one part of a split ZIP: Dulcamara needs a single .zip file.");
        else if (e == MZ_ZIP_NOT_AN_ARCHIVE)
            link_fail(r, LINK_FORMAT, "The file is not a ZIP archive.");
        else
            link_fail(r, LINK_DAMAGED, "The ZIP is damaged or incomplete (%s).", mz_zip_get_error_string(e));
        goto done;
    }
    count = mz_zip_reader_get_num_files(&z);
    if (!count || count > LINKS_MAX_ENTRIES)
    {
        link_fail(r, count ? LINK_UNSAFE : LINK_FORMAT,
                  count ? "Refused: the ZIP has more than 65536 entries; it looks like a ZIP bomb."
                        : "The ZIP is empty: there is no game in it.");
        goto done;
    }
    entries = calloc(count, sizeof(*entries));
    if (!entries)
    {
        link_fail(r, LINK_IO, "Not enough memory to check the ZIP.");
        goto done;
    }
    link_progress(observer, LINK_STAGE_CHECK, 0, count);
    uint64_t total = 0, packed = 0, files = 0;
    mz_uint kept = 0;
    for (mz_uint i = 0; i < count; ++i)
    {
        if ((i & 255) == 0 && link_cancelled(observer))
        {
            link_fail(r, LINK_CANCELLED, "Cancelled.");
            goto done;
        }
        mz_zip_archive_file_stat s;
        if (!mz_zip_reader_file_stat(&z, i, &s))
        {
            link_fail(r, LINK_DAMAGED, "The ZIP's directory is damaged.");
            goto done;
        }
        if (mz_zip_reader_get_filename(&z, i, NULL, 0) > sizeof(s.m_filename))
        {
            link_fail(r, LINK_UNSAFE, "Refused: an entry's name is longer than the paths Dulcamara uses.");
            goto done;
        }
        if (mz_zip_reader_get_filename(&z, i, NULL, 0) != strlen(s.m_filename) + 1)
        {
            /* a NUL inside the name would cut it short: the entry would be written under another name */
            link_fail(r, LINK_UNSAFE, "Refused for safety: a control character (NUL) in a name.");
            goto done;
        }
        if (s.m_is_encrypted)
        {
            link_fail(r, LINK_FORMAT, "The ZIP is encrypted (it has a password): unpack it on a PC instead.");
            goto done;
        }
        if (!s.m_is_supported)
        {
            link_fail(r, LINK_FORMAT,
                      "The ZIP uses a compression method Dulcamara can't unpack (only Store and Deflate): unpack it "
                      "on a PC and copy the game's folder over FTP.");
            goto done;
        }
        char *name = s.m_filename;
        size_t length = strlen(name);
        if (!strncmp(name, "__MACOSX/", 9) || !strcmp(name, "__MACOSX"))
            continue; /* a Mac's resource forks: not the game's */
        const char *why = NULL;
        if (s.m_is_directory && length && name[length - 1] == '/')
            name[--length] = '\0';
        if (!name_safe(name, &why))
        {
            /* shown as '?' where it has a control byte: the name cannot add a line to the log or the message */
            for (char *c = s.m_filename; *c; ++c)
                if ((unsigned char)*c < 0x20 || *c == 0x7f)
                    *c = '?';
            link_fail(r, LINK_UNSAFE, "Refused for safety: %s (\"%.80s\").", why, s.m_filename);
            goto done;
        }
        const unsigned type = (s.m_external_attr >> 16) & 0170000;
        if (type && type != (s.m_is_directory ? 0040000u : 0100000u))
        {
            link_fail(r, LINK_UNSAFE, "Refused for safety: \"%.80s\" is a link or a special file, not a file.",
                      name);
            goto done;
        }
        if ((s.m_is_directory && s.m_uncomp_size) || (s.m_method == 0 && s.m_comp_size != s.m_uncomp_size) ||
            (s.m_method == MZ_DEFLATED && s.m_uncomp_size / LINKS_DEFLATE_RATIO > s.m_comp_size + 1))
        {
            link_fail(r, LINK_UNSAFE, "Refused: \"%.80s\" declares sizes no real file has (a ZIP bomb?).", name);
            goto done;
        }
        Entry *e = &entries[kept];
        if (!extent(&z, &s, e))
        {
            link_fail(r, LINK_DAMAGED, "The ZIP is damaged: \"%.80s\" is not where its directory says.", name);
            goto done;
        }
        e->name = strdup(name);
        if (!e->name)
        {
            link_fail(r, LINK_IO, "Not enough memory to check the ZIP.");
            goto done;
        }
        e->index = i;
        e->size = s.m_uncomp_size;
        e->packed = s.m_comp_size;
        e->crc = s.m_crc32;
        e->directory = s.m_is_directory;
        ++kept;
        if (!e->directory)
            ++files;
        if (!add_to(&total, s.m_uncomp_size) || !add_to(&packed, s.m_comp_size))
        {
            link_fail(r, LINK_UNSAFE, "Refused: the ZIP's sizes overflow.");
            goto done;
        }
    }
    if (!kept)
    {
        link_fail(r, LINK_FORMAT, "The ZIP is empty: there is no game in it.");
        goto done;
    }
    /* no two entries' bytes may overlap */
    qsort(entries, kept, sizeof(*entries), by_begin);
    for (mz_uint i = 1; i < kept; ++i)
        if (entries[i].begin < entries[i - 1].end)
        {
            link_fail(r, LINK_UNSAFE, "Refused: entries in the ZIP share their data (a ZIP bomb's shape).");
            goto done;
        }
    if (total > LINKS_MAX_TOTAL || (total > LINKS_RATIO_FLOOR && total / LINKS_MAX_RATIO > packed))
    {
        link_fail(r, LINK_UNSAFE,
                  "Refused: the ZIP would unpack to %.1f GB from %.1f MB (over %u times its size, or over 64 GB); it "
                  "looks like a ZIP bomb.",
                  (double)total / 1e9, (double)packed / 1048576.0, LINKS_MAX_RATIO);
        goto done;
    }

    /* the folder: the archive's single top folder, else the file's name */
    const size_t top = strcspn(entries[0].name, "/");
    bool single = true, inside = false;
    for (mz_uint i = 0; i < kept && single; ++i)
    {
        const char *n = entries[i].name;
        if (strncmp(n, entries[0].name, top) || (n[top] != '/' && n[top] != '\0') || (!n[top] && !entries[i].directory))
            single = false;
        else if (n[top] == '/')
            inside = true;
    }
    if (single && inside)
    {
        char first[NAME_MAX_BYTES + 1];
        snprintf(first, sizeof(first), "%.*s", (int)top, entries[0].name);
        folder_name(first, folder, sizeof(folder));
        for (mz_uint i = 0; i < kept; ++i)
        {
            char *n = entries[i].name;
            if (!n[top])
                entries[i].skip = true; /* the top folder itself */
            else
                memmove(n, n + top + 1, strlen(n + top + 1) + 1);
        }
    }
    else
    {
        char hint[256];
        snprintf(hint, sizeof(hint), "%s", name_hint && name_hint[0] ? name_hint : "");
        const char *dot = strrchr(hint, '.');
        if (dot && (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".easyrpg")))
            hint[dot - hint] = '\0';
        folder_name(hint, folder, sizeof(folder));
    }

    /* the same name twice must be the same entry; a file can't also be a folder */
    qsort(entries, kept, sizeof(*entries), by_name);
    const Entry *a = NULL; /* the last entry kept */
    for (mz_uint i = 0; i < kept; ++i)
    {
        Entry *b = &entries[i];
        if (b->skip)
            continue;
        if (a && !name_order(a->name, b->name))
        {
            if (strcmp(a->name, b->name))
            {
                link_fail(r, LINK_UNSAFE, "Refused: \"%.80s\" and \"%.80s\" differ only in case.", a->name, b->name);
                goto done;
            }
            if (a->directory && b->directory)
                b->skip = true;
            else if (!a->directory && !b->directory && a->size == b->size && a->crc == b->crc)
                b->skip = true, --files;
            else
            {
                link_fail(r, LINK_UNSAFE, "Refused: two different entries are named \"%.80s\".", b->name);
                goto done;
            }
            continue;
        }
        if (a && !a->directory)
        {
            const size_t n = strlen(a->name);
            if (!strncasecmp(a->name, b->name, n) && b->name[n] == '/')
            {
                link_fail(r, LINK_UNSAFE, "Refused: \"%.80s\" is both a file and a folder.", a->name);
                goto done;
            }
        }
        a = b;
    }
    /* each installed path fits the app's paths, with room for " (99)" */
    const size_t base = strlen(games) + 1 + strlen(folder) + 5 + 1;
    for (mz_uint i = 0; i < kept; ++i)
        if (!entries[i].skip && base + strlen(entries[i].name) >= NP_PATH_MAX)
        {
            link_fail(r, LINK_UNSAFE, "Refused: \"%.80s...\" would have a path longer than Dulcamara's %d bytes.",
                      entries[i].name, NP_PATH_MAX);
            goto done;
        }
    const uint64_t needed = total + files * LINKS_ENTRY_OVERHEAD + LINKS_RESERVE;
    if (!mt_space(games, needed))
    {
        link_fail(r, LINK_NO_SPACE, "Not enough free space to unpack: %.0f MB needed, %.0f MB free.",
                  (double)needed / 1048576.0, (double)free_bytes(games) / 1048576.0);
        goto done;
    }
    if (link_cancelled(observer))
    {
        link_fail(r, LINK_CANCELLED, "Cancelled.");
        goto done;
    }

    /* unpack into staging/unpack-<tag>/<folder> */
    if (snprintf(work, sizeof(work), "%s/unpack-%s", staging, tag) >= (int)sizeof(work) ||
        snprintf(stage, sizeof(stage), "%s/%s", work, folder) >= (int)sizeof(stage) || !link_remove_tree(work) ||
        mkdir(work, 0755) || (made_work = true, mkdir(stage, 0755)))
    {
        link_fail(r, LINK_IO, "Couldn't make the unpacking folder.");
        goto done;
    }
    uint64_t unpacked = 0;
    link_progress(observer, LINK_STAGE_UNPACK, 0, total);
    for (mz_uint i = 0; i < kept; ++i)
    {
        const Entry *e = &entries[i];
        if (e->skip)
            continue;
        if (link_cancelled(observer))
        {
            link_fail(r, LINK_CANCELLED, "Cancelled.");
            goto done;
        }
        if (!make_parents(stage, e->name, e->directory))
        {
            if (errno == ENOSPC || errno == EDQUOT)
                link_fail(r, LINK_NO_SPACE, "The storage is full: unpacking stopped. Free some space and try again.");
            else
                link_fail(r, LINK_IO, "Couldn't make the folder for \"%.80s\".", e->name);
            goto done;
        }
        if (e->directory)
            continue;
        char path[MT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", stage, e->name);
        Out o = {.observer = observer, .limit = e->size, .unpacked = &unpacked, .total = total,
                 .next_report = unpacked + PROGRESS_EVERY};
        o.fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        if (o.fd < 0)
        {
            if (errno == ENOSPC || errno == EDQUOT)
                link_fail(r, LINK_NO_SPACE, "The storage is full: unpacking stopped. Free some space and try again.");
            else
                link_fail(r, LINK_IO, "Couldn't write \"%.80s\".", e->name);
            goto done;
        }
        bool written = mz_zip_reader_extract_to_callback(&z, e->index, on_bytes, &o, 0);
        const mz_zip_error error = written ? MZ_ZIP_NO_ERROR : mz_zip_get_last_error(&z);
        if (written && (o.written != e->size || ftruncate(o.fd, (off_t)o.written)))
            written = false, o.io = o.io || o.written == e->size;
        if (written && fsync(o.fd) && errno != ENOSYS && errno != EINVAL && errno != ENOTSUP)
            written = false, o.no_space = errno == ENOSPC || errno == EDQUOT, o.io = !o.no_space;
        if (close(o.fd) && written)
            written = false, o.no_space = errno == ENOSPC || errno == EDQUOT, o.io = !o.no_space;
        if (!written)
        {
            if (o.cancelled)
                link_fail(r, LINK_CANCELLED, "Cancelled.");
            else if (o.no_space)
                link_fail(r, LINK_NO_SPACE, "The storage is full: unpacking stopped. Free some space and try again.");
            else if (o.too_big)
                link_fail(r, LINK_UNSAFE, "Refused: \"%.80s\" holds more data than it declares (a ZIP bomb?).",
                          e->name);
            else if (o.io)
                link_fail(r, LINK_IO, "Couldn't write \"%.80s\".", e->name);
            else
                link_fail(r, LINK_DAMAGED, "The ZIP is damaged: \"%.80s\" can't be unpacked (%s).", e->name,
                          mz_zip_get_error_string(error));
            goto done;
        }
        link_progress(observer, LINK_STAGE_UNPACK, unpacked, total);
    }
    if (!sync_tree(stage, 0))
    {
        link_fail(r, LINK_IO, "Couldn't finish writing the game.");
        goto done;
    }

    /* into games/, under a name nothing has: rename never merges into, nor replaces, a folder with files in it */
    link_progress(observer, LINK_STAGE_INSTALL, 0, 1);
    if (link_cancelled(observer))
    {
        link_fail(r, LINK_CANCELLED, "Cancelled.");
        goto done;
    }
    char target[NP_PATH_MAX];
    bool placed = false;
    for (int k = 1; k <= LINKS_SUFFIX_MAX && !placed; ++k)
    {
        if (k == 1)
            snprintf(target, sizeof(target), "%s/%s", games, folder);
        else
            snprintf(target, sizeof(target), "%s/%s (%d)", games, folder, k);
        if (!lstat(target, &st) || errno != ENOENT)
            continue;
        if (!rename(stage, target))
            placed = true;
        else if (errno != EEXIST && errno != ENOTEMPTY)
            break;
    }
    if (!placed)
    {
        link_fail(r, LINK_IO, "Couldn't put the game in the games folder as \"%s\".", folder);
        goto done;
    }
    mt_sync_dir(games);
    link_progress(observer, LINK_STAGE_INSTALL, 1, 1);
    str_copy(r->folder, sizeof(r->folder), target);
    r->error = LINK_OK;
    r->message[0] = '\0';
    ok = true;
done:
    if (z.m_pState)
        mz_zip_reader_end(&z);
    if (fd >= 0)
        close(fd);
    for (mz_uint i = 0; entries && i < count; ++i)
        free(entries[i].name);
    free(entries);
    if (made_work && !link_remove_tree(work))
        np_log("links: couldn't remove %s (the next start does)", work);
    return ok;
}
