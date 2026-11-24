/* SPDX-License-Identifier: GPL-3.0-or-later */
#if !defined(__PROSPERO__)
#define _XOPEN_SOURCE 700
#endif
#define _DARWIN_C_SOURCE 1
#include "maintenance.h"
#include "../examples/self-update/self_update_sha256.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#if defined(__PROSPERO__)
#include <sys/mount.h>
extern int mt_ps5_fstatfs(int, struct statfs *) __asm__("_fstatfs");
#endif
#include <unistd.h>

static bool fsync_supported_ok(int fd)
{
    if (fsync(fd) == 0)
        return true;
    return errno == ENOSYS || errno == EINVAL || errno == ENOTSUP;
}

/* The SDK exports this BSD API but hides its declaration under _XOPEN_SOURCE. */
extern void arc4random_buf(void *, size_t);

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static unsigned recovery_required = 1, recovery_done;
static bool runtime_owned, runtime_quiet, runtime_unsafe, needs_restart;
static MaintenanceOwner owner;
void maintenance_recovery_begin(unsigned required)
{
    pthread_mutex_lock(&gate);
    recovery_required = required;
    recovery_done = 0;
    pthread_mutex_unlock(&gate);
}
void maintenance_recovery_finish(unsigned part, bool ok)
{
    pthread_mutex_lock(&gate);
    if (ok)
        recovery_done |= part;
    else
        recovery_done &= ~part;
    pthread_mutex_unlock(&gate);
}
void maintenance_recovered(bool ok)
{
    maintenance_recovery_finish(MAINT_RECOVER_SAVES, ok);
}
static bool recovered(void)
{
    return recovery_required && (recovery_done & recovery_required) == recovery_required;
}
bool maintenance_launch(void)
{
    pthread_mutex_lock(&gate);
    bool ok =
        recovered() && !runtime_owned && !runtime_unsafe && !needs_restart && owner == MAINT_NONE;
    if (ok)
        runtime_owned = true;
    pthread_mutex_unlock(&gate);
    return ok;
}
void maintenance_runtime(bool owned, bool quiet, bool unsafe)
{
    pthread_mutex_lock(&gate);
    runtime_owned = owned;
    runtime_quiet = quiet;
    runtime_unsafe |= unsafe;
    pthread_mutex_unlock(&gate);
}
bool maintenance_acquire(MaintenanceOwner next)
{
    pthread_mutex_lock(&gate);
    bool ok = next > MAINT_NONE && next <= MAINT_LINKS && owner == MAINT_NONE &&
              !runtime_unsafe && (!needs_restart || next == MAINT_RECOVERY) &&
              (recovered() || next == MAINT_RECOVERY) &&
              (!runtime_owned || (next == MAINT_SNAPSHOT && runtime_quiet));
    if (ok)
        owner = next;
    pthread_mutex_unlock(&gate);
    return ok;
}
void maintenance_release(MaintenanceOwner previous, bool restart)
{
    pthread_mutex_lock(&gate);
    if (owner == previous)
    {
        owner = MAINT_NONE;
        needs_restart |= restart;
    }
    pthread_mutex_unlock(&gate);
}
bool maintenance_ready(void)
{
    pthread_mutex_lock(&gate);
    bool ok =
        recovered() && !runtime_owned && !runtime_unsafe && !needs_restart && owner == MAINT_NONE;
    pthread_mutex_unlock(&gate);
    return ok;
}
bool maintenance_restart_ready(void)
{
    pthread_mutex_lock(&gate);
    bool ok = recovered() && !runtime_owned && !runtime_unsafe && owner == MAINT_NONE;
    pthread_mutex_unlock(&gate);
    return ok;
}
bool mt_cancelled(const MtObserver *o)
{
    return o && o->cancelled && o->cancelled(o->context);
}
static void progress(const MtObserver *o, MtPhase phase, uint64_t n, uint64_t total)
{
    if (o && o->progress)
        o->progress(o->context, phase, n, total);
}
bool mt_relative(const char *p)
{
    if (!p || !p[0] || p[0] == '/')
        return false;
    for (const char *s = p; *s;)
    {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return false;
        if (!e)
            return true;
        s = e + 1;
    }
    return false;
}
bool mt_path(char out[MT_PATH_MAX], const char *base, const char *rel)
{
    if (!base || !rel || !mt_relative(rel))
        return false;
    int n = snprintf(out, MT_PATH_MAX, "%s/%s", base, rel);
    return n > 0 && n < MT_PATH_MAX;
}
/* Resolve existing ancestors, preserve nonexistent leaf spelling. */
bool mt_canonical(const char *path, char out[MT_PATH_MAX])
{
    if (!path || path[0] != '/' || strlen(path) >= MT_PATH_MAX)
        return false;
    char current[MT_PATH_MAX];
    strcpy(current, path);
    char suffix[MT_PATH_MAX] = "";
    for (;;)
    {
        char resolved[4096];
        if (realpath(current, resolved))
        {
            int n = snprintf(out, MT_PATH_MAX, "%s%s", resolved, suffix);
            return n > 1 && n < MT_PATH_MAX;
        }
        if (errno != ENOENT)
            return false;
        char *slash = strrchr(current, '/');
        if (!slash || !slash[1])
            return false;
        if (!strcmp(slash + 1, ".") || !strcmp(slash + 1, ".."))
            return false;
        char next[MT_PATH_MAX];
        int n = snprintf(next, sizeof(next), "/%s%s", slash + 1, suffix);
        if (n < 0 || n >= MT_PATH_MAX)
            return false;
        strcpy(suffix, next);
        if (slash == current)
            current[1] = 0;
        else
            *slash = 0;
    }
}
static bool canonical_exact(const char *p)
{
    char c[MT_PATH_MAX];
    return mt_canonical(p, c) && !strcmp(p, c);
}
bool mt_sync_dir(const char *p)
{
    int fd = open(p, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return false;
    bool ok = fsync_supported_ok(fd);
    int e = errno;
    if (close(fd))
        ok = false;
    errno = e;
    return ok;
}
bool mt_sync_parent(const char *p)
{
    char b[MT_PATH_MAX];
    if (strlen(p) >= sizeof(b))
        return false;
    strcpy(b, p);
    char *s = strrchr(b, '/');
    if (!s)
        return false;
    if (s == b)
        s[1] = 0;
    else
        *s = 0;
    return mt_sync_dir(b);
}
bool mt_mkdirs(const char *path)
{
    if (!canonical_exact(path))
        return false;
    char b[MT_PATH_MAX];
    strcpy(b, path);
    for (char *p = b + 1;; ++p)
    {
        if (*p != '/' && *p)
            continue;
        char c = *p;
        *p = 0;
        struct stat st;
        if (lstat(b, &st))
        {
            if (errno != ENOENT || mkdir(b, 0700) || !mt_sync_parent(b))
                return false;
        }
        else if (!S_ISDIR(st.st_mode))
            return false;
        *p = c;
        if (!c)
            break;
    }
    return true;
}
static bool parent_dirs(const char *p)
{
    char b[MT_PATH_MAX];
    if (strlen(p) >= sizeof(b))
        return false;
    strcpy(b, p);
    char *s = strrchr(b, '/');
    if (!s || s == b)
        return false;
    *s = 0;
    return mt_mkdirs(b);
}
bool mt_hash(const char *p, unsigned char digest[32], uint64_t *size)
{
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode);
    self_update_sha256 h;
    self_update_sha256_start(&h);
    uint64_t total = 0;
    unsigned char b[65536];
    ssize_t n = 0;
    while (ok && (n = read(fd, b, sizeof(b))) > 0)
    {
        if (UINT64_MAX - total < (uint64_t)n)
        {
            ok = false;
            break;
        }
        total += (uint64_t)n;
        self_update_sha256_add(&h, b, (size_t)n);
    }
    ok = ok && n == 0;
    if (close(fd))
        ok = false;
    if (ok)
    {
        self_update_sha256_finish(&h, digest);
        if (size)
            *size = total;
    }
    return ok;
}
bool mt_copy(const char *src, const char *dst, const MtObserver *o)
{
    if (!canonical_exact(src) || !canonical_exact(dst) || !parent_dirs(dst))
        return false;
    int in = open(src, O_RDONLY | O_NOFOLLOW);
    if (in < 0)
        return false;
    struct stat st;
    bool ok = !fstat(in, &st) && S_ISREG(st.st_mode);
    int out = ok ? open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600) : -1;
    if (out < 0)
        ok = false;
    unsigned char b[65536];
    ssize_t n = 0;
    while (ok && !mt_cancelled(o) && (n = read(in, b, sizeof(b))) > 0)
    {
        ssize_t at = 0;
        while (at < n)
        {
            ssize_t w = write(out, b + at, (size_t)(n - at));
            if (w <= 0)
            {
                ok = false;
                break;
            }
            at += w;
        }
    }
    ok = ok && n == 0 && !mt_cancelled(o);
    if (out >= 0)
    {
        if (!fsync_supported_ok(out))
            ok = false;
        if (close(out))
            ok = false;
    }
    if (close(in))
        ok = false;
    if (ok)
    {
        unsigned char a[32], bhash[32];
        uint64_t as, bs;
        ok = mt_hash(src, a, &as) && mt_hash(dst, bhash, &bs) && as == bs &&
             !memcmp(a, bhash, 32) && mt_sync_parent(dst);
    }
    if (!ok && out >= 0)
        unlink(dst);
    return ok;
}
bool mt_space(const char *path, uint64_t bytes)
{
    char b[MT_PATH_MAX];
    if (!mt_canonical(path, b))
        return false;
#if defined(__PROSPERO__)
    for (;;)
    {
        int fd = open(b, O_RDONLY);
        if (fd >= 0)
        {
            struct statfs fs;
            memset(&fs, 0, sizeof(fs));
            int rc = mt_ps5_fstatfs(fd, &fs);
            int e = errno;
            close(fd);
            if (rc == 0)
            {
                uint64_t block = (uint64_t)fs.f_bsize;
                if (!block)
                    return false;
                if (fs.f_bavail <= 0)
                    return bytes == 0;
                return bytes / block + (bytes % block != 0) <= (uint64_t)fs.f_bavail;
            }
            errno = e;
            return e == ENOSYS || e == ENOTSUP;
        }
        if (errno != ENOENT)
            return false;
        char *p = strrchr(b, '/');
        if (!p || p == b)
            return false;
        *p = 0;
    }
#else
    struct statvfs s;
    while (statvfs(b, &s))
    {
        if (errno == ENOSYS || errno == ENOTSUP)
            return true;
        if (errno != ENOENT)
            return false;
        char *p = strrchr(b, '/');
        if (!p || p == b)
            return false;
        *p = 0;
    }
    uint64_t block = s.f_frsize ? s.f_frsize : s.f_bsize;
    if (!block)
        return false;
    return bytes / block + (bytes % block != 0) <= (uint64_t)s.f_bavail;
#endif
}
bool mt_atomic(const char *path, const void *data, size_t size)
{
    if (!canonical_exact(path) || !parent_dirs(path))
        return false;
    char temp[MT_PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    int n =
        snprintf(temp, sizeof(temp), "%.*s/.%s.tmp.XXXXXX", (int)(slash - path), path, slash + 1);
    if (n < 0 || n >= MT_PATH_MAX)
        return false;
    int fd = mkstemp(temp);
    if (fd < 0)
        return false;
    const unsigned char *p = data;
    size_t at = 0;
    bool ok = true;
    while (at < size)
    {
        ssize_t w = write(fd, p + at, size - at);
        if (w <= 0)
        {
            ok = false;
            break;
        }
        at += (size_t)w;
    }
    if (!fsync_supported_ok(fd))
        ok = false;
    if (close(fd))
        ok = false;
    if (ok)
        ok = !rename(temp, path) && mt_sync_parent(path);
    if (!ok)
        unlink(temp);
    return ok;
}
static bool below(const char *root, const char *p)
{
    size_t n = strlen(root);
    return !strncmp(root, p, n) && (p[n] == 0 || p[n] == '/');
}
static bool renpy_name(const char *relative, const char *suffix)
{
    size_t length = strlen(relative), suffix_length = strlen(suffix);
    if (!strcmp(relative, "persistent") ||
        (length >= suffix_length && !strcmp(relative + length - suffix_length, suffix)))
        return true;
    char base[MT_PATH_MAX];
    if (length >= sizeof(base))
        return false;
    strcpy(base, relative);
    if (length > 4 && !strcmp(base + length - 4, ".new"))
        base[length - 4] = 0;
    else if (length > 5 && !strcmp(base + length - 4, ".tmp"))
    {
        base[length - 4] = 0;
        char *dot = strrchr(base, '.');
        if (!dot || !dot[1])
            return false;
        for (char *p = dot + 1; *p; ++p)
            if (*p < '0' || *p > '9')
                return false;
        *dot = 0;
    }
    else
        return false;
    length = strlen(base);
    return !strcmp(base, "persistent") ||
           (length >= suffix_length && !strcmp(base + length - suffix_length, suffix));
}
bool mt_scope_contains(const struct dr_save_entry *s, const char *target)
{
    if (!dr_save_entry_valid(s) || !canonical_exact(s->path) || !canonical_exact(target) ||
        !strcmp(s->path, "/"))
        return false;
    const char *home = getenv("HOME");
    if (home && below(s->path, home) && s->kind != DR_SAVE_FILE)
        return false;
    if (s->kind == DR_SAVE_FILE)
        return !strcmp(s->path, target);
    if (s->kind == DR_SAVE_RENPY_FILE)
    {
        size_t base = strlen(s->path);
        if (strncmp(s->path, target, base))
            return false;
        const char *tail = target + base;
        if (!*tail || !strcmp(tail, ".new"))
            return true;
        if (*tail++ != '.' || *tail < '0' || *tail > '9')
            return false;
        do
        {
            ++tail;
        } while (*tail >= '0' && *tail <= '9');
        return !strcmp(tail, ".tmp");
    }
    size_t n = strlen(s->path);
    if (!below(s->path, target) || target[n] != '/')
        return false;
    const char *rel = target + n + 1;
    if (!mt_relative(rel))
        return false;
    if (s->kind == DR_SAVE_DIRECTORY)
        return true;
    if (s->kind == DR_SAVE_RENPY_SLOTS)
        return !strchr(rel, '/') && renpy_name(rel, s->suffix);
    size_t len = strlen(rel);
    return !strchr(rel, '/') && len >= 8 && !strncasecmp(rel, "Save", 4) &&
           !strcasecmp(rel + len - 4, ".lsd");
}

typedef struct
{
    MtTarget target;
    uint32_t existed, removal, old_mode, reserved;
    uint64_t old_size, new_size;
    unsigned char old_hash[32], new_hash[32];
} Record;
typedef struct
{
    uint32_t magic, version, count, reserved;
    unsigned char digest[32];
} Journal;
static bool fail(char *e, size_t n, const char *s)
{
    if (e && n)
        snprintf(e, n, "%s (%s)", s, strerror(errno));
    return false;
}
static bool same(const char *p, const unsigned char *h, uint64_t size)
{
    unsigned char d[32];
    uint64_t n;
    return mt_hash(p, d, &n) && n == size && !memcmp(h, d, 32);
}
static bool artifact(char out[MT_PATH_MAX], const char *dir, uint32_t i, const char *ext)
{
    char rel[64];
    snprintf(rel, sizeof(rel), "%u.%s", i, ext);
    return mt_path(out, dir, rel);
}
static bool staged(char out[MT_PATH_MAX], const char *target)
{
    int n = snprintf(out, MT_PATH_MAX, "%s.eclipse-stage", target);
    return n > 0 && n < MT_PATH_MAX;
}
/* Stage names must not alias another destination (or a source), in either order.
 * Also reject case aliases conservatively on filesystems with insensitive lookup. */
static bool disjoint(const MtTarget *a, const MtTarget *b)
{
    char as[MT_PATH_MAX], bs[MT_PATH_MAX];
    if (!staged(as, a->target) || !staged(bs, b->target))
        return false;
    return strcasecmp(a->target, b->target) && strcasecmp(as, b->target) &&
           strcasecmp(bs, a->target) && strcasecmp(as, b->source) && strcasecmp(bs, a->source) &&
           !below(a->target, b->target) && !below(b->target, a->target);
}
/* Filesystem collation guard. Its checked plan is durable before any probe exists.
 * Probes contain only owned zero-byte name placeholders, never target bytes. */
typedef struct
{
    uint32_t magic, version, before_staging, reserved;
    unsigned char nonce[16], records[32], digest[32];
} ProbePlan;
typedef struct NameProbe
{
    struct NameProbe *next;
    dev_t device;
    ino_t inode;
    char parent[MT_PATH_MAX], path[MT_PATH_MAX];
    uint32_t *indices, count, capacity;
} NameProbe;
static void bytes_hash(const void *bytes, size_t size, unsigned char hash[32])
{
    self_update_sha256 h;
    self_update_sha256_start(&h);
    self_update_sha256_add(&h, bytes, size);
    self_update_sha256_finish(&h, hash);
}
static void free_probes(NameProbe *p)
{
    while (p)
    {
        NameProbe *next = p->next;
        free(p->indices);
        free(p);
        p = next;
    }
}
static NameProbe *probe_parent(NameProbe *p, const char *target)
{
    char parent[MT_PATH_MAX];
    strcpy(parent, target);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return NULL;
    *slash = 0;
    struct stat st;
    if (stat(parent, &st) || !S_ISDIR(st.st_mode))
        return NULL;
    for (; p; p = p->next)
        if (p->device == st.st_dev && p->inode == st.st_ino)
            return p;
    return NULL;
}
static bool probe_paths(const Record *r, uint32_t count, const ProbePlan *plan, NameProbe **out)
{
    *out = NULL;
    char leaf[64] = ".eclipse-names-";
    size_t at = strlen(leaf);
    for (size_t i = 0; i < sizeof(plan->nonce); ++i)
    {
        snprintf(leaf + at, 3, "%02x", plan->nonce[i]);
        at += 2;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        NameProbe *p = probe_parent(*out, r[i].target.target);
        if (!p)
        {
            char parent[MT_PATH_MAX];
            strcpy(parent, r[i].target.target);
            char *slash = strrchr(parent, '/');
            if (!slash || slash == parent)
                return false;
            *slash = 0;
            struct stat st;
            if (stat(parent, &st) || !S_ISDIR(st.st_mode))
                return false;
            p = calloc(1, sizeof(*p));
            if (!p)
                return false;
            strcpy(p->parent, parent);
            p->device = st.st_dev;
            p->inode = st.st_ino;
            if (!mt_path(p->path, parent, leaf))
            {
                free(p);
                return false;
            }
            p->next = *out;
            *out = p;
        }
        if (p->count == p->capacity)
        {
            uint32_t capacity = p->capacity ? p->capacity * 2 : 16;
            uint32_t *indices = realloc(p->indices, capacity * sizeof(*indices));
            if (!indices)
                return false;
            p->indices = indices;
            p->capacity = capacity;
        }
        p->indices[p->count++] = i;
    }
    return true;
}
static bool probe_placeholder(const NameProbe *probe, const Record *r, const char *name)
{
    char file[MT_PATH_MAX];
    struct stat entry;
    if (!mt_path(file, probe->path, name) || lstat(file, &entry))
        return false;
    /* Exact spelling is the common fast path. Physical identity handles filesystems
     * that normalize returned names without relying on any Unicode mapping table. */
    for (unsigned physical = 0; physical < 2; ++physical)
        for (uint32_t at = 0; at < probe->count; ++at)
            for (unsigned kind = 0; kind < 2; ++kind)
            {
                uint32_t i = probe->indices[at];
                char stage[MT_PATH_MAX];
                const char *base = strrchr(r[i].target.target, '/') + 1;
                if (kind)
                {
                    if (!staged(stage, r[i].target.target))
                        return false;
                    base = strrchr(stage, '/') + 1;
                }
                if (!physical)
                {
                    if (!strcmp(base, name))
                        return true;
                    continue;
                }
                struct stat candidate;
                if (mt_path(file, probe->path, base) && !lstat(file, &candidate) &&
                    candidate.st_dev == entry.st_dev && candidate.st_ino == entry.st_ino)
                    return true;
            }
    return false;
}
static bool probe_marker(char path[MT_PATH_MAX], const NameProbe *probe, const ProbePlan *plan)
{
    char leaf[64] = ".owner-";
    size_t at = strlen(leaf);
    for (size_t i = 0; i < sizeof(plan->nonce); ++i)
    {
        snprintf(leaf + at, 3, "%02x", plan->nonce[i]);
        at += 2;
    }
    return mt_path(path, probe->path, leaf);
}
static bool remove_probe(const NameProbe *probe, const Record *r, const ProbePlan *plan)
{
    struct stat st;
    if (lstat(probe->path, &st))
        return errno == ENOENT && mt_sync_dir(probe->parent);
    if (!S_ISDIR(st.st_mode) || !canonical_exact(probe->path))
        return false;
    char marker[MT_PATH_MAX];
    if (!probe_marker(marker, probe, plan))
        return false;
    unsigned char owner_bytes[sizeof(*plan)];
    size_t length = 0;
    bool marker_exists = false;
    int fd = open(marker, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0)
    {
        marker_exists = true;
        bool valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 &&
                     (uint64_t)st.st_size <= sizeof(*plan);
        ssize_t got = valid ? read(fd, owner_bytes, sizeof(owner_bytes)) : -1;
        if (close(fd))
            valid = false;
        if (!valid || got < 0 || got != st.st_size || memcmp(owner_bytes, plan, (size_t)got))
            return false;
        length = (size_t)got;
    }
    else if (errno != ENOENT)
        return false;
    /* Empty first-creation directories and a partial matching owner marker have no
     * target data. Populated directories require the complete nonce-bound marker. */
    DIR *d = opendir(probe->path);
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
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            !strcmp(e->d_name, strrchr(marker, '/') + 1))
            continue;
        char file[MT_PATH_MAX];
        if (length != sizeof(*plan) || !probe_placeholder(probe, r, e->d_name) ||
            !mt_path(file, probe->path, e->d_name) || lstat(file, &st) || !S_ISREG(st.st_mode) ||
            st.st_size || st.st_nlink != 1 || unlink(file))
        {
            ok = false;
            break;
        }
    }
    if (closedir(d))
        ok = false;
    if (ok && marker_exists && unlink(marker))
        ok = false;
    return ok && !rmdir(probe->path) && mt_sync_dir(probe->parent);
}
static bool discard_probe_plan(const char *dir)
{
    char path[MT_PATH_MAX];
    return mt_path(path, dir, "probe-plan") && (!unlink(path) || errno == ENOENT) &&
           mt_sync_dir(dir);
}
static bool cleanup_probe_plan(const char *dir, const Record *r, uint32_t count, bool *had,
                               bool *unstaged, bool discard)
{
    *had = false;
    *unstaged = false;
    char path[MT_PATH_MAX];
    if (!mt_path(path, dir, "probe-plan"))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT;
    *had = true;
    ProbePlan plan;
    unsigned char digest[32], records[32];
    bool ok = fread(&plan, 1, sizeof(plan), f) == sizeof(plan) && fgetc(f) == EOF && !(ferror)(f);
    if (fclose(f))
        ok = false;
    if (!ok)
        return false;
    bytes_hash(&plan, offsetof(ProbePlan, digest), digest);
    bytes_hash(r, count * sizeof(*r), records);
    if (plan.magic != 0x45504e4du || plan.version != 1 || plan.before_staging > 1 ||
        plan.reserved || memcmp(plan.digest, digest, 32) || memcmp(plan.records, records, 32))
        return false;
    *unstaged = plan.before_staging != 0;
    NameProbe *probes = NULL;
    ok = probe_paths(r, count, &plan, &probes);
    for (NameProbe *p = probes; ok && p; p = p->next)
        ok = remove_probe(p, r, &plan);
    free_probes(probes);
    return ok && (!discard || discard_probe_plan(dir));
}
static bool filesystem_names_disjoint(const char *dir, const Record *r, uint32_t count,
                                      bool before_staging)
{
    ProbePlan plan = {.magic = 0x45504e4du, .version = 1, .before_staging = before_staging};
    NameProbe *probes = NULL;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; ++i)
        ok = parent_dirs(r[i].target.target);
    if (!ok)
        return false;
    /* Pick only absent exact probe paths. Similar pre-existing names are never scanned
     * or removed; recovery derives only paths authorized by this checked nonce plan. */
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        free_probes(probes);
        probes = NULL;
        arc4random_buf(plan.nonce, sizeof(plan.nonce));
        ok = probe_paths(r, count, &plan, &probes);
        for (NameProbe *p = probes; ok && p; p = p->next)
        {
            struct stat st;
            ok = lstat(p->path, &st) && errno == ENOENT;
        }
        if (ok)
            break;
    }
    if (!ok)
    {
        free_probes(probes);
        return false;
    }
    bytes_hash(r, count * sizeof(*r), plan.records);
    bytes_hash(&plan, offsetof(ProbePlan, digest), plan.digest);
    char record[MT_PATH_MAX];
    ok = mt_path(record, dir, "probe-plan") && mt_atomic(record, &plan, sizeof(plan));
    bool published = ok;
    for (NameProbe *p = probes; ok && p; p = p->next)
    {
        ok = !mkdir(p->path, 0700) && mt_sync_dir(p->parent);
        struct stat private_dir;
        if (ok)
            ok = !lstat(p->path, &private_dir);
        for (uint32_t i = 0; ok && i < count; ++i)
        {
            struct stat st;
            if (!lstat(r[i].target.target, &st) && st.st_dev == private_dir.st_dev &&
                st.st_ino == private_dir.st_ino)
                ok = false;
        }
        char marker[MT_PATH_MAX];
        if (ok)
            ok = probe_marker(marker, p, &plan);
        int fd = ok ? open(marker, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600) : -1;
        if (fd < 0)
            ok = false;
        else
        {
            ok = write(fd, &plan, sizeof(plan)) == sizeof(plan) && fsync_supported_ok(fd);
            if (close(fd))
                ok = false;
        }
        if (ok)
            ok = mt_sync_dir(p->path);
    }
    for (uint32_t i = 0; ok && i < count; ++i)
    {
        NameProbe *p = probe_parent(probes, r[i].target.target);
        char file[MT_PATH_MAX], stage[MT_PATH_MAX];
        for (unsigned kind = 0; ok && kind < 2; ++kind)
        {
            ok = p && (kind ? staged(stage, r[i].target.target) &&
                                  mt_path(file, p->path, strrchr(stage, '/') + 1)
                            : mt_path(file, p->path, strrchr(r[i].target.target, '/') + 1));
            int fd = ok ? open(file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600) : -1;
            if (fd < 0)
                ok = false;
            else if (close(fd))
                ok = false;
        }
    }
    free_probes(probes);
    if (published)
    {
        bool had, unstaged;
        if (!cleanup_probe_plan(dir, r, count, &had, &unstaged, ok))
            ok = false;
    }
    return ok;
}

static bool read_phase(const char *dir, uint32_t *phase)
{
    char path[MT_PATH_MAX];
    if (!mt_path(path, dir, "phase"))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint32_t r[4];
    bool ok = fread(r, 1, sizeof(r), f) == sizeof(r) && fgetc(f) == EOF && !(ferror)(f) &&
              r[0] == 0x45504853u && r[2] == ~r[1] && !r[3] && r[1] >= MT_PREPARING &&
              r[1] <= MT_COMMITTED;
    if (fclose(f))
        ok = false;
    if (ok)
        *phase = r[1];
    return ok;
}
static bool absent_journal(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return errno == ENOENT;
    bool ok = true, phase_seen = false, probe_seen = false;
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
        if (!strcmp(e->d_name, "phase"))
            phase_seen = true;
        else if (!strcmp(e->d_name, "probe-plan"))
            probe_seen = true;
        else
            ok = false; /* surviving originals/unknown evidence cannot certify quiescence */
    }
    if (closedir(d))
        ok = false;
    uint32_t phase;
    if (phase_seen)
        ok = ok && read_phase(dir, &phase) && phase == MT_COMMITTED;
    if (ok && probe_seen)
    {
        /* Metadata-only rejection first cleaned every probe, then published terminal
         * phase and removed its journal. This checked initial-plan receipt survives
         * until that removal is durable; it authorizes no target/stage deletion. */
        char path[MT_PATH_MAX];
        ProbePlan plan;
        unsigned char digest[32];
        FILE *f = mt_path(path, dir, "probe-plan") ? fopen(path, "rb") : NULL;
        ok = phase_seen && f && fread(&plan, 1, sizeof(plan), f) == sizeof(plan) &&
             fgetc(f) == EOF && !(ferror)(f);
        if (f && fclose(f))
            ok = false;
        if (ok)
        {
            bytes_hash(&plan, offsetof(ProbePlan, digest), digest);
            ok = plan.magic == 0x45504e4du && plan.version == 1 && plan.before_staging == 1 &&
                 !plan.reserved && !memcmp(plan.digest, digest, 32) && discard_probe_plan(dir);
        }
    }
    if (ok && phase_seen)
    {
        char p[MT_PATH_MAX];
        ok = mt_path(p, dir, "phase") && !unlink(p) && mt_sync_dir(dir);
    }
    return ok && mt_sync_dir(dir) && mt_sync_parent(dir);
}
static bool set_phase(const char *dir, MtPhase phase)
{
    char p[MT_PATH_MAX];
    uint32_t value[4] = {0x45504853u, (uint32_t)phase, ~(uint32_t)phase, 0};
    return mt_path(p, dir, "phase") && mt_atomic(p, value, sizeof(value));
}
static bool finish_metadata(const char *dir)
{
    char p[MT_PATH_MAX];
    if (!set_phase(dir, MT_COMMITTED))
        return false;
    if (!mt_path(p, dir, "journal") || (unlink(p) && errno != ENOENT) || !mt_sync_dir(dir))
        return false;
    return discard_probe_plan(dir) && mt_path(p, dir, "phase") && (!unlink(p) || errno == ENOENT) &&
           mt_sync_dir(dir);
}
static bool write_journal(const char *dir, const Record *r, uint32_t count)
{
    size_t size = sizeof(Journal) + count * sizeof(*r);
    unsigned char *b = calloc(1, size);
    if (!b)
        return false;
    Journal *h = (Journal *)b;
    h->magic = 0x4554584e;
    h->version = 2;
    h->count = count;
    memcpy(b + sizeof(*h), r, count * sizeof(*r));
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    self_update_sha256_add(&hash, r, count * sizeof(*r));
    self_update_sha256_finish(&hash, h->digest);
    char p[MT_PATH_MAX];
    bool ok = mt_path(p, dir, "journal") && mt_atomic(p, b, size);
    free(b);
    return ok;
}
static bool cleanup(const char *dir, const Record *r, uint32_t count)
{
    char p[MT_PATH_MAX];
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!staged(p, r[i].target.target))
            return false;
        if (unlink(p) && errno != ENOENT)
            return false;
        if (!mt_sync_parent(p))
            return false;
        if (!artifact(p, dir, i, "old"))
            return false;
        if (unlink(p) && errno != ENOENT)
            return false;
        if (!artifact(p, dir, i, "rollback"))
            return false;
        if (unlink(p) && errno != ENOENT)
            return false;
    }
    return finish_metadata(dir);
}
static bool durable_mode(const char *path, uint32_t mode)
{
    if (chmod(path, mode))
        return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    bool ok = fsync_supported_ok(fd);
    if (close(fd))
        ok = false;
    return ok;
}
static bool rollback(const char *dir, const Record *r, uint32_t count, const MtObserver *o)
{
    char old[MT_PATH_MAX], tmp[MT_PATH_MAX];
    /* Verify ALL recovery bytes/current states before changing any target. */
    for (uint32_t i = 0; i < count; ++i)
    {
        const Record *x = r + i;
        struct stat st;
        if (x->existed && (!artifact(old, dir, i, "old") || !same(old, x->old_hash, x->old_size)))
            return false;
        if (!lstat(x->target.target, &st))
        {
            if (!same(x->target.target, x->old_hash, x->old_size) &&
                (x->removal || !same(x->target.target, x->new_hash, x->new_size)))
                return false;
        }
        else if (errno != ENOENT)
            return false;
    }
    if (!set_phase(dir, MT_ROLLBACK))
        return false;
    progress(o, MT_ROLLBACK, 0, count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const Record *x = r + i;
        if (x->existed)
        {
            if (!artifact(old, dir, i, "old") || !staged(tmp, x->target.target))
                return false;
            if (unlink(tmp) && errno != ENOENT)
                return false;
            if (!mt_copy(old, tmp, NULL) || !durable_mode(tmp, x->old_mode) ||
                rename(tmp, x->target.target) != 0 || !mt_sync_parent(x->target.target))
                return false;
        }
        else
        {
            if (unlink(x->target.target) && errno != ENOENT)
                return false;
            if (!mt_sync_parent(x->target.target))
                return false;
        }
        progress(o, MT_ROLLBACK, i + 1, count);
    }
    /* Durable terminal marker makes partially completed cleanup recoverable. */
    return set_phase(dir, MT_COMMITTED) && cleanup(dir, r, count);
}
bool mt_recover(const char *dir, const struct dr_save_entry *allowed, uint32_t allowed_count,
                const MtObserver *o, char *error, size_t es)
{
    char p[MT_PATH_MAX];
    if (allowed_count > DR_SAVE_MAX || (allowed_count && !allowed) || !mt_path(p, dir, "journal"))
        return fail(error, es, "Bad journal path or ownership");
    FILE *f = fopen(p, "rb");
    if (!f)
    {
        if (errno == ENOENT)
            return absent_journal(dir) ? true
                                       : fail(error, es, "Missing journal with recovery evidence");
        return fail(error, es, "Read journal");
    }
    Journal h;
    Record *r = NULL;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == 0x4554584e && h.version == 2 &&
              h.count <= MT_FILES_MAX && h.reserved == 0;
    if (ok)
    {
        r = calloc(h.count ? h.count : 1, sizeof(*r));
        ok = r && fread(r, sizeof(*r), h.count, f) == h.count && fgetc(f) == EOF && !(ferror)(f);
    }
    fclose(f);
    if (ok)
    {
        self_update_sha256 hash;
        unsigned char d[32];
        self_update_sha256_start(&hash);
        self_update_sha256_add(&hash, r, h.count * sizeof(*r));
        self_update_sha256_finish(&hash, d);
        ok = !memcmp(d, h.digest, 32);
    }
    for (uint32_t i = 0; ok && i < h.count; ++i)
    {
        Record *x = r + i;
        ok = x->existed <= 1 && x->removal <= 1 && x->old_mode <= 0777 && !x->reserved &&
             x->target.mode <= 0777 && memchr(x->target.target, 0, MT_PATH_MAX) &&
             memchr(x->target.source, 0, MT_PATH_MAX) && !below(dir, x->target.target) &&
             mt_scope_contains(&x->target.scope, x->target.target);
        bool authorized = false;
        for (uint32_t j = 0; ok && j < allowed_count; ++j)
            if (mt_scope_contains(allowed + j, x->target.target))
                authorized = true;
        ok = ok && authorized;
        for (uint32_t j = 0; ok && j < i; ++j)
            ok = disjoint(&r[j].target, &x->target);
    }
    uint32_t phase = MT_PREPARING;
    if (ok)
        ok = read_phase(dir, &phase);
    bool probe_pending = false, probe_unstaged = false;
    if (ok)
        ok = cleanup_probe_plan(dir, r, h.count, &probe_pending, &probe_unstaged, false);
    if (ok && probe_pending && probe_unstaged && (phase == MT_PREPARING || phase == MT_COMMITTED))
        ok = finish_metadata(dir); /* plan survives only before actual staging starts */
    else if (ok)
    {
        ok = (!probe_pending || discard_probe_plan(dir)) &&
             filesystem_names_disjoint(dir, r, h.count, false);
        if (ok)
            ok = (phase == MT_PREPARING || phase == MT_COMMITTED) ? cleanup(dir, r, h.count)
                                                                  : rollback(dir, r, h.count, o);
    }
    free(r);
    return ok ? true : fail(error, es, "Recovery required; originals retained");
}
bool mt_transaction(const char *dir, const MtTarget *targets, uint32_t count, const MtObserver *o,
                    char *error, size_t es)
{
    if (count > MT_FILES_MAX || (!targets && count))
        return fail(error, es, "Invalid transaction");
    char p[MT_PATH_MAX];
    struct stat st;
    if (!mt_mkdirs(dir) || !mt_path(p, dir, "journal"))
        return fail(error, es, "Create journal directory");
    if (!lstat(p, &st) || errno != ENOENT)
        return fail(error, es, "Recover existing journal first");
    if (!mt_path(p, dir, "phase"))
        return false;
    if (!lstat(p, &st) || errno != ENOENT || !absent_journal(dir))
        return fail(error, es, "Recover existing phase or evidence first");
    Record *r = calloc(count ? count : 1, sizeof(*r));
    if (!r)
        return fail(error, es, "Memory");
    bool ok = true;
    uint64_t required = 0;
    for (uint32_t i = 0; ok && i < count; ++i)
    {
        Record *x = r + i;
        x->target = targets[i];
        ok = memchr(x->target.target, 0, MT_PATH_MAX) && memchr(x->target.source, 0, MT_PATH_MAX) &&
             x->target.mode <= 0777 && !below(dir, x->target.target) &&
             mt_scope_contains(&x->target.scope, x->target.target) && staged(p, x->target.target);
        if (!ok)
            break;
        for (uint32_t j = 0; ok && j < i; ++j)
            ok = disjoint(&r[j].target, &x->target);
        if (!ok)
            break;
        if (!lstat(p, &st) || errno != ENOENT)
        {
            ok = false;
            break;
        }
        x->removal = !x->target.source[0];
        if (!lstat(x->target.target, &st))
        {
            x->existed = 1;
            x->old_mode = st.st_mode & 0777;
            ok = S_ISREG(st.st_mode) && mt_hash(x->target.target, x->old_hash, &x->old_size);
        }
        else
            ok = errno == ENOENT;
        if (ok && !x->removal)
            ok = mt_hash(x->target.source, x->new_hash, &x->new_size);
        if (ok)
            ok = UINT64_MAX - required >= x->old_size &&
                 UINT64_MAX - required - x->old_size >= x->new_size;
        if (ok)
        {
            required += x->old_size + x->new_size;
            ok = mt_space(x->target.target, required) && mt_space(dir, required);
        }
    }
    if (ok)
        ok = set_phase(dir, MT_PREPARING) && write_journal(dir, r, count);
    bool prepared = false;
    if (ok)
        ok = filesystem_names_disjoint(dir, r, count, true);
    prepared = ok;
    progress(o, MT_PREPARING, 0, count);
    for (uint32_t i = 0; ok && i < count; ++i)
    {
        Record *x = r + i;
        ok = !mt_cancelled(o);
        if (ok && x->existed)
            ok = artifact(p, dir, i, "old") && mt_copy(x->target.target, p, o) &&
                 same(p, x->old_hash, x->old_size);
        if (ok && !x->removal)
            ok = staged(p, x->target.target) && mt_copy(x->target.source, p, o) &&
                 durable_mode(p, x->target.mode) && same(p, x->new_hash, x->new_size);
        if (ok && !x->existed)
            ok = parent_dirs(x->target.target);
        progress(o, MT_PREPARING, i + 1, count);
    }
    bool applying = false;
    if (ok && !mt_cancelled(o))
    {
        ok = set_phase(dir, MT_APPLYING);
        applying = ok;
    }
    else
        ok = false;
    if (ok)
        progress(o, MT_APPLYING, 0, count);
    for (uint32_t i = 0; ok && i < count; ++i)
    {
        Record *x = r + i;
        ok = !mt_cancelled(o) && mt_scope_contains(&x->target.scope, x->target.target);
        if (ok && x->existed)
            ok = same(x->target.target, x->old_hash, x->old_size);
        if (ok && !x->existed)
            ok = lstat(x->target.target, &st) && errno == ENOENT;
        if (ok)
        {
            if (x->removal)
                ok = unlink(x->target.target) == 0 || errno == ENOENT;
            else
                ok = staged(p, x->target.target) && rename(p, x->target.target) == 0;
        }
        if (ok)
            ok = mt_sync_parent(x->target.target);
        progress(o, MT_APPLYING, i + 1, count);
    }
    if (ok)
    {
        ok = set_phase(dir, MT_COMMITTED);
        if (ok)
        {
            progress(o, MT_COMMITTED, count, count);
            ok = cleanup(dir, r, count);
        }
    }
    else if (applying)
    {
        if (!rollback(dir, r, count, o))
            fail(error, es, "Rollback pending; keep journal");
    }
    else
    { /* No target changed; cleanup only a journal belonging to this invocation. */
        if (mt_path(p, dir, "journal") && !lstat(p, &st))
        {
            bool had, unstaged;
            if (cleanup_probe_plan(dir, r, count, &had, &unstaged, false))
            {
                if (prepared)
                    cleanup(dir, r, count);
                else
                    finish_metadata(dir);
            }
        }
    }
    free(r);
    return ok ? true : fail(error, es, "Transaction failed; recovery required before launch");
}
