#include "saves.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
/* Test-only libc-boundary interposition, with real directory identities. */
static struct stat read_fault_directory;
static int read_fault_after = -1;
static unsigned read_faults;
static void fail_directory_read(const char *path, int after)
{
    assert(!stat(path, &read_fault_directory));
    read_fault_after = after;
}
struct dirent *saves_test_readdir(DIR *directory)
{
    struct stat st;
    bool selected = read_fault_after >= 0 && !fstat(dirfd(directory), &st) &&
                    st.st_dev == read_fault_directory.st_dev &&
                    st.st_ino == read_fault_directory.st_ino;
    if (selected && !read_fault_after)
    {
        read_fault_after = -1;
        ++read_faults;
        errno = EIO;
        return NULL;
    }
    struct dirent *entry = readdir(directory);
    if (selected && entry && strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
        --read_fault_after;
    return entry;
}
static struct stat short_read_identity;
static bool short_read_cleanup;
static unsigned short_read_count;
size_t saves_test_fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    struct stat st;
    if (short_read_cleanup && !fstat(fileno(stream), &st) &&
        st.st_dev == short_read_identity.st_dev && st.st_ino == short_read_identity.st_ino)
    {
        ++short_read_count;
        errno = EIO;
        return 0;
    }
    return fread(buffer, size, count, stream);
}
static bool halt_pending_mkdir;
int saves_test_mkdir(const char *path, mode_t mode)
{
    int result = mkdir(path, mode);
    if (!result && halt_pending_mkdir && strstr(path, "/.pending-"))
        raise(SIGSTOP);
    return result;
}
static int halt_hidden_unlink;
int saves_test_unlink(const char *path)
{
    bool selected = halt_hidden_unlink && strstr(path, "/.deleting-");
    if (selected && halt_hidden_unlink == 1)
        raise(SIGSTOP);
    int result = unlink(path);
    if (selected && halt_hidden_unlink == 2 && !result)
        raise(SIGSTOP);
    return result;
}
/* Stops at the Nth cleanup-record publication, after its bytes are durable in
 * the .cleanup-next slot and before the rename that makes them authoritative. */
static int halt_record_rename;
int saves_test_rename(const char *from, const char *to)
{
    const char *slash = strrchr(from, '/');
    if (halt_record_rename && slash && !strcmp(slash + 1, ".cleanup-next") &&
        !--halt_record_rename)
        raise(SIGSTOP);
    return rename(from, to);
}
static void put(const char *p, const char *s)
{
    FILE *f = fopen(p, "wb");
    assert(f);
    assert(fwrite(s, 1, strlen(s), f) == strlen(s));
    assert(!fclose(f));
}
static void expect(const char *p, const char *s)
{
    char b[64] = {0};
    FILE *f = fopen(p, "rb");
    assert(f);
    assert(fread(b, 1, sizeof(b) - 1, f) == strlen(s));
    fclose(f);
    assert(!strcmp(b, s));
}

static void path(char out[MT_PATH_MAX], const char *base, const char *name)
{
    assert(mt_path(out, base, name));
}
typedef struct
{
    MtPhase phase;
    uint64_t done;
    bool cancel;
} Halt;
static void halt_progress(void *arg, MtPhase phase, uint64_t done, uint64_t total)
{
    (void)total;
    Halt *h = arg;
    if (phase == h->phase && done == h->done)
        raise(SIGSTOP);
}
static bool should_cancel(void *arg)
{
    return ((Halt *)arg)->cancel;
}
static void cancel_progress(void *arg, MtPhase phase, uint64_t done, uint64_t total)
{
    (void)phase;
    (void)total;
    if (done)
        ((Halt *)arg)->cancel = true;
}
static void rollback_halt(void *arg, MtPhase phase, uint64_t done, uint64_t total)
{
    (void)total;
    Halt *h = arg;
    if (phase == MT_APPLYING && done == 1)
        h->cancel = true;
    if (phase == MT_ROLLBACK && done == h->done)
        raise(SIGSTOP);
}
static void transaction_checks(const char *root)
{
    char dir[MT_PATH_MAX], a[MT_PATH_MAX], b[MT_PATH_MAX], src[MT_PATH_MAX], journal[MT_PATH_MAX];
    path(dir, root, "tx-targets");
    assert(mt_mkdirs(dir));
    path(a, dir, "a");
    path(b, dir, "b");
    path(src, root, "new-bytes");
    put(src, "new");
    path(journal, root, "tx-journal");
    MtTarget t[2] = {0};
    struct dr_save_entry scope = {
        .kind = DR_SAVE_DIRECTORY, .scope = DR_SAVE_GAME, .policy = DR_SAVE_READ_WRITE};
    strcpy(scope.path, dir);
    for (int i = 0; i < 2; ++i)
    {
        t[i].scope = scope;
        t[i].mode = 0755;
        strcpy(t[i].source, src);
    }
    strcpy(t[0].target, a);
    strcpy(t[1].target, b);
    const Halt boundaries[] = {{MT_PREPARING, 0, false}, {MT_PREPARING, 1, false},
                               {MT_PREPARING, 2, false}, {MT_APPLYING, 0, false},
                               {MT_APPLYING, 1, false},  {MT_APPLYING, 2, false},
                               {MT_COMMITTED, 2, false}};
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(*boundaries); ++i)
    {
        put(a, "old");
        assert(!chmod(a, 0644));
        unlink(b);
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid)
        {
            Halt h = boundaries[i];
            MtObserver observer = {NULL, halt_progress, &h};
            char error[256];
            bool ok = mt_transaction(journal, t, 2, &observer, error, sizeof(error));
            _exit(ok ? 0 : 2);
        }
        int status;
        assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
        assert(!kill(pid, SIGKILL));
        assert(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
        char error[256] = {0};
        assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
        assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
        struct stat st;
        if (boundaries[i].phase == MT_COMMITTED)
        {
            expect(a, "new");
            expect(b, "new");
        }
        else
        {
            expect(a, "old");
            assert(lstat(b, &st) && errno == ENOENT);
            assert(!stat(a, &st));
            assert((st.st_mode & 0777) == 0644);
        }
    }
    for (unsigned boundary = 0; boundary <= 2; ++boundary)
    {
        put(a, "old");
        unlink(b);
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid)
        {
            Halt h = {MT_ROLLBACK, boundary, false};
            MtObserver observer = {should_cancel, rollback_halt, &h};
            char why[256];
            mt_transaction(journal, t, 2, &observer, why, sizeof(why));
            _exit(4);
        }
        int status;
        assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
        kill(pid, SIGKILL);
        assert(waitpid(pid, &status, 0) == pid);
        char why[256];
        assert(mt_recover(journal, &scope, 1, NULL, why, sizeof(why)));
        expect(a, "old");
        assert(access(b, F_OK));
    }
    /* An invalid persisted phase or mismatched ownership cannot authorize recovery. */
    put(a, "old");
    unlink(b);
    pid_t pending = fork();
    assert(pending >= 0);
    if (!pending)
    {
        Halt h = {MT_APPLYING, 0, false};
        MtObserver observer = {NULL, halt_progress, &h};
        char why[256];
        mt_transaction(journal, t, 2, &observer, why, sizeof(why));
        _exit(4);
    }
    int killed;
    assert(waitpid(pending, &killed, WUNTRACED) == pending && WIFSTOPPED(killed));
    kill(pending, SIGKILL);
    assert(waitpid(pending, &killed, 0) == pending);
    char phase_path[MT_PATH_MAX], why[256];
    path(phase_path, journal, "phase");
    unsigned char valid_phase[16];
    FILE *phase_file = fopen(phase_path, "rb");
    assert(phase_file);
    assert(fread(valid_phase, 1, sizeof(valid_phase), phase_file) == sizeof(valid_phase));
    fclose(phase_file);
    uint32_t phase = 99;
    assert(mt_atomic(phase_path, &phase, sizeof(phase)));
    assert(!mt_recover(journal, &scope, 1, NULL, why, sizeof(why)));
    expect(a, "old");
    assert(mt_atomic(phase_path, valid_phase, sizeof(valid_phase)));
    struct dr_save_entry wrong = scope;
    strcpy(wrong.path, src);
    assert(!mt_recover(journal, &wrong, 1, NULL, why, sizeof(why)));
    assert(mt_recover(journal, &scope, 1, NULL, why, sizeof(why)));
    /* Cancellation during apply rolls back the entire set. */
    put(a, "old");
    unlink(b);
    Halt h = {0};
    MtObserver cancel = {should_cancel, cancel_progress, &h};
    char error[256];
    assert(!mt_transaction(journal, t, 2, &cancel, error, sizeof(error)));
    expect(a, "old");
    assert(!mt_space(dir, UINT64_MAX));
    strcpy(t[0].target, src);
    assert(!mt_transaction(journal, t, 1, NULL, error, sizeof(error)));
    expect(src, "new");
    puts("transactions: killed at prepare/apply/commit boundaries, idempotent recovery, modes, "
         "scope, cancellation, space");
}
static void extra_checks(const char *root, const char *data, SaveRequest *r, const char *file)
{
    SaveResult result;
    char why[256], id[SAVE_ID_MAX], other[MT_PATH_MAX];
    SaveInfo list[64];
    size_t n;
    r->kind = SAVE_MANUAL;
    assert(save_snapshot(root, r, NULL, &result));
    strcpy(id, result.snapshot);
    path(other, data, "RPG_RT.ldb");
    put(other, "asset");
    for (int i = 0; i < 8; ++i)
    {
        char value[20];
        snprintf(value, sizeof(value), "value%d", i);
        put(file, value);
        r->kind = SAVE_AUTOMATIC;
        assert(save_snapshot(root, r, NULL, &result));
    }
    assert(save_list(root, r->game_id, list, 64, &n, why, sizeof(why)));
    unsigned autos = 0, manual = 0;
    for (size_t i = 0; i < n; ++i)
    {
        autos += list[i].kind == SAVE_AUTOMATIC;
        manual += list[i].kind == SAVE_MANUAL;
    }
    assert(autos == 5 && manual == 1);
    assert(save_restore(root, r->game_id, id, true, NULL, &result));
    expect(other, "asset");
    /* Same canonical root belongs to two catalog games; restore detail reports it shared. */
    SaveRequest second = *r;
    strcpy(second.game_id, "other-game");
    assert(save_snapshot(root, &second, NULL, &result));
    SaveInfo detail;
    struct dr_save_layout layout;
    assert(save_details(root, r->game_id, id, &detail, &layout, why, sizeof(why)) && detail.shared);
    /* Missing snapshot bytes fail validation without changing a current native save. */
    char dir[MT_PATH_MAX], blob[MT_PATH_MAX];
    path(dir, root, "backups/fixture");
    char snap[MT_PATH_MAX];
    path(snap, dir, id);
    path(blob, snap, "data/0");
    put(blob, "corrupt");
    put(file, "current");
    assert(!save_restore(root, r->game_id, id, true, NULL, &result));
    expect(file, "current");
    assert(maintenance_launch());
    maintenance_runtime(false, false, false);
    assert(save_recover(root, NULL, why, sizeof(why)));
    /* Empty snapshots restore absence within native scope, preserving assets. */
    unlink(file);
    r->kind = SAVE_MANUAL;
    assert(save_snapshot(root, r, NULL, &result));
    strcpy(id, result.snapshot);
    put(file, "later");
    assert(save_restore(root, r->game_id, id, true, NULL, &result));
    assert(access(file, F_OK));
    expect(other, "asset");
    /* Active or unsafe runtimes never copy or restore, even with a metadata certificate. */
    assert(maintenance_launch());
    assert(!save_snapshot(root, r, NULL, &result));
    assert(!save_restore(root, r->game_id, id, true, NULL, &result));
    maintenance_runtime(false, false, false);
    /* Cancelled copy publishes nothing and keeps completed records. */
    put(file, "cancel me");
    size_t before;
    assert(save_list(root, r->game_id, NULL, 0, &before, why, sizeof(why)));
    Halt h = {0};
    MtObserver o = {should_cancel, cancel_progress, &h};
    assert(!save_snapshot(root, r, &o, &result));
    assert(save_list(root, r->game_id, NULL, 0, &n, why, sizeof(why)) && n == before);
    /* A real filesystem write-limit error, no test hook in production. */
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid)
    {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit = {16, 16};
        assert(!setrlimit(RLIMIT_FSIZE, &limit));
        bool ok = save_snapshot(root, r, NULL, &result);
        _exit(ok ? 3 : 0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    expect(file, "cancel me");
    puts("saves: retention/manual, shared association, corruption, empty saves, runtime exclusion, "
         "cancellation, real write failure");
}
static void early_checks(const char *root, const char *base)
{
    char early[MT_PATH_MAX];
    path(early, base, "early");
    assert(mt_mkdirs(early));
    SaveRequest r = {.core = 1, .kind = SAVE_PRE_INITIAL, .session = 123};
    strcpy(r.game_id, "early");
    r.layout = (struct dr_save_layout){.version = DR_RUNTIME_VERSION,
                                       .size = sizeof(r.layout),
                                       .available = true,
                                       .complete = true,
                                       .checkpoint = true,
                                       .preinitial = true};
    SaveResult result;
    char paths[8][MT_PATH_MAX];
    for (unsigned i = 0; i < 8; ++i)
    {
        char name[16];
        snprintf(name, sizeof(name), "multi-%u", i);
        path(paths[i], early, name);
        if (i % 2 == 0)
            put(paths[i], "original");
        r.layout.count = i + 1;
        struct dr_save_entry *e = r.layout.entries + i;
        strcpy(e->path, paths[i]);
        e->kind = DR_SAVE_FILE;
        e->scope = DR_SAVE_SHARED;
        e->policy = DR_SAVE_READ_WRITE;
        assert(save_snapshot(root, &r, NULL, &result));
        put(paths[i], "written during early");
    }
    r.layout.preinitial = false;
    r.layout.initial = true;
    r.kind = SAVE_AUTOMATIC;
    assert(save_snapshot(root, &r, NULL, &result));
    char id[SAVE_ID_MAX];
    strcpy(id, result.snapshot);
    assert(save_restore(root, r.game_id, id, true, NULL, &result));
    for (unsigned i = 0; i < 8; ++i)
        if (i % 2 == 0)
            expect(paths[i], "original");
        else
            assert(access(paths[i], F_OK));
    /* Clean stop may retain initial=true; it must capture current post-game bytes. */
    for (unsigned i = 0; i < 8; ++i)
        put(paths[i], "post-game");
    r.layout.checkpoint = false;
    r.layout.writers_finished = true;
    assert(save_snapshot(root, &r, NULL, &result) && !result.unchanged);
    strcpy(id, result.snapshot);
    for (unsigned i = 0; i < 8; ++i)
        put(paths[i], "later");
    assert(save_restore(root, r.game_id, id, true, NULL, &result));
    for (unsigned i = 0; i < 8; ++i)
        expect(paths[i], "post-game");
    /* Dedup across launches still carries the unchanged early baseline into the union. */
    strcpy(r.game_id, "early-dedup");
    r.layout.count = 1;
    r.layout.writers_finished = false;
    r.layout.checkpoint = r.layout.preinitial = true;
    r.layout.initial = false;
    r.kind = SAVE_PRE_INITIAL;
    r.session = 200;
    put(paths[0], "unchanged original");
    assert(save_snapshot(root, &r, NULL, &result));
    r.session = 201;
    assert(save_snapshot(root, &r, NULL, &result) && result.unchanged);
    strcpy(r.baseline, result.snapshot);
    put(paths[0], "early change");
    r.layout.preinitial = false;
    r.layout.initial = true;
    r.kind = SAVE_AUTOMATIC;
    assert(save_snapshot(root, &r, NULL, &result));
    strcpy(id, result.snapshot);
    assert(save_restore(root, r.game_id, id, true, NULL, &result));
    expect(paths[0], "unchanged original");
    puts("early snapshots: eight native shared files preserve first-seen content and absence "
         "through five-auto retention");
}

static void wait_job(SavesStatus *status)
{
    do
    {
        saves_poll(status);
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    } while (status->state == SAVES_WORKING);
}
static unsigned char replace_byte(const char *path, size_t offset, unsigned char value)
{
    FILE *file = fopen(path, "r+b");
    assert(file);
    assert(!fseek(file, (long)offset, SEEK_SET));
    int old = fgetc(file);
    assert(old >= 0);
    assert(!fseek(file, (long)offset, SEEK_SET));
    assert(fputc(value, file) == value);
    assert(!fclose(file));
    return (unsigned char)old;
}
static void persisted_flags(const char *root, SaveRequest *request)
{
    SaveResult result;
    assert(save_snapshot(root, request, NULL, &result));
    char dir[MT_PATH_MAX], manifest[MT_PATH_MAX], catalog[MT_PATH_MAX], relative[128];
    snprintf(relative, sizeof(relative), "backups/%s/%s", request->game_id, result.snapshot);
    path(dir, root, relative);
    path(manifest, dir, "manifest");
    snprintf(relative, sizeof(relative), "save-layouts/%s", request->game_id);
    path(catalog, root, relative);
    FILE *file = fopen(manifest, "rb");
    assert(file);
    unsigned char header[256];
    assert(fread(header, 1, sizeof(header), file) == sizeof(header));
    fclose(file);
    uint32_t signature[2] = {DR_RUNTIME_VERSION, sizeof(struct dr_save_layout)};
    size_t layout = 0;
    unsigned matches = 0;
    for (size_t i = 0; i + sizeof(signature) <= sizeof(header); ++i)
        if (!memcmp(header + i, signature, sizeof(signature)))
        {
            layout = i;
            ++matches;
        }
    assert(matches == 1);
    /* Unreleased older protocol layouts are preserved, never silently reinterpreted. */
    unsigned char prior_version = replace_byte(manifest, layout, DR_RUNTIME_VERSION - 1);
    char version_error[256];
    assert(!save_details(root, request->game_id, result.snapshot, NULL, NULL, version_error,
                         sizeof(version_error)));
    assert(!access(manifest, F_OK));
    replace_byte(manifest, layout, prior_version);
    prior_version = replace_byte(catalog, 0, DR_RUNTIME_VERSION - 1);
    SaveRequest unsupported = *request;
    unsupported.layout.count = 0;
    SaveResult rejected_version;
    assert(!save_snapshot(root, &unsupported, NULL, &rejected_version));
    assert(!access(catalog, F_OK));
    replace_byte(catalog, 0, prior_version);
    const size_t flags[] = {offsetof(struct dr_save_layout, available),
                            offsetof(struct dr_save_layout, complete),
                            offsetof(struct dr_save_layout, checkpoint),
                            offsetof(struct dr_save_layout, initial),
                            offsetof(struct dr_save_layout, writers_finished),
                            offsetof(struct dr_save_layout, preinitial)};
    for (unsigned i = 0; i < 6; ++i)
    {
        SaveInfo info;
        char why[256];
        unsigned char old = replace_byte(manifest, layout + flags[i], 2);
        assert(
            !save_details(root, request->game_id, result.snapshot, &info, NULL, why, sizeof(why)));
        replace_byte(manifest, layout + flags[i], old);
        old = replace_byte(catalog, flags[i], 2);
        SaveRequest resolved = *request;
        resolved.layout.count = 0;
        SaveResult rejected;
        assert(!save_snapshot(root, &resolved, NULL, &rejected));
        replace_byte(catalog, flags[i], old);
        assert(
            save_details(root, request->game_id, result.snapshot, &info, NULL, why, sizeof(why)));
    }
}
/* A cleanup record's device number made stale, as after a restart that numbers devices anew: the record's magic,
 * version, operation and reserved words come first, then the device. */
static void stale_device(const char *intent)
{
    FILE *f = fopen(intent, "r+b");
    assert(f && !fseek(f, 16, SEEK_SET));
    unsigned char device[8];
    assert(fread(device, 1, sizeof(device), f) == sizeof(device));
    for (size_t i = 0; i < sizeof(device); ++i)
        device[i] ^= 0x5a;
    assert(!fseek(f, 16, SEEK_SET) && fwrite(device, 1, sizeof(device), f) == sizeof(device) && !fclose(f));
}
static unsigned count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    assert(d);
    unsigned n = 0;
    while (readdir(d))
        ++n;
    assert(!closedir(d));
    return n;
}
/* One game's cleanup record is left as found. Recovery still finishes, naming that game, and unlocks launches; another
 * game snapshots and restores; that game refuses snapshot, restore and delete, and keeps every file it had. */
static void only_that_game_locked(const char *root, const char *base, const SaveRequest *affected,
                                  const char *game_base, const char *native_file, const char *native_value)
{
    char why[256] = "";
    assert(save_recover(root, NULL, why, sizeof(why)));
    assert(strstr(why, affected->game_id));
    assert(maintenance_launch());
    maintenance_runtime(false, false, false);
    const unsigned entries = count_entries(game_base);
    size_t listed = 0, after = 0;
    assert(save_list(root, affected->game_id, NULL, 0, &listed, why, sizeof(why)) && listed > 0);
    SaveInfo *info = calloc(listed, sizeof(*info));
    assert(info && save_list(root, affected->game_id, info, listed, &listed, why, sizeof(why)));
    SaveResult result;
    assert(!save_snapshot(root, affected, NULL, &result));
    assert(!save_restore(root, affected->game_id, info[0].snapshot, true, NULL, &result));
    assert(!save_delete(root, affected->game_id, info[0].snapshot, why, sizeof(why)));
    free(info);
    assert(count_entries(game_base) == entries);
    assert(save_list(root, affected->game_id, NULL, 0, &after, why, sizeof(why)) && after == listed);
    expect(native_file, native_value);
    assert(maintenance_launch());
    maintenance_runtime(false, false, false);
    SaveRequest other = *affected;
    other.kind = SAVE_MANUAL;
    strcpy(other.game_id, "neighbour");
    char native[MT_PATH_MAX], file[MT_PATH_MAX], id[SAVE_ID_MAX];
    path(native, base, "neighbour-native");
    assert(mt_mkdirs(native));
    strcpy(other.layout.entries[0].path, native);
    path(file, native, "slot.save");
    put(file, "neighbour before");
    assert(save_snapshot(root, &other, NULL, &result));
    strcpy(id, result.snapshot);
    put(file, "neighbour after");
    assert(save_restore(root, other.game_id, id, true, NULL, &result));
    expect(file, "neighbour before");
}
static void namespace_and_ui_checks(const char *root, const char *base)
{
    SaveRequest r = {.core = 1, .kind = SAVE_MANUAL};
    strcpy(r.game_id, "namespace");
    r.layout = (struct dr_save_layout){.version = DR_RUNTIME_VERSION,
                                       .size = sizeof(r.layout),
                                       .available = true,
                                       .complete = true,
                                       .count = 1,
                                       .writers_finished = true};
    r.layout.entries[0] = (struct dr_save_entry){
        .kind = DR_SAVE_DIRECTORY, .scope = DR_SAVE_GAME, .policy = DR_SAVE_READ_WRITE};
    SaveResult result;
    const char *bad[] = {root, base, "/"};
    for (unsigned i = 0; i < 3; ++i)
    {
        strcpy(r.layout.entries[0].path, bad[i]);
        assert(!save_snapshot(root, &r, NULL, &result));
    }
    char p[MT_PATH_MAX];
    path(p, root, "backups/child");
    strcpy(r.layout.entries[0].path, p);
    assert(!save_snapshot(root, &r, NULL, &result));
    path(p, root, "save-transactions");
    strcpy(r.layout.entries[0].path, p);
    assert(!save_snapshot(root, &r, NULL, &result));
    path(p, base, "ui-native");
    assert(mt_mkdirs(p));
    strcpy(r.layout.entries[0].path, p);
    strcpy(r.game_id, "pages");
    unsigned char *raw = (unsigned char *)&r.layout;
    raw[offsetof(struct dr_save_layout, complete)] = 2;
    assert(!save_snapshot(root, &r, NULL, &result));
    r.layout.complete = true;
    r.kind = (SaveKind)999;
    assert(!save_snapshot(root, &r, NULL, &result));
    r.kind = SAVE_MANUAL;
    char oldest[SAVE_ID_MAX];
    for (unsigned i = 0; i < 130; ++i)
    {
        assert(save_snapshot(root, &r, NULL, &result));
        if (!i)
            strcpy(oldest, result.snapshot);
    }
    assert(saves_init(root));
    SavesStatus status;
    wait_job(&status);
    assert(status.state == SAVES_DONE);
    assert(saves_start_page("pages", 128));
    wait_job(&status);
    assert(status.state == SAVES_DONE && status.count == 130 && status.offset == 128);
    assert(!strcmp(status.list[1].snapshot, oldest));
    assert(saves_start(SAVES_DETAILS, &r, oldest, false));
    wait_job(&status);
    assert(status.state == SAVES_DONE && status.detail.kind == SAVE_MANUAL &&
           status.layout.count == 1);
    assert(saves_start(SAVES_DELETE, &r, oldest, false));
    wait_job(&status);
    assert(status.state == SAVES_DONE);
    assert(maintenance_launch());
    maintenance_runtime(true, true, false);
    assert(!maintenance_acquire(MAINT_UPDATE));
    assert(saves_start(SAVES_REGISTER, &r, NULL, false));
    wait_job(&status);
    assert(status.state == SAVES_DONE);
    assert(!maintenance_acquire(MAINT_UPDATE)); /* reservation survives worker completion */
    maintenance_runtime(false, false, false);
    assert(maintenance_acquire(MAINT_UPDATE));
    maintenance_release(MAINT_UPDATE, false);
    /* A killed snapshot worker has never published its hidden temporary tree. */
    char file[MT_PATH_MAX];
    path(file, p, "native.save");
    put(file, "original");
    size_t before, n;
    char why[256];
    assert(save_list(root, r.game_id, NULL, 0, &before, why, sizeof(why)));
    for (int cycle = 0; cycle < 12; ++cycle)
    {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid)
        {
            halt_pending_mkdir = cycle >= 6;
            Halt h = {MT_PREPARING, 1, false};
            MtObserver observer = {NULL, halt_progress, &h};
            bool ok = save_snapshot(root, &r, &observer, &result);
            _exit(ok ? 0 : 3);
        }
        int w;
        assert(waitpid(pid, &w, WUNTRACED) == pid && WIFSTOPPED(w));
        kill(pid, SIGKILL);
        assert(waitpid(pid, &w, 0) == pid);
        assert(save_recover(root, NULL, why, sizeof(why)));
        assert(save_list(root, r.game_id, NULL, 0, &n, why, sizeof(why)) && n == before);
        expect(file, "original");
        char backups[MT_PATH_MAX];
        assert(mt_path(backups, root, "backups"));
        char game_backups[MT_PATH_MAX];
        assert(mt_path(game_backups, backups, r.game_id));
        DIR *contents = opendir(game_backups);
        assert(contents);
        struct dirent *item;
        while ((item = readdir(contents)))
            assert(strncmp(item->d_name, ".pending-", 9));
        assert(!closedir(contents));
    }
    /* Repeated interrupted delete, then interrupted cleanup. Legacy similarly
     * named trees are deliberately not service-owned and survive every pass. */
    char backup_base[MT_PATH_MAX], game_base[MT_PATH_MAX], legacy[MT_PATH_MAX];
    path(backup_base, root, "backups");
    path(game_base, backup_base, r.game_id);
    path(legacy, game_base, ".pending-legacy");
    assert(!mkdir(legacy, 0700));
    char legacy_file[MT_PATH_MAX];
    path(legacy_file, legacy, "preserve");
    put(legacy_file, "uncertain recovery bytes");
    for (int cycle = 0; cycle < 6; ++cycle)
    {
        r.kind = SAVE_MANUAL;
        assert(save_snapshot(root, &r, NULL, &result));
        pid_t child = fork();
        assert(child >= 0);
        if (!child)
        {
            halt_hidden_unlink = 1;
            _exit(save_delete(root, r.game_id, result.snapshot, why, sizeof(why)) ? 0 : 3);
        }
        int status;
        assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
        assert(!kill(child, SIGKILL) && waitpid(child, &status, 0) == child);
        if (cycle == 1)
        {
            /* the record's device number no longer matches (a restart renumbered devices): the recorded inode, on
             * the same filesystem as the game's backups, still proves the hidden folder is the app's own */
            char intent[MT_PATH_MAX];
            path(intent, game_base, ".cleanup-intent");
            stale_device(intent);
        }
        if (!cycle)
        {
            char intent[MT_PATH_MAX];
            path(intent, game_base, ".cleanup-intent");
            assert(!stat(intent, &short_read_identity));
            size_t size = (size_t)short_read_identity.st_size;
            unsigned char *bytes = malloc(size);
            assert(bytes);
            FILE *record = fopen(intent, "rb");
            assert(record && fread(bytes, 1, size, record) == size && !fclose(record));
            short_read_cleanup = true;
            /* a record that cannot be read whole: only this game stays locked */
            assert(save_recover(root, NULL, why, sizeof(why)) && short_read_count == 1);
            short_read_cleanup = false;
            struct stat kept;
            assert(!stat(intent, &kept) && kept.st_ino == short_read_identity.st_ino);
            unsigned char first = bytes[0];
            bytes[0] ^= 0xff;
            record = fopen(intent, "wb");
            assert(record && fwrite(bytes, 1, size, record) == size && !fclose(record));
            only_that_game_locked(root, base, &r, game_base, file, "original"); /* bad contents */
            assert(!stat(intent, &kept) && kept.st_size == (off_t)size);
            record = fopen(intent, "rb");
            assert(record && fgetc(record) == bytes[0] && !fclose(record));
            bytes[0] = first;
            record = fopen(intent, "wb");
            assert(record && fwrite(bytes, 1, size, record) == size && !fclose(record));
            free(bytes);
            expect(file, "original");
            char hidden[MT_PATH_MAX] = "", retained[MT_PATH_MAX], outsider[MT_PATH_MAX];
            DIR *listing = opendir(game_base);
            assert(listing);
            struct dirent *item;
            while ((item = readdir(listing)))
                if (!strncmp(item->d_name, ".deleting-", 10))
                    path(hidden, game_base, item->d_name);
            assert(!closedir(listing) && hidden[0]);
            path(retained, game_base, ".uncertain-retained");
            assert(!rename(hidden, retained));
            assert(!mkdir(hidden, 0700));
            path(outsider, hidden, "unrelated");
            put(outsider, "preserve collision");
            only_that_game_locked(root, base, &r, game_base, file, "original"); /* another inode */
            expect(outsider, "preserve collision");
            assert(!unlink(outsider) && !rmdir(hidden));
            assert(!symlink(retained, hidden));
            only_that_game_locked(root, base, &r, game_base, file, "original"); /* a link in its place */
            assert(!access(retained, F_OK));
            assert(!unlink(hidden) && !rename(retained, hidden));
        }
        child = fork();
        assert(child >= 0);
        if (!child)
        {
            halt_hidden_unlink = 2;
            _exit(save_recover(root, NULL, why, sizeof(why)) ? 0 : 3);
        }
        assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
        assert(!kill(child, SIGKILL) && waitpid(child, &status, 0) == child);
        assert(save_recover(root, NULL, why, sizeof(why)));
        assert(save_recover(root, NULL, why, sizeof(why)));
        DIR *contents = opendir(game_base);
        assert(contents);
        struct dirent *item;
        while ((item = readdir(contents)))
            assert(strncmp(item->d_name, ".deleting-", 10));
        assert(!closedir(contents));
        expect(file, "original");
        expect(legacy_file, "uncertain recovery bytes");
    }
    puts("six interrupted snapshots/deletes/recoveries: owned hidden bytes return to zero; legacy retained");
    /* Killed while publishing a cleanup record: before the pending mkdir, after
     * the empty pending mkdir, and before a delete's unpublish rename. No action
     * ever follows an unpublished record, so recovery must discard the regular
     * .cleanup-next slot, finish owned cleanup and leave launch unlocked. */
    for (int stop = 1; stop <= 3; ++stop)
    {
        r.kind = SAVE_MANUAL;
        char victim[SAVE_ID_MAX] = "";
        if (stop == 3)
        {
            assert(save_snapshot(root, &r, NULL, &result));
            strcpy(victim, result.snapshot);
        }
        size_t listed;
        assert(save_list(root, r.game_id, NULL, 0, &listed, why, sizeof(why)));
        pid_t child = fork();
        assert(child >= 0);
        if (!child)
        {
            halt_record_rename = stop == 3 ? 1 : stop;
            bool ok = stop == 3 ? save_delete(root, r.game_id, victim, why, sizeof(why))
                                : save_snapshot(root, &r, NULL, &result);
            _exit(ok ? 0 : 3);
        }
        int status;
        assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
        assert(!kill(child, SIGKILL) && waitpid(child, &status, 0) == child);
        char next[MT_PATH_MAX];
        path(next, game_base, ".cleanup-next");
        struct stat st;
        assert(!lstat(next, &st) && S_ISREG(st.st_mode));
        assert(save_recover(root, NULL, why, sizeof(why)));
        assert(lstat(next, &st) && errno == ENOENT);
        char intent[MT_PATH_MAX];
        path(intent, game_base, ".cleanup-intent");
        assert(lstat(intent, &st) && errno == ENOENT);
        DIR *contents = opendir(game_base);
        assert(contents);
        struct dirent *item;
        while ((item = readdir(contents)))
            assert(!strcmp(item->d_name, ".pending-legacy") ||
                   (strncmp(item->d_name, ".pending-", 9) &&
                    strncmp(item->d_name, ".deleting-", 10)));
        assert(!closedir(contents));
        assert(save_list(root, r.game_id, NULL, 0, &n, why, sizeof(why)) && n == listed);
        expect(file, "original");
        expect(legacy_file, "uncertain recovery bytes");
        /* The single record slot is reusable afterwards. */
        assert(stop == 3 ? save_delete(root, r.game_id, victim, why, sizeof(why))
                         : save_snapshot(root, &r, NULL, &result));
    }
    /* Stray things in backups/ (a user's file, an oddly named folder, a file named like a game) are not games'
     * backups: they never block launches, and they stay. */
    char stray[MT_PATH_MAX], odd[MT_PATH_MAX], id_file[MT_PATH_MAX];
    path(stray, backup_base, "notes.txt");
    put(stray, "a user's notes");
    path(odd, backup_base, "Odd Name (copy)");
    assert(!mkdir(odd, 0700));
    path(id_file, backup_base, "strayfile");
    put(id_file, "not a folder");
    assert(save_recover(root, NULL, why, sizeof(why)));
    assert(maintenance_launch());
    maintenance_runtime(false, false, false);
    r.kind = SAVE_MANUAL;
    assert(save_snapshot(root, &r, NULL, &result));
    expect(stray, "a user's notes");
    expect(id_file, "not a folder");
    assert(!access(odd, F_OK));
    assert(!unlink(stray) && !rmdir(odd) && !unlink(id_file));
    /* A non-regular .cleanup-next was not created by the service: preserve it. */
    char foreign_next[MT_PATH_MAX];
    path(foreign_next, game_base, ".cleanup-next");
    assert(!mkdir(foreign_next, 0700));
    only_that_game_locked(root, base, &r, game_base, file, "original");
    assert(!access(foreign_next, F_OK));
    assert(!rmdir(foreign_next) && save_recover(root, NULL, why, sizeof(why)));
    puts("killed cleanup-record publication: unpublished slot discarded, launch unlocked, "
         "foreign slot preserved");
    persisted_flags(root, &r);
    puts("namespace separation, all persisted flag bytes, malformed enums, asynchronous "
         "pagination/details/delete, "
         "disabled-backup registration, killed snapshot passed");
}
static void selector_checks(const char *root, const char *base)
{
    char dir[MT_PATH_MAX], slot[MT_PATH_MAX], asset[MT_PATH_MAX], token[MT_PATH_MAX],
        other[MT_PATH_MAX];
    path(dir, base, "selector-native");
    assert(mt_mkdirs(dir));
    path(slot, dir, "one.custom");
    path(asset, dir, "script.rpy");
    path(token, dir, "security_keys.txt");
    path(other, dir, "unrelated-token.txt");
    put(slot, "slot-before");
    put(asset, "asset-before");
    put(token, "key-before");
    put(other, "neighbor-before");
    SaveRequest r = {.kind = SAVE_MANUAL, .core = 1};
    strcpy(r.game_id, "selectors");
    r.layout = (struct dr_save_layout){.version = DR_RUNTIME_VERSION,
                                       .size = sizeof(r.layout),
                                       .count = 2,
                                       .available = true,
                                       .complete = true,
                                       .writers_finished = true};
    r.layout.entries[0] = (struct dr_save_entry){
        .kind = DR_SAVE_RENPY_SLOTS, .scope = DR_SAVE_GAME, .policy = DR_SAVE_READ_WRITE};
    strcpy(r.layout.entries[0].path, dir);
    strcpy(r.layout.entries[0].suffix, ".custom");
    r.layout.entries[1] = (struct dr_save_entry){
        .kind = DR_SAVE_FILE, .scope = DR_SAVE_SHARED, .policy = DR_SAVE_READ_WRITE};
    strcpy(r.layout.entries[1].path, token);
    SaveResult result;
    assert(save_snapshot(root, &r, NULL, &result) && result.files == 2);
    char id[SAVE_ID_MAX];
    strcpy(id, result.snapshot);
    put(slot, "slot-after");
    put(asset, "asset-after");
    put(token, "key-after");
    put(other, "neighbor-after");
    assert(save_restore(root, r.game_id, id, true, NULL, &result));
    expect(slot, "slot-before");
    expect(token, "key-before");
    expect(asset, "asset-after");
    expect(other, "neighbor-after");
    strcpy(r.layout.entries[0].suffix, "../invalid");
    assert(!save_snapshot(root, &r, NULL, &result));
    puts("native suffix and exact token selectors preserve unrelated assets and shared neighbors");
}
static void pending_transaction(const char *journal, const MtTarget *target)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid)
    {
        Halt h = {MT_APPLYING, 1, false};
        MtObserver observer = {NULL, halt_progress, &h};
        char why[256];
        mt_transaction(journal, target, 1, &observer, why, sizeof(why));
        _exit(4);
    }
    int status;
    assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
    assert(!kill(pid, SIGKILL));
    assert(waitpid(pid, &status, 0) == pid);
}
static void fix1_checks(const char *base, int directory_errors)
{
    char root[MT_PATH_MAX], native[MT_PATH_MAX], file[MT_PATH_MAX], child[MT_PATH_MAX],
        nested[MT_PATH_MAX], added[MT_PATH_MAX], dir[MT_PATH_MAX], source[MT_PATH_MAX], why[256];
    path(root, base, "store");
    path(native, base, "native");
    assert(mt_mkdirs(native));
    path(file, native, "slot.custom");
    put(file, "before");
    path(child, native, "other-game/saves");
    assert(mt_mkdirs(child));
    path(nested, child, "unrelated.custom");
    put(nested, "nested-before");
    path(added, child, "new.custom");
    SaveRequest r = {.kind = SAVE_MANUAL, .core = 1};
    strcpy(r.game_id, "scancheck");
    r.layout = (struct dr_save_layout){.version = DR_RUNTIME_VERSION,
                                       .size = sizeof(r.layout),
                                       .count = 1,
                                       .available = true,
                                       .complete = true,
                                       .writers_finished = true};
    struct dr_save_entry *scope = r.layout.entries;
    *scope = (struct dr_save_entry){
        .kind = DR_SAVE_RENPY_SLOTS, .scope = DR_SAVE_GAME, .policy = DR_SAVE_READ_WRITE};
    strcpy(scope->path, native);
    strcpy(scope->suffix, ".custom");
    maintenance_recovered(true);
    SaveResult result;
    assert(save_snapshot(root, &r, NULL, &result));
    char id[SAVE_ID_MAX];
    strcpy(id, result.snapshot);
    if (!directory_errors)
    {
        assert(result.files == 1); /* Descendant is not declared by its parent suffix. */
        put(file, "after");
        put(nested, "nested-after");
        put(added, "added");
        assert(save_restore(root, r.game_id, id, true, NULL, &result));
        expect(file, "before");
        expect(nested, "nested-after");
        expect(added, "added");
        assert(!mt_scope_contains(scope, nested));
        path(dir, native, "asset-link");
        assert(!symlink(child, dir));
        /* A suffix-named directory still is not a native regular save file. */
        path(dir, native, "asset.custom");
        assert(mt_mkdirs(dir));
        char inner[MT_PATH_MAX];
        path(inner, dir, "inner.custom");
        put(inner, "asset");
        assert(save_snapshot(root, &r, NULL, &result) && result.files == 1);
        path(source, base, "source");
        put(source, "journal-new");
        MtTarget t = {.mode = 0644, .scope = *scope};
        t.scope.kind = DR_SAVE_FILE;
        t.scope.suffix[0] = 0;
        strcpy(t.scope.path, nested);
        strcpy(t.target, nested);
        strcpy(t.source, source);
        path(dir, base, "journal");
        pending_transaction(dir, &t);
        assert(!mt_recover(dir, scope, 1, NULL, why, sizeof(why)));
        expect(nested, "journal-new");
        assert(mt_recover(dir, &t.scope, 1, NULL, why, sizeof(why)));
        expect(nested, "nested-after");
        puts("direct-child capture, descendant preservation, and journal authorization passed");
        return;
    }
    if (directory_errors == 1)
    {
        assert(save_snapshot(root, &r, NULL, &result));
        path(dir, root, "backups/scancheck");
        fail_directory_read(dir, 1);
        SaveInfo info[8];
        size_t count = 99;
        assert(!save_list(root, r.game_id, info, 8, &count, why, sizeof(why)));
        assert(count == 0 && read_faults == 1);
        assert(save_list(root, r.game_id, info, 8, &count, why, sizeof(why)) && count == 2);
        strcpy(r.game_id, "other-scan");
        assert(save_snapshot(root, &r, NULL, &result));
        path(dir, root, "save-layouts");
        fail_directory_read(dir, 1);
        assert(!save_details(root, "scancheck", id, NULL, NULL, why, sizeof(why)));
        assert(read_faults == 2);
        fail_directory_read(native, 1);
        assert(!save_snapshot(root, &r, NULL, &result));
        assert(read_faults == 3);
        expect(file, "before");
    }
    path(source, base, "source");
    put(source, "after");
    MtTarget t = {.scope = *scope, .mode = 0644};
    strcpy(t.target, file);
    strcpy(t.source, source);
    char journal[MT_PATH_MAX];
    path(journal, root, "save-transactions/scancheck");
    pending_transaction(journal, &t);
    expect(file, "after");
    maintenance_recovered(false);
    path(dir, root, "save-transactions");
    fail_directory_read(dir, 0);
    assert(!save_recover(root, NULL, why, sizeof(why)));
    assert(read_faults == (directory_errors == 1 ? 4u : 1u) && !maintenance_ready() &&
           !maintenance_launch());
    assert(!maintenance_acquire(MAINT_UPDATE));
    expect(file, "after");
    path(dir, journal, "journal");
    assert(!access(dir, F_OK));
    assert(save_recover(root, NULL, why, sizeof(why)) && maintenance_ready());
    expect(file, "before");
    puts("readdir EIO rejects truncation and keeps pending recovery locked");
}
/* Production helper declarations from 40 distinct process temporary suffixes. */
static void repeated_native_checks(const char *base, const char *declarations)
{
    char root[MT_PATH_MAX], native[MT_PATH_MAX], slot[MT_PATH_MAX], sibling[MT_PATH_MAX],
        temporary[MT_PATH_MAX], invalid[MT_PATH_MAX], catalog[MT_PATH_MAX], why[256];
    path(root, base, "store");
    path(native, base, "native");
    path(slot, native, "nested/explicit.custom");
    path(sibling, native, "nested/other.custom");
    put(sibling, "unrelated");
    path(invalid, native, "nested/explicit.custom.bad.tmp");
    put(invalid, "not-native");
    char persistent[MT_PATH_MAX];
    path(persistent, native, "persistent");
    put(persistent, "persistent-before");
    path(catalog, root, "save-layouts/repeated");
    FILE *input = fopen(declarations, "r");
    assert(input);
    maintenance_recovered(true);
    SaveRequest r = {.kind = SAVE_AUTOMATIC, .core = 1};
    strcpy(r.game_id, "repeated");
    char line[4096], first_id[SAVE_ID_MAX] = "";
    unsigned iterations = 0;
    while (fgets(line, sizeof(line), input))
    {
        unsigned count = (unsigned)strtoul(line, NULL, 10);
        assert(count && count <= DR_SAVE_MAX);
        r.layout = (struct dr_save_layout){.version = DR_RUNTIME_VERSION,
                                           .size = sizeof(r.layout),
                                           .count = count,
                                           .available = true,
                                           .complete = true,
                                           .writers_finished = true};
        for (unsigned i = 0; i < count; ++i)
        {
            assert(fgets(line, sizeof(line), input));
            struct dr_save_entry *e = r.layout.entries + i;
            assert(sscanf(line, "%u %u %u\t%1023[^\t]\t%127[^\n]", &e->kind, &e->scope, &e->policy,
                          e->path, e->suffix) >= 4);
        }
        char bytes[64];
        snprintf(bytes, sizeof(bytes), "native-%u", iterations);
        put(slot, bytes);
        snprintf(temporary, sizeof(temporary), "%s.%u.tmp", slot, 100000 + iterations);
        put(temporary, "native-rename-temp");
        assert(!unlink(temporary)); /* completed clean stop */
        r.session = ++iterations;
        SaveResult result;
        if (!save_snapshot(root, &r, NULL, &result))
        {
            fprintf(stderr, "native catalog failed at process %u: %s\n", iterations, result.error);
            abort();
        }
        if (iterations == 1)
        {
            r.kind = SAVE_MANUAL;
            assert(save_snapshot(root, &r, NULL, &result));
            strcpy(first_id, result.snapshot);
            r.kind = SAVE_AUTOMATIC;
        }
        expect(sibling, "unrelated");
        expect(invalid, "not-native");
    }
    fclose(input);
    assert(iterations > DR_SAVE_MAX);
    struct dr_save_layout persisted;
    FILE *f = fopen(catalog, "rb");
    assert(f);
    assert(fread(&persisted, 1, sizeof(persisted), f) == sizeof(persisted));
    fclose(f);
    assert(persisted.count == 2); /* parent slots + stable exact native file artifacts */
    SaveResult result;
    assert(save_restore(root, r.game_id, first_id, true, NULL, &result));
    expect(slot, "native-0");
    expect(persistent, "persistent-before");
    expect(sibling, "unrelated");
    expect(invalid, "not-native");
    path(temporary, native, "nested/explicit.custom.999999.tmp");
    put(temporary, "temporary-before");
    const struct dr_save_entry *authorized = NULL;
    for (uint32_t i = 0; i < persisted.count; ++i)
        if (mt_scope_contains(persisted.entries + i, temporary))
            authorized = persisted.entries + i;
    assert(authorized && !mt_scope_contains(authorized, sibling) &&
           !mt_scope_contains(authorized, invalid));
    assert(authorized->kind == DR_SAVE_RENPY_FILE);
    char new_file[MT_PATH_MAX], artifact_id[SAVE_ID_MAX];
    snprintf(new_file, sizeof(new_file), "%s.new", slot);
    put(new_file, "new-before");
    assert(save_snapshot(root, &r, NULL, &result));
    strcpy(artifact_id, result.snapshot);
    put(temporary, "damaged");
    put(new_file, "damaged");
    assert(save_restore(root, r.game_id, artifact_id, true, NULL, &result));
    expect(temporary, "temporary-before");
    expect(new_file, "new-before");
    SaveRequest alias = r;
    strcpy(alias.game_id, "artifact-alias");
    alias.kind = SAVE_MANUAL;
    alias.layout.count = 1;
    alias.layout.entries[0] = *authorized;
    alias.layout.entries[0].kind = DR_SAVE_FILE;
    strcpy(alias.layout.entries[0].path, temporary);
    assert(save_snapshot(root, &alias, NULL, &result));
    SaveInfo info;
    assert(save_details(root, r.game_id, artifact_id, &info, NULL, why, sizeof(why)));
    assert(info.shared);
    char source[MT_PATH_MAX], journal[MT_PATH_MAX];
    path(source, base, "replacement");
    put(source, "replacement");
    MtTarget t = {.scope = *authorized, .mode = 0644};
    strcpy(t.target, temporary);
    strcpy(t.source, source);
    path(journal, root, "save-transactions/repeated");
    pending_transaction(journal, &t);
    maintenance_recovered(false);
    assert(save_recover(root, NULL, why, sizeof(why)));
    expect(temporary, "temporary-before");
    expect(slot, "native-0");
    expect(persistent, "persistent-before");
    expect(sibling, "unrelated");
    t.scope = alias.layout.entries[0];
    strcpy(t.scope.path, sibling);
    strcpy(t.target, sibling);
    pending_transaction(journal, &t);
    assert(!mt_recover(journal, persisted.entries, persisted.count, NULL, why, sizeof(why)));
    expect(sibling, "replacement");
    assert(mt_recover(journal, &t.scope, 1, NULL, why, sizeof(why)));
    expect(sibling, "unrelated");
    puts("40 native process suffixes keep bounded catalog, completed snapshots and artifact "
         "recovery");
}
int main(int argc, char **argv)
{
    assert(argc >= 2 && argc <= 4);
    if (argc == 4)
    {
        repeated_native_checks(argv[1], argv[3]);
        return 0;
    }
    if (argc == 3)
    {
        fix1_checks(argv[1], !strcmp(argv[2], "readdir")    ? 1
                             : !strcmp(argv[2], "recovery") ? 2
                                                            : 0);
        return 0;
    }
    assert(!saves_start(SAVES_RECOVER, NULL, NULL, false));
    char invalid_root[256];
    assert(!save_recover("", NULL, invalid_root, sizeof(invalid_root)));
    char root[MT_PATH_MAX], data[MT_PATH_MAX], file[MT_PATH_MAX];
    assert(mt_path(root, argv[1], "store") && mt_path(data, argv[1], "native") && mt_mkdirs(data));
    assert(mt_path(file, data, "Save01.lsd"));
    put(file, "old");
    SaveRequest r = {0};
    strcpy(r.game_id, "fixture");
    r.core = 0;
    r.kind = SAVE_AUTOMATIC;
    r.layout = (struct dr_save_layout){.version = DR_RUNTIME_VERSION,
                                       .size = sizeof(r.layout),
                                       .available = true,
                                       .complete = true,
                                       .count = 1,
                                       .writers_finished = true};
    strcpy(r.layout.entries[0].path, data);
    r.layout.entries[0].kind = DR_SAVE_RPG_SLOTS;
    r.layout.entries[0].scope = DR_SAVE_GAME;
    r.layout.entries[0].policy = DR_SAVE_READ_WRITE;
    maintenance_recovered(true);
    SaveResult result = {0};
    assert(save_snapshot(root, &r, NULL, &result));
    assert(!result.unchanged);
    char id[64];
    strcpy(id, result.snapshot);
    assert(save_snapshot(root, &r, NULL, &result) && result.unchanged);
    put(file, "new");
    assert(save_restore(root, "fixture", id, true, NULL, &result));
    expect(file, "old");
    puts("snapshot dedup and verified restore passed");
    transaction_checks(argv[1]);
    extra_checks(root, data, &r, file);
    early_checks(root, argv[1]);
    namespace_and_ui_checks(root, argv[1]);
    selector_checks(root, argv[1]);
    maintenance_runtime(true, false, true);
    assert(!save_snapshot(root, &r, NULL, &result));
    assert(!save_restore(root, r.game_id, id, true, NULL, &result));
    assert(!maintenance_launch());
    puts("sticky unsafe runtime rejects copy, restore and relaunch");
    return 0;
}
