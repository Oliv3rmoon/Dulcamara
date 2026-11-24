/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "saves.h"
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
#include <time.h>
#include <unistd.h>

typedef struct
{
    uint32_t root, reserved;
    char relative[MT_PATH_MAX];
    uint64_t size, modified;
    unsigned char digest[32];
} SaveFile;
typedef struct
{
    uint32_t magic, version, kind, core, count, reserved;
    uint64_t created, session;
    char game[24];
    struct dr_save_layout layout;
    unsigned char digest[32];
} Manifest;
typedef struct
{
    Manifest head;
    SaveFile *files;
    size_t capacity;
} Snapshot;
static bool error(char *e, size_t n, const char *s)
{
    if (e && n)
        snprintf(e, n, "%s (%s)", s, strerror(errno));
    return false;
}
/* errno belongs to this read, not to any work performed on the previous entry. */
static struct dirent *next_entry(DIR *directory, bool *ok)
{
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (!entry && errno)
        *ok = false;
    return entry;
}
static bool id_valid(const char *s, size_t max)
{
    if (!s || !s[0] || strnlen(s, max) >= max)
        return false;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_'))
            return false;
    return true;
}
static bool kind_valid(uint32_t k)
{
    return k >= SAVE_AUTOMATIC && k <= SAVE_PRE_INITIAL;
}
static bool automatic(uint32_t k)
{
    return k == SAVE_AUTOMATIC || k == SAVE_PRE_INITIAL;
}
static bool layout_flags_valid(const struct dr_save_layout *layout)
{
    const unsigned char *b = (const unsigned char *)layout;
    const size_t offsets[] = {offsetof(struct dr_save_layout, available),
                              offsetof(struct dr_save_layout, complete),
                              offsetof(struct dr_save_layout, checkpoint),
                              offsetof(struct dr_save_layout, initial),
                              offsetof(struct dr_save_layout, writers_finished),
                              offsetof(struct dr_save_layout, preinitial)};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i)
        if (b[offsets[i]] > 1)
            return false;
    return true;
}
static bool path_overlap(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    return (!strncasecmp(a, b, n) && (b[n] == 0 || b[n] == '/')) ||
           (!strncasecmp(a, b, m) && (a[m] == 0 || a[m] == '/'));
}
/* Never let a native declaration absorb the service's own backups/journals/catalog.
 * Also reserve updater staging/cache for Task6's separate explicit app ownership. */
static bool layout_separate(const char *root, const struct dr_save_layout *layout)
{
    static const char *reserved[] = {"backups", "save-layouts", "save-transactions", "maintenance",
                                     "cache"};
    char canonical[MT_PATH_MAX];
    if (!mt_canonical(root, canonical))
        return false;
    for (uint32_t i = 0; i < layout->count; ++i)
        for (size_t j = 0; j < sizeof(reserved) / sizeof(*reserved); ++j)
        {
            char path[MT_PATH_MAX];
            if (!mt_path(path, canonical, reserved[j]) ||
                path_overlap(path, layout->entries[i].path))
                return false;
        }
    return true;
}
static bool layout_valid(const struct dr_save_layout *l)
{
    if (!layout_flags_valid(l) || l->version != DR_RUNTIME_VERSION || l->size != sizeof(*l) ||
        !l->available || !l->complete || !l->count || l->count > DR_SAVE_MAX)
        return false;
    for (uint32_t i = 0; i < l->count; ++i)
        if (!dr_save_entry_valid(l->entries + i))
            return false;
    return true;
}
static bool game_path(char out[MT_PATH_MAX], const char *root, const char *category,
                      const char *game)
{
    char base[MT_PATH_MAX];
    return id_valid(game, 24) && mt_path(base, root, category) && mt_path(out, base, game);
}
static bool snapshot_path(char out[MT_PATH_MAX], const char *root, const char *game,
                          const char *snapshot)
{
    char b[MT_PATH_MAX];
    return id_valid(snapshot, SAVE_ID_MAX) && game_path(b, root, "backups", game) &&
           mt_path(out, b, snapshot);
}
/* A native-file artifact selector scans only its containing directory; every
 * candidate still passes the exact filename/artifact matcher before collection. */
static void collection_root(const struct dr_save_entry *entry, char out[MT_PATH_MAX])
{
    strcpy(out, entry->path);
    if (entry->kind == DR_SAVE_RENPY_FILE)
    {
        char *slash = strrchr(out, '/');
        if (slash == out)
            slash[1] = 0;
        else
            *slash = 0;
    }
}
static bool file_path(char out[MT_PATH_MAX], const Snapshot *s, uint32_t n)
{
    const SaveFile *f = s->files + n;
    const struct dr_save_entry *entry = s->head.layout.entries + f->root;
    if (entry->kind == DR_SAVE_FILE)
    {
        strcpy(out, entry->path);
        return !strcmp(f->relative, "file") && mt_scope_contains(entry, out);
    }
    char base[MT_PATH_MAX];
    collection_root(entry, base);
    return mt_path(out, base, f->relative) && mt_scope_contains(entry, out);
}
static bool blob_path(char out[MT_PATH_MAX], const char *dir, uint32_t n)
{
    char rel[40];
    snprintf(rel, sizeof(rel), "data/%u", n);
    return mt_path(out, dir, rel);
}
static void digest_manifest(Manifest *h, const SaveFile *f, unsigned char out[32])
{
    Manifest copy = *h;
    memset(copy.digest, 0, 32);
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    self_update_sha256_add(&hash, &copy, sizeof(copy));
    self_update_sha256_add(&hash, f, h->count * sizeof(*f));
    self_update_sha256_finish(&hash, out);
}
static bool manifest_write(const char *dir, Snapshot *s)
{
    char p[MT_PATH_MAX];
    if (!mt_path(p, dir, "manifest"))
        return false;
    digest_manifest(&s->head, s->files, s->head.digest);
    size_t n = sizeof(s->head) + s->head.count * sizeof(*s->files);
    unsigned char *b = malloc(n);
    if (!b)
        return false;
    memcpy(b, &s->head, sizeof(s->head));
    if (s->head.count)
        memcpy(b + sizeof(s->head), s->files, s->head.count * sizeof(*s->files));
    bool ok = mt_atomic(p, b, n);
    free(b);
    return ok;
}
static bool manifest_read(const char *dir, Snapshot *s, bool verify)
{
    memset(s, 0, sizeof(*s));
    char p[MT_PATH_MAX];
    if (!mt_path(p, dir, "manifest"))
        return false;
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    FILE *f = fdopen(fd, "rb");
    if (!f)
    {
        close(fd);
        return false;
    }
    Manifest *h = &s->head;
    bool ok = fread(h, 1, sizeof(*h), f) == sizeof(*h) && h->magic == 0x45534e50 &&
              h->version == 2 && kind_valid(h->kind) && h->core < 3 && h->reserved == 0 &&
              memchr(h->game, 0, sizeof(h->game)) && id_valid(h->game, 24) &&
              h->count <= MT_FILES_MAX && layout_valid(&h->layout);
    if (ok)
    {
        s->files = calloc(h->count ? h->count : 1, sizeof(*s->files));
        ok = s->files && fread(s->files, sizeof(*s->files), h->count, f) == h->count &&
             fgetc(f) == EOF && !(ferror)(f);
    }
    fclose(f);
    unsigned char hash[32];
    if (ok)
    {
        digest_manifest(h, s->files, hash);
        ok = !memcmp(hash, h->digest, 32);
    }
    for (uint32_t i = 0; ok && i < h->count; ++i)
    {
        SaveFile *x = s->files + i;
        ok = x->root < h->layout.count && !x->reserved &&
             memchr(x->relative, 0, sizeof(x->relative)) && mt_relative(x->relative) &&
             file_path(p, s, i);
        for (uint32_t j = 0; ok && j < i; ++j)
        {
            char q[MT_PATH_MAX];
            ok = file_path(q, s, j) && strcmp(p, q) != 0;
        }
        if (ok && verify)
        {
            uint64_t size;
            ok = blob_path(p, dir, i) && mt_hash(p, hash, &size) && size == x->size &&
                 !memcmp(hash, x->digest, 32);
        }
    }
    if (!ok)
    {
        free(s->files);
        s->files = NULL;
    }
    return ok;
}
static bool append_file(Snapshot *s, uint32_t root, const char *relative, const char *path)
{
    for (uint32_t i = 0; i < s->head.count; ++i)
    {
        char existing[MT_PATH_MAX];
        if (!file_path(existing, s, i))
            return false;
        if (!strcmp(path, existing))
            return true;
    }
    if (s->head.count == MT_FILES_MAX)
        return false;
    if (s->head.count == s->capacity)
    {
        size_t n = s->capacity ? s->capacity * 2 : 32;
        SaveFile *p = realloc(s->files, n * sizeof(*p));
        if (!p)
            return false;
        s->files = p;
        s->capacity = n;
    }
    SaveFile *f = s->files + s->head.count;
    memset(f, 0, sizeof(*f));
    f->root = root;
    if (strlen(relative) >= sizeof(f->relative))
        return false;
    strcpy(f->relative, relative);
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || !mt_hash(path, f->digest, &f->size))
        return false;
    f->modified = (uint64_t)st.st_mtime;
    ++s->head.count;
    return true;
}
static bool walk(Snapshot *s, uint32_t root, const char *path, const char *rel, const MtObserver *o,
                 unsigned depth)
{
    if (mt_cancelled(o) || depth > 64)
        return false;
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT;
    if (S_ISLNK(st.st_mode))
        return false;
    const struct dr_save_entry *entry = s->head.layout.entries + root;
    if (S_ISREG(st.st_mode))
    {
        if (!mt_scope_contains(entry, path))
            return true;
        return append_file(s, root, entry->kind == DR_SAVE_FILE ? "file" : rel, path);
    }
    if (!S_ISDIR(st.st_mode) || entry->kind == DR_SAVE_FILE)
        return false;
    /* Native slot scans enumerate files directly in the declared location. */
    if (depth && (entry->kind == DR_SAVE_RPG_SLOTS || entry->kind == DR_SAVE_RENPY_SLOTS ||
                  entry->kind == DR_SAVE_RENPY_FILE))
        return true;
    DIR *d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = next_entry(d, &ok)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[MT_PATH_MAX], r[MT_PATH_MAX];
        if (rel[0])
            ok = mt_path(r, rel, e->d_name);
        else
        {
            ok = strlen(e->d_name) < sizeof(r);
            if (ok)
                strcpy(r, e->d_name);
        }
        if (ok)
            ok = mt_path(p, path, e->d_name);
        if (!ok)
            break;
        if ((entry->kind == DR_SAVE_RPG_SLOTS || entry->kind == DR_SAVE_RENPY_SLOTS ||
             entry->kind == DR_SAVE_RENPY_FILE) &&
            !mt_scope_contains(entry, p))
            continue;
        ok = walk(s, root, p, r, o, depth + 1);
    }
    if (closedir(d))
        ok = false;
    return ok;
}
static bool collect(Snapshot *s, const MtObserver *o)
{
    for (uint32_t i = 0; i < s->head.layout.count; ++i)
    {
        char base[MT_PATH_MAX];
        collection_root(s->head.layout.entries + i, base);
        if (!walk(s, i, base, "", o, 0))
            return false;
    }
    return true;
}
static bool layout_canonical(struct dr_save_layout *l)
{
    if (!layout_valid(l))
        return false;
    for (uint32_t i = 0; i < l->count; ++i)
    {
        char p[MT_PATH_MAX];
        if (!mt_canonical(l->entries[i].path, p))
            return false;
        strcpy(l->entries[i].path, p);
        char probe[MT_PATH_MAX];
        if (l->entries[i].kind == DR_SAVE_FILE || l->entries[i].kind == DR_SAVE_RENPY_FILE)
            strcpy(probe, p);
        else if (!mt_path(probe, p,
                          l->entries[i].kind == DR_SAVE_RPG_SLOTS     ? "Save01.lsd"
                          : l->entries[i].kind == DR_SAVE_RENPY_SLOTS ? "persistent"
                                                                      : "probe"))
            return false;
        if (!mt_scope_contains(l->entries + i, probe))
            return false;
    }
    return true;
}
static bool catalog_path(char out[MT_PATH_MAX], const char *root, const char *game)
{
    return game_path(out, root, "save-layouts", game);
}
static bool catalog_read(const char *root, const char *game, struct dr_save_layout *out)
{
    char p[MT_PATH_MAX];
    if (!catalog_path(p, root, game))
        return false;
    FILE *f = fopen(p, "rb");
    if (!f)
        return false;
    bool ok = fread(out, 1, sizeof(*out), f) == sizeof(*out) && fgetc(f) == EOF && !(ferror)(f);
    fclose(f);
    if (!ok || !layout_valid(out))
        return false;
    struct dr_save_layout canonical = *out;
    if (!layout_canonical(&canonical) || !layout_separate(root, out))
        return false;
    for (uint32_t i = 0; i < out->count; ++i)
        if (strcmp(canonical.entries[i].path, out->entries[i].path))
            return false;
    return true;
}
static bool catalog_store(const char *root, const char *game, const struct dr_save_layout *declared)
{
    char path[MT_PATH_MAX];
    if (!catalog_path(path, root, game))
        return false;
    struct dr_save_layout merged = *declared, previous;
    struct stat st;
    if (!lstat(path, &st))
    {
        if (!catalog_read(root, game, &previous))
            return false;
        for (uint32_t i = 0; i < previous.count; ++i)
        {
            bool found = false;
            for (uint32_t j = 0; j < merged.count; ++j)
                if (previous.entries[i].kind == merged.entries[j].kind &&
                    !strcmp(previous.entries[i].suffix, merged.entries[j].suffix) &&
                    !strcmp(previous.entries[i].path, merged.entries[j].path))
                {
                    found = true;
                    if (previous.entries[i].scope == DR_SAVE_SHARED)
                        merged.entries[j].scope = DR_SAVE_SHARED;
                }
            if (!found)
            {
                if (merged.count == DR_SAVE_MAX)
                    return false;
                merged.entries[merged.count++] = previous.entries[i];
            }
        }
    }
    else if (errno != ENOENT)
        return false;
    merged.generation = 0;
    merged.checkpoint = merged.initial = merged.preinitial = false;
    merged.writers_finished = true;
    if (!lstat(path, &st) && !memcmp(&merged, &previous, sizeof(merged)))
        return true;
    return layout_separate(root, &merged) && mt_atomic(path, &merged, sizeof(merged));
}
static bool overlap(const struct dr_save_entry *a, const struct dr_save_entry *b)
{
    /* Association warnings conservatively include case aliases on removable media
     * and case-insensitive desktops. Mutation still requires exact canonical scope. */
    if ((a->kind != DR_SAVE_FILE || b->kind != DR_SAVE_FILE) && path_overlap(a->path, b->path))
        return true;
    struct stat x, y;
    if (!stat(a->path, &x) && !stat(b->path, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino)
        return true;
    if (!strcasecmp(a->path, b->path))
        return true;
    if (a->kind != DR_SAVE_FILE && mt_scope_contains(a, b->path))
        return true;
    if (b->kind != DR_SAVE_FILE && mt_scope_contains(b, a->path))
        return true;
    return false;
}
static bool shared_layout(const char *root, const char *game, struct dr_save_layout *l)
{
    char p[MT_PATH_MAX];
    if (!mt_path(p, root, "save-layouts"))
        return false;
    DIR *d = opendir(p);
    if (!d)
        return errno == ENOENT;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = next_entry(d, &ok)))
    {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, game))
            continue;
        if (!id_valid(e->d_name, 24))
        {
            ok = false;
            break;
        }
        struct dr_save_layout other;
        if (!catalog_read(root, e->d_name, &other))
        {
            ok = false;
            break;
        }
        for (uint32_t i = 0; i < l->count; ++i)
            for (uint32_t j = 0; j < other.count; ++j)
                if (overlap(l->entries + i, other.entries + j))
                    l->entries[i].scope = DR_SAVE_SHARED;
    }
    if (closedir(d))
        ok = false;
    return ok;
}
static bool remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    DIR *d = opendir(path);
    if (!d)
        return false;
    struct dirent *e;
    bool ok = true;
    while (ok && (e = next_entry(d, &ok)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[MT_PATH_MAX];
        ok = mt_path(p, path, e->d_name) && remove_tree(p);
    }
    if (closedir(d))
        ok = false;
    return ok && rmdir(path) == 0;
}
/* One durable intent per game's hidden workspace. The recorded inode, of a
 * directory on the same filesystem as the game's backups folder (so not a mount
 * point), is the ownership proof; names alone never authorize recursive
 * deletion. The recorded device number is kept in the record but not compared:
 * a device number need not survive a restart. Legacy unrecorded artifacts
 * remain untouched. */
typedef struct
{
    uint32_t magic, version, operation, reserved;
    uint64_t device, inode;
    char game[24], name[SAVE_ID_MAX];
} CleanupIntent;
static bool cleanup_path(char out[MT_PATH_MAX], const char *base)
{
    return mt_path(out, base, ".cleanup-intent");
}
static bool cleanup_owned(const char *base)
{
    char intent_path[MT_PATH_MAX], target[MT_PATH_MAX], canonical[MT_PATH_MAX], next[MT_PATH_MAX];
    struct stat st, folder;
    if (!mt_canonical(base, canonical) || strcmp(base, canonical) || lstat(base, &folder) ||
        !S_ISDIR(folder.st_mode) ||
        !cleanup_path(intent_path, base) || !mt_path(next, base, ".cleanup-next"))
        return false;
    /* A surviving .cleanup-next was never authoritative: every action follows
     * only its rename to .cleanup-intent and a successful directory flush.
     * Discard the service's regular slot left by an interrupted write; any other
     * file type is not ours and stays preserved for diagnosis. */
    if (!lstat(next, &st))
    {
        if (!S_ISREG(st.st_mode) || unlink(next) || !mt_sync_dir(base))
            return false;
    }
    else if (errno != ENOENT)
        return false;
    if (lstat(intent_path, &st))
        return errno == ENOENT && mt_sync_dir(base);
    if (!S_ISREG(st.st_mode) || st.st_size != sizeof(CleanupIntent))
        return false;
    CleanupIntent record = {0};
    FILE *f = fopen(intent_path, "rb");
    if (!f)
        return false;
    bool ok = fread(&record, 1, sizeof(record), f) == sizeof(record);
    if (fclose(f))
        ok = false;
    const char *game = strrchr(base, '/');
    const char *prefix = record.operation == 1 ? ".pending-" : ".deleting-";
    size_t prefix_size = strlen(prefix);
    if (!ok || record.magic != 0x45434c4e || record.version != 1 || record.reserved ||
        (record.operation != 1 && record.operation != 2) ||
        !memchr(record.game, 0, sizeof(record.game)) || !game ||
        strcmp(record.game, game + 1) ||
        !memchr(record.name, 0, sizeof(record.name)) ||
        strncmp(record.name, prefix, prefix_size) ||
        !id_valid(record.name + prefix_size, sizeof(record.name) - prefix_size) ||
        !mt_path(target, base, record.name))
        return false;
    if (!lstat(target, &st))
    {
        if (!S_ISDIR(st.st_mode))
            return false;
        if (!record.inode)
        {
            /* Before identity publication no bytes may have been copied.
             * rmdir alone can remove only the exact planned empty directory;
             * never recurse through an unproven identity. */
            if (record.operation != 1 || record.device || rmdir(target))
                return false;
        }
        else if (st.st_dev != folder.st_dev || record.inode != (uint64_t)st.st_ino ||
                 !remove_tree(target))
            return false;
    }
    else if (errno != ENOENT)
        return false;
    /* Retry must flush target absence even if a previous remove already won. */
    return mt_sync_dir(base) && unlink(intent_path) == 0 && mt_sync_dir(base);
}
static bool cleanup_record(const char *base, const char *name, unsigned operation,
                           const struct stat *identity)
{
    char path[MT_PATH_MAX];
    const char *game = strrchr(base, '/');
    if (!game || !id_valid(game + 1, 24) || !cleanup_path(path, base))
        return false;
    CleanupIntent record = {.magic = 0x45434c4e, .version = 1, .operation = operation};
    snprintf(record.game, sizeof(record.game), "%s", game + 1);
    snprintf(record.name, sizeof(record.name), "%s", name);
    if (identity)
    {
        record.device = identity->st_dev;
        record.inode = identity->st_ino;
    }
    char next[MT_PATH_MAX];
    if (!mt_path(next, base, ".cleanup-next"))
        return false;
    int fd = open(next, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    bool ok = write(fd, &record, sizeof(record)) == sizeof(record);
    if (fsync(fd) && errno != ENOSYS && errno != EINVAL && errno != ENOTSUP)
        ok = false;
    if (close(fd))
        ok = false;
    if (ok)
        ok = rename(next, path) == 0 && mt_sync_dir(base);
    if (!ok)
        (void)unlink(next); /* only the temporary file exclusively created above */
    return ok;
}
/* The startup pass over every game's backups. Hidden backup workspaces never
 * hold live saves, so a game whose cleanup record cannot be settled is left as
 * found and named in note (its own snapshot, restore and delete refuse through
 * cleanup_owned until it is); it does not lock the other games' launches. A
 * name in backups/ that is no game's id is not ours and is passed over. Only a
 * failure to read backups/ itself fails the pass. */
static bool cleanup_all(const char *root, char *note, size_t note_size)
{
    char base[MT_PATH_MAX];
    if (!mt_path(base, root, "backups"))
        return false;
    DIR *d = opendir(base);
    if (!d)
        return errno == ENOENT;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = next_entry(d, &ok)))
    {
        if (entry->d_name[0] == '.' || !id_valid(entry->d_name, 24))
            continue;
        char game[MT_PATH_MAX];
        if (!mt_path(game, base, entry->d_name) || !cleanup_owned(game))
        {
            const size_t used = strnlen(note, note_size);
            if (used + 1 < note_size)
                snprintf(note + used, note_size - used, "%s%s", used ? ", " : "Backups left as found for ",
                         entry->d_name);
        }
    }
    if (closedir(d))
        ok = false;
    return ok;
}
/* Unpublish by directory rename before cleanup, so an interrupted deletion
 * cannot expose a half-deleted "completed" snapshot to listing or retention. */
static bool unpublish(const char *dir)
{
    char hidden[MT_PATH_MAX], base[MT_PATH_MAX], name[SAVE_ID_MAX];
    const char *slash = strrchr(dir, '/');
    if (!slash || (size_t)(slash - dir) >= sizeof(base))
        return false;
    snprintf(base, sizeof(base), "%.*s", (int)(slash - dir), dir);
    int n = snprintf(name, sizeof(name), ".deleting-%s", slash + 1);
    struct stat st, original;
    if (n < 0 || n >= (int)sizeof(name) || !mt_path(hidden, base, name) ||
        !cleanup_owned(base) || !lstat(hidden, &st) || errno != ENOENT ||
        lstat(dir, &original) || !S_ISDIR(original.st_mode))
        return false;
    if (!cleanup_record(base, name, 2, &original) || rename(dir, hidden) ||
        !mt_sync_dir(base))
        return false;
    return cleanup_owned(base);
}
static void info(const Snapshot *s, const char *id, SaveInfo *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->snapshot, sizeof(out->snapshot), "%s", id);
    out->kind = (SaveKind)s->head.kind;
    out->moment = s->head.kind == SAVE_MANUAL       ? SAVE_MOMENT_MANUAL
                  : s->head.kind == SAVE_RECOVERY   ? SAVE_MOMENT_RECOVERY
                  : s->head.layout.writers_finished ? SAVE_MOMENT_POSTSTOP
                  : s->head.layout.preinitial       ? SAVE_MOMENT_EARLY
                                                    : SAVE_MOMENT_PREPLAY;
    out->created = s->head.created;
    out->files = s->head.count;
    for (uint32_t i = 0; i < s->head.count; ++i)
        out->bytes += s->files[i].size;
    for (uint32_t i = 0; i < s->head.layout.count; ++i)
        out->shared |= s->head.layout.entries[i].scope == DR_SAVE_SHARED;
}
bool save_details(const char *root, const char *game, const char *id, SaveInfo *out,
                  struct dr_save_layout *layout, char *e, size_t es)
{
    char dir[MT_PATH_MAX];
    Snapshot s;
    if (!snapshot_path(dir, root, game, id) || !manifest_read(dir, &s, true))
        return error(e, es, "Snapshot damaged or unavailable");
    bool ok = !strcmp(s.head.game, game) && layout_separate(root, &s.head.layout) &&
              shared_layout(root, game, &s.head.layout);
    if (ok)
    {
        if (out)
            info(&s, id, out);
        if (layout)
            *layout = s.head.layout;
    }
    free(s.files);
    return ok ? true : error(e, es, "Ownership unavailable");
}
static int newest(const void *a, const void *b)
{
    const SaveInfo *x = a, *y = b;
    return x->created < y->created   ? 1
           : x->created > y->created ? -1
                                     : strcmp(y->snapshot, x->snapshot);
}
static bool list_page(const char *root, const char *game, SaveInfo *out, size_t capacity,
                      size_t offset, size_t *count, char *error_out, size_t es)
{
    if (count)
        *count = 0;
    char dir[MT_PATH_MAX];
    if (!game_path(dir, root, "backups", game))
        return error(error_out, es, "Invalid game");
    DIR *d = opendir(dir);
    if (!d)
        return errno == ENOENT;
    SaveInfo *all = NULL;
    size_t n = 0;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = next_entry(d, &ok)))
    {
        if (e->d_name[0] == '.')
            continue;
        char p[MT_PATH_MAX];
        Snapshot s;
        if (!snapshot_path(p, root, game, e->d_name) || !manifest_read(p, &s, false))
        {
            ok = false;
            break;
        }
        if (strcmp(s.head.game, game))
        {
            free(s.files);
            ok = false;
            break;
        }
        SaveInfo *next = realloc(all, (n + 1) * sizeof(*all));
        if (!next)
        {
            free(s.files);
            ok = false;
            break;
        }
        all = next;
        info(&s, e->d_name, all + n++);
        free(s.files);
    }
    if (closedir(d))
        ok = false;
    if (ok)
    {
        if (n)
            qsort(all, n, sizeof(*all), newest);
        size_t remaining = offset < n ? n - offset : 0;
        size_t copied = remaining < capacity ? remaining : capacity;
        if (copied && out)
            memcpy(out, all + offset, copied * sizeof(*out));
        if (count)
            *count = n;
    }
    free(all);
    return ok ? true : error(error_out, es, "Invalid backup record retained");
}
bool save_list(const char *root, const char *game, SaveInfo *out, size_t capacity, size_t *count,
               char *error_out, size_t es)
{
    return list_page(root, game, out, capacity, 0, count, error_out, es);
}
static bool register_layout(const char *root, const SaveRequest *request)
{
    struct dr_save_layout layout = request->layout;
    char path[MT_PATH_MAX];
    return layout_canonical(&layout) && shared_layout(root, request->game_id, &layout) &&
           catalog_path(path, root, request->game_id) && layout_separate(root, &layout) &&
           catalog_store(root, request->game_id, &layout);
}
static bool same_content(const Snapshot *a, const Snapshot *b)
{
    if (a->head.count != b->head.count || a->head.layout.count != b->head.layout.count)
        return false;
    for (uint32_t i = 0; i < a->head.layout.count; ++i)
    {
        const struct dr_save_entry *x = a->head.layout.entries + i, *y = b->head.layout.entries + i;
        if (x->kind != y->kind || strcmp(x->suffix, y->suffix) || strcmp(x->path, y->path))
            return false;
    }
    for (uint32_t i = 0; i < a->head.count; ++i)
    {
        char path[MT_PATH_MAX];
        if (!file_path(path, a, i))
            return false;
        bool found = false;
        for (uint32_t j = 0; j < b->head.count; ++j)
        {
            char other[MT_PATH_MAX];
            if (!file_path(other, b, j))
                return false;
            if (!strcmp(path, other))
            {
                found = a->files[i].size == b->files[j].size &&
                        !memcmp(a->files[i].digest, b->files[j].digest, 32);
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}
static bool prune(const char *root, const char *game, const char *published)
{
    size_t count = 0;
    char why[256];
    if (!save_list(root, game, NULL, 0, &count, why, sizeof(why)))
        return false;
    SaveInfo *list = calloc(count ? count : 1, sizeof(*list));
    if (!list)
        return false;
    bool ok = save_list(root, game, list, count, &count, why, sizeof(why));
    unsigned autos =
        1; /* The newly published snapshot stays protected even if the clock moved back. */
    for (size_t i = 0; ok && i < count; ++i)
        if (automatic(list[i].kind) && strcmp(list[i].snapshot, published) && ++autos > 5)
        {
            char dir[MT_PATH_MAX];
            Snapshot verified;
            ok = snapshot_path(dir, root, game, list[i].snapshot) &&
                 manifest_read(dir, &verified, true);
            if (ok)
            {
                free(verified.files);
                ok = unpublish(dir);
            }
        }
    free(list);
    return ok;
}
/* Keep first-seen bytes/absence from early native declarations in every later
 * startup snapshot of this session. Thus five-auto retention cannot discard the
 * only original when more than five early MultiPersistent roots are discovered. */
static bool early_baseline(const char *root, const SaveRequest *request, Snapshot *s,
                           Snapshot *prior, char prior_dir[MT_PATH_MAX], int **origins)
{
    memset(prior, 0, sizeof(*prior));
    *origins = NULL;
    prior_dir[0] = 0;
    if (!request->session || !automatic(request->kind) || !s->head.layout.checkpoint ||
        s->head.layout.writers_finished || (!s->head.layout.preinitial && !s->head.layout.initial))
        return true;
    if (request->baseline[0])
    {
        if (!snapshot_path(prior_dir, root, request->game_id, request->baseline) ||
            !manifest_read(prior_dir, prior, true) || prior->head.kind != SAVE_PRE_INITIAL ||
            strcmp(prior->head.game, request->game_id))
            return false;
    }
    size_t count = 0;
    char why[256];
    if (!save_list(root, request->game_id, NULL, 0, &count, why, sizeof(why)))
        return false;
    SaveInfo *list = calloc(count ? count : 1, sizeof(*list));
    if (!list)
        return false;
    bool ok = save_list(root, request->game_id, list, count, &count, why, sizeof(why));
    for (size_t i = 0; ok && !prior_dir[0] && i < count; ++i)
    {
        if (list[i].kind != SAVE_PRE_INITIAL)
            continue;
        Snapshot candidate;
        char path[MT_PATH_MAX];
        ok = snapshot_path(path, root, request->game_id, list[i].snapshot) &&
             manifest_read(path, &candidate, true);
        if (!ok)
            break;
        if (candidate.head.session == request->session)
        {
            *prior = candidate;
            strcpy(prior_dir, path);
            break;
        }
        free(candidate.files);
    }
    free(list);
    if (!ok || !prior_dir[0])
        return ok;
    uint32_t mapped[DR_SAVE_MAX];
    for (uint32_t i = 0; i < prior->head.layout.count; ++i)
    {
        mapped[i] = DR_SAVE_MAX;
        for (uint32_t j = 0; j < s->head.layout.count; ++j)
            if (prior->head.layout.entries[i].kind == s->head.layout.entries[j].kind &&
                !strcmp(prior->head.layout.entries[i].suffix, s->head.layout.entries[j].suffix) &&
                !strcmp(prior->head.layout.entries[i].path, s->head.layout.entries[j].path))
                mapped[i] = j;
        if (mapped[i] == DR_SAVE_MAX)
            return false;
    }
    size_t capacity = (size_t)s->head.count + prior->head.count;
    SaveFile *files = calloc(capacity ? capacity : 1, sizeof(*files));
    int *source = malloc((capacity ? capacity : 1) * sizeof(*source));
    if (!files || !source)
    {
        free(files);
        free(source);
        return false;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->head.count; ++i)
    {
        char path[MT_PATH_MAX];
        if (!file_path(path, s, i))
        {
            free(files);
            free(source);
            return false;
        }
        bool covered = false;
        for (uint32_t j = 0; j < prior->head.layout.count; ++j)
            if (mt_scope_contains(prior->head.layout.entries + j, path))
                covered = true;
        if (!covered)
        {
            files[n] = s->files[i];
            source[n++] = -1;
        }
    }
    for (uint32_t i = 0; i < prior->head.count; ++i)
    {
        files[n] = prior->files[i];
        files[n].root = mapped[files[n].root];
        source[n++] = (int)i;
    }
    free(s->files);
    s->files = files;
    s->head.count = n;
    s->capacity = capacity;
    *origins = source;
    return true;
}
static bool snapshot_impl(const char *root, const SaveRequest *r, const MtObserver *o,
                          SaveResult *result)
{
    memset(result, 0, sizeof(*result));
    if (!r || !id_valid(r->game_id, 24) || !kind_valid(r->kind) || r->core >= 3)
        return error(result->error, sizeof(result->error), "Invalid request");
    SaveRequest resolved = *r;
    if (!resolved.layout.count)
    {
        if (!catalog_read(root, r->game_id, &resolved.layout))
            return error(result->error, sizeof(result->error),
                         "No known native save layout; launch and safely stop this game first");
        r = &resolved;
    }
    Snapshot s = {0};
    s.head = (Manifest){.magic = 0x45534e50,
                        .version = 2,
                        .kind = r->kind,
                        .core = r->core,
                        .session = r->session,
                        .layout = r->layout};
    strcpy(s.head.game, r->game_id);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    s.head.created = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
    char catalog[MT_PATH_MAX];
    bool ok = layout_canonical(&s.head.layout) && layout_separate(root, &s.head.layout) &&
              shared_layout(root, r->game_id, &s.head.layout) &&
              catalog_path(catalog, root, r->game_id) &&
              catalog_store(root, r->game_id, &s.head.layout) && collect(&s, o);
    Snapshot prior = {0};
    char prior_dir[MT_PATH_MAX];
    int *origins = NULL;
    if (ok)
        ok = early_baseline(root, r, &s, &prior, prior_dir, &origins);
    uint64_t bytes = sizeof(s.head) + (uint64_t)s.head.count * sizeof(SaveFile);
    for (uint32_t i = 0; ok && i < s.head.count; ++i)
    {
        ok = UINT64_MAX - bytes >= s.files[i].size;
        if (ok)
            bytes += s.files[i].size;
    }
    char base[MT_PATH_MAX], dir[MT_PATH_MAX] = "", published[MT_PATH_MAX];
    ok = ok && game_path(base, root, "backups", r->game_id) && mt_mkdirs(base) &&
         cleanup_owned(base);
    /* Compare newest automatic snapshot of this phase, verifying its bytes. */
    if (ok && automatic(r->kind))
    {
        size_t count = 0;
        ok = save_list(root, r->game_id, NULL, 0, &count, result->error, sizeof(result->error));
        SaveInfo *list = ok ? calloc(count ? count : 1, sizeof(*list)) : NULL;
        ok = ok && list &&
             save_list(root, r->game_id, list, count, &count, result->error, sizeof(result->error));
        for (size_t i = 0; ok && i < count; ++i)
            if (list[i].kind == r->kind)
            {
                Snapshot prior;
                char path[MT_PATH_MAX];
                ok = snapshot_path(path, root, r->game_id, list[i].snapshot) &&
                     manifest_read(path, &prior, true);
                if (ok)
                {
                    result->unchanged = same_content(&s, &prior);
                    free(prior.files);
                    if (result->unchanged)
                    {
                        /* A previous rename may have succeeded while its parent flush
                         * failed. Content equality cannot certify publication durability. */
                        ok = mt_sync_parent(path);
                        if (ok)
                            strcpy(result->snapshot, list[i].snapshot);
                    }
                }
                break;
            }
        free(list);
    }
    if (ok && !result->unchanged)
    {
        char id[SAVE_ID_MAX];
        snprintf(id, sizeof(id), "%llu-%ld", (unsigned long long)s.head.created, (long)getpid());
        char temp[SAVE_ID_MAX];
        snprintf(temp, sizeof(temp), ".pending-%llu", (unsigned long long)s.head.created);
        struct stat pending_identity;
        ok = mt_space(base, bytes) && mt_path(dir, base, temp) &&
             lstat(dir, &pending_identity) != 0 && errno == ENOENT &&
             cleanup_record(base, temp, 1, NULL) && mkdir(dir, 0700) == 0 &&
             mt_sync_parent(dir) && !lstat(dir, &pending_identity) &&
             cleanup_record(base, temp, 1, &pending_identity) && mt_path(published, base, id);
        for (uint32_t i = 0; ok && i < s.head.count; ++i)
        {
            char source[MT_PATH_MAX], target[MT_PATH_MAX];
            unsigned char hash[32];
            uint64_t size;
            ok = !mt_cancelled(o) &&
                 (origins && origins[i] >= 0 ? blob_path(source, prior_dir, (uint32_t)origins[i])
                                             : file_path(source, &s, i)) &&
                 blob_path(target, dir, i) && mt_copy(source, target, o) &&
                 mt_hash(target, hash, &size) && size == s.files[i].size &&
                 !memcmp(hash, s.files[i].digest, 32);
            if (o && o->progress)
                o->progress(o->context, MT_PREPARING, i + 1, s.head.count);
        }
        if (ok)
            ok = !mt_cancelled(o) && manifest_write(dir, &s) && mt_sync_dir(dir);
        Snapshot verify;
        if (ok)
        {
            ok = manifest_read(dir, &verify, true);
            if (ok)
                free(verify.files);
        }
        if (ok)
            ok = rename(dir, published) == 0 && mt_sync_parent(published);
        if (ok)
            ok = cleanup_owned(base);
        if (ok)
            strcpy(result->snapshot, id);
        else if (dir[0])
            (void)cleanup_owned(base);
    }
    if (ok)
    {
        /* A successful unchanged retry may be certifying a previously renamed
         * snapshot whose failed publication flush skipped retention. */
        if (automatic(r->kind) && !prune(root, r->game_id, result->snapshot))
            snprintf(result->error, sizeof(result->error),
                     "Backup published; retention deferred because a record could not be verified");
        result->files = s.head.count;
        result->bytes = bytes - sizeof(s.head) - (uint64_t)s.head.count * sizeof(SaveFile);
        result->created = s.head.created;
        for (uint32_t i = 0; i < s.head.layout.count; ++i)
            result->shared |= s.head.layout.entries[i].scope == DR_SAVE_SHARED;
    }
    free(origins);
    free(prior.files);
    free(s.files);
    return ok ? true
              : error(result->error, sizeof(result->error),
                      mt_cancelled(o) ? "Backup cancelled"
                                      : "Backup failed; prior backups retained");
}
static bool restore_impl(const char *root, const char *game, const char *id, bool confirmed,
                         const MtObserver *o, SaveResult *result)
{
    memset(result, 0, sizeof(*result));
    if (!confirmed)
        return error(result->error, sizeof(result->error), "Restore confirmation required");
    char dir[MT_PATH_MAX], journal[MT_PATH_MAX];
    Snapshot s;
    if (!snapshot_path(dir, root, game, id) || !manifest_read(dir, &s, true))
        return error(result->error, sizeof(result->error), "Snapshot damaged; saves unchanged");
    struct dr_save_layout current;
    bool ok = !strcmp(s.head.game, game) && layout_separate(root, &s.head.layout) &&
              catalog_read(root, game, &current) && shared_layout(root, game, &s.head.layout);
    for (uint32_t i = 0; ok && i < s.head.layout.count; ++i)
    {
        bool found = false;
        for (uint32_t j = 0; j < current.count; ++j)
            if (current.entries[j].kind == s.head.layout.entries[i].kind &&
                !strcmp(current.entries[j].suffix, s.head.layout.entries[i].suffix) &&
                !strcmp(current.entries[j].path, s.head.layout.entries[i].path))
                found = true;
        ok = found;
    }
    SaveRequest recovery = {.kind = SAVE_RECOVERY, .core = s.head.core, .layout = s.head.layout};
    snprintf(recovery.game_id, sizeof(recovery.game_id), "%s", game);
    SaveResult recovered_copy;
    if (ok)
        ok = snapshot_impl(root, &recovery, o, &recovered_copy);
    Snapshot live = {.head = s.head};
    live.head.count = 0;
    if (ok)
        ok = collect(&live, o);
    size_t capacity = (size_t)s.head.count + live.head.count;
    MtTarget *targets = ok ? calloc(capacity ? capacity : 1, sizeof(*targets)) : NULL;
    if (ok && !targets)
        ok = false;
    uint32_t count = 0;
    for (uint32_t i = 0; ok && i < s.head.count; ++i)
    {
        MtTarget *t = targets + count++;
        t->scope = s.head.layout.entries[s.files[i].root];
        t->mode = 0600;
        ok = file_path(t->target, &s, i) && blob_path(t->source, dir, i);
    }
    for (uint32_t i = 0; ok && i < live.head.count; ++i)
    {
        char path[MT_PATH_MAX];
        ok = file_path(path, &live, i);
        bool found = false;
        for (uint32_t j = 0; j < count; ++j)
            if (!strcmp(path, targets[j].target))
                found = true;
        if (!found)
        {
            MtTarget *t = targets + count++;
            strcpy(t->target, path);
            t->scope = live.head.layout.entries[live.files[i].root];
            t->mode = 0600;
        }
    }
    if (ok)
        ok = game_path(journal, root, "save-transactions", game) &&
             mt_transaction(journal, targets, count, o, result->error, sizeof(result->error));
    if (ok)
    {
        strcpy(result->snapshot, id);
        result->files = s.head.count;
        result->shared = recovered_copy.shared;
    }
    free(targets);
    free(live.files);
    free(s.files);
    if (!ok && !result->error[0])
        error(result->error, sizeof(result->error), "Restore failed; originals retained");
    return ok;
}
static bool delete_impl(const char *root, const char *game, const char *id, char *e, size_t es)
{
    SaveInfo i;
    char dir[MT_PATH_MAX], journal[MT_PATH_MAX], p[MT_PATH_MAX];
    struct stat st;
    if (!save_details(root, game, id, &i, NULL, e, es))
        return false;
    if (i.kind == SAVE_RECOVERY)
        return error(e, es, "Recovery snapshots are retained");
    if (!game_path(journal, root, "save-transactions", game) || !mt_path(p, journal, "journal") ||
        !lstat(p, &st) || errno != ENOENT)
        return error(e, es, "Pending restore must recover first");
    return snapshot_path(dir, root, game, id) && unpublish(dir);
}
static bool restore_pending(const char *root, const char *game)
{
    char dir[MT_PATH_MAX], path[MT_PATH_MAX];
    struct stat st;
    if (!game_path(dir, root, "save-transactions", game) || !mt_path(path, dir, "journal"))
        return false;
    return !lstat(path, &st) || errno != ENOENT;
}
bool save_snapshot(const char *root, const SaveRequest *r, const MtObserver *o, SaveResult *out)
{
    if (!maintenance_acquire(MAINT_SNAPSHOT))
        return error(out->error, sizeof(out->error), "Runtime or maintenance is unsafe/busy");
    bool ok = snapshot_impl(root, r, o, out);
    maintenance_release(MAINT_SNAPSHOT, false);
    return ok;
}
bool save_restore(const char *root, const char *game, const char *id, bool confirmed,
                  const MtObserver *o, SaveResult *out)
{
    if (!maintenance_acquire(MAINT_RESTORE))
        return error(out->error, sizeof(out->error), "Stop runtime and finish recovery first");
    bool ok = restore_impl(root, game, id, confirmed, o, out);
    if (!ok && restore_pending(root, game))
        maintenance_recovered(false);
    maintenance_release(MAINT_RESTORE, false);
    return ok;
}
bool save_delete(const char *root, const char *game, const char *id, char *e, size_t es)
{
    if (!maintenance_acquire(MAINT_RESTORE))
        return error(e, es, "Stop runtime and finish recovery first");
    bool ok = delete_impl(root, game, id, e, es);
    maintenance_release(MAINT_RESTORE, false);
    return ok;
}
static SavesUpdateRecovery update_recovery;
static void *update_recovery_context;
void saves_set_update_recovery(SavesUpdateRecovery callback, void *context)
{
    update_recovery = callback;
    update_recovery_context = context;
}
bool save_recover(const char *root, const MtObserver *o, char *e, size_t es)
{
    char canonical[MT_PATH_MAX];
    if (!mt_canonical(root, canonical) || strcmp(root, canonical))
        return error(e, es, "Invalid maintenance root");
    if (!maintenance_acquire(MAINT_RECOVERY))
        return error(e, es, "Runtime or maintenance busy");
    if (e && es)
        e[0] = 0;
    char base[MT_PATH_MAX];
    bool ok = mt_path(base, root, "save-transactions");
    DIR *d = ok ? opendir(base) : NULL;
    /* A fresh library has no restore journal. Its first restore creates the directory. */
    if (ok && !d && errno != ENOENT)
    {
        if (e && es) snprintf(e, es, "Cannot open %.160s (%s)", base, strerror(errno));
        ok = false;
    }
    struct dirent *entry;
    while (ok && d && (entry = next_entry(d, &ok)))
    {
        if (entry->d_name[0] == '.')
            continue;
        struct dr_save_layout layout;
        char dir[MT_PATH_MAX];
        ok = catalog_read(root, entry->d_name, &layout) && mt_path(dir, base, entry->d_name) &&
             mt_recover(dir, layout.entries, layout.count, o, e, es);
    }
    if (d && closedir(d))
        ok = false;
    char note[256] = "";
    if (ok)
        ok = cleanup_all(root, note, sizeof(note));
    maintenance_recovered(ok);
    bool restart = false;
    if (ok && update_recovery)
    {
        ok = update_recovery(update_recovery_context, &restart, e, es);
        maintenance_recovery_finish(MAINT_RECOVER_UPDATE, ok);
    }
    maintenance_release(MAINT_RECOVERY, restart);
    if (ok && note[0] && e && es)
        snprintf(e, es, "%s (their cleanup records need attention; other games are unaffected)", note);
    if (!ok && e && es && !*e)
        error(e, es, "Recovery incomplete; launch remains locked");
    return ok;
}

static pthread_mutex_t service_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool joinable, cancel_requested;
static char service_root[MT_PATH_MAX];
static SavesStatus status;
static uint64_t job_serial;
typedef struct
{
    SavesOperation operation;
    SaveRequest request;
    char id[SAVE_ID_MAX];
    bool confirmed;
    size_t offset;
    uint64_t serial;
} Job;
static bool cancelled(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&service_lock);
    bool value = cancel_requested;
    pthread_mutex_unlock(&service_lock);
    return value;
}
static void job_progress(void *unused, MtPhase phase, uint64_t done, uint64_t total)
{
    (void)unused;
    pthread_mutex_lock(&service_lock);
    status.phase = phase;
    status.done = done;
    status.total = total;
    pthread_mutex_unlock(&service_lock);
}
static MaintenanceOwner job_owner(SavesOperation op)
{
    return (op == SAVES_CREATE || op == SAVES_REGISTER) ? MAINT_SNAPSHOT
           : op == SAVES_RECOVER                        ? MAINT_RECOVERY
                                                        : MAINT_RESTORE;
}
static void *run_job(void *arg)
{
    Job *j = arg;
    MtObserver observer = {cancelled, job_progress, NULL};
    SavesStatus result = {.operation = j->operation, .serial = j->serial};
    bool ok = false;
    switch (j->operation)
    {
    case SAVES_RECOVER:
        ok =
            save_recover(service_root, &observer, result.result.error, sizeof(result.result.error));
        break;
    case SAVES_CREATE:
        ok = snapshot_impl(service_root, &j->request, &observer, &result.result);
        break;
    case SAVES_RESTORE:
        ok = restore_impl(service_root, j->request.game_id, j->id, j->confirmed, &observer,
                          &result.result);
        if (!ok && restore_pending(service_root, j->request.game_id))
            maintenance_recovered(false);
        break;
    case SAVES_DELETE:
        ok = delete_impl(service_root, j->request.game_id, j->id, result.result.error,
                         sizeof(result.result.error));
        break;
    case SAVES_LIST:
        result.offset = j->offset;
        ok = list_page(service_root, j->request.game_id, result.list, SAVE_LIST_MAX, j->offset,
                       &result.count, result.result.error, sizeof(result.result.error));
        break;
    case SAVES_DETAILS:
        ok = save_details(service_root, j->request.game_id, j->id, &result.detail, &result.layout,
                          result.result.error, sizeof(result.result.error));
        break;
    case SAVES_REGISTER:
        ok = register_layout(service_root, &j->request);
        if (!ok)
            snprintf(result.result.error, sizeof(result.result.error),
                     "Could not persist save ownership");
        break;
    }
    if (j->operation != SAVES_RECOVER)
        maintenance_release(job_owner(j->operation), false);
    pthread_mutex_lock(&service_lock);
    result.state = ok ? SAVES_DONE : cancel_requested ? SAVES_CANCELLED : SAVES_ERROR;
    result.phase = status.phase;
    result.done = status.done;
    result.total = status.total;
    status = result;
    pthread_mutex_unlock(&service_lock);
    free(j);
    return NULL;
}
void saves_poll(SavesStatus *out)
{
    pthread_mutex_lock(&service_lock);
    bool join = joinable && status.state != SAVES_WORKING;
    if (join)
        joinable = false;
    if (out)
        *out = status;
    pthread_mutex_unlock(&service_lock);
    if (join)
        pthread_join(worker, NULL);
}
static bool start_job(SavesOperation op, const SaveRequest *r, const char *id, bool confirmed,
                      size_t offset)
{
    saves_poll(NULL);
    pthread_mutex_lock(&service_lock);
    if (!service_root[0] || status.state == SAVES_WORKING || op > SAVES_REGISTER ||
        op < SAVES_RECOVER)
    {
        pthread_mutex_unlock(&service_lock);
        return false;
    }
    Job *j = calloc(1, sizeof(*j));
    if (!j)
    {
        pthread_mutex_unlock(&service_lock);
        return false;
    }
    j->operation = op;
    if (r)
        j->request = *r;
    if (id)
    {
        if (!id_valid(id, sizeof(j->id)))
        {
            free(j);
            pthread_mutex_unlock(&service_lock);
            return false;
        }
        strcpy(j->id, id);
    }
    j->confirmed = confirmed;
    j->offset = offset;
    if (op != SAVES_RECOVER && !maintenance_acquire(job_owner(op)))
    {
        free(j);
        pthread_mutex_unlock(&service_lock);
        return false;
    }
    if (op == SAVES_RECOVER)
        maintenance_recovered(false);
    cancel_requested = false;
    j->serial = ++job_serial;
    status = (SavesStatus){.state = SAVES_WORKING, .operation = op, .serial = j->serial};
    bool ok = pthread_create(&worker, NULL, run_job, j) == 0;
    if (ok)
        joinable = true;
    else
    {
        status.state = SAVES_ERROR;
        snprintf(status.result.error, sizeof(status.result.error), "Could not start save worker");
        if (op != SAVES_RECOVER)
            maintenance_release(job_owner(op), false);
        free(j);
    }
    pthread_mutex_unlock(&service_lock);
    return ok;
}
bool saves_start(SavesOperation op, const SaveRequest *r, const char *id, bool confirmed)
{
    return start_job(op, r, id, confirmed, 0);
}
bool saves_start_page(const char *game, size_t offset)
{
    if (!id_valid(game, 24))
        return false;
    SaveRequest request = {0};
    strcpy(request.game_id, game);
    return start_job(SAVES_LIST, &request, NULL, false, offset);
}
bool saves_init(const char *root)
{
    if (saves_busy() || !mt_canonical(root, service_root))
        return false;
    maintenance_recovered(false);
    return saves_start(SAVES_RECOVER, NULL, NULL, false);
}
void saves_cancel(void)
{
    pthread_mutex_lock(&service_lock);
    cancel_requested = true;
    pthread_mutex_unlock(&service_lock);
}
bool saves_busy(void)
{
    pthread_mutex_lock(&service_lock);
    bool busy = status.state == SAVES_WORKING;
    pthread_mutex_unlock(&service_lock);
    return busy;
}
