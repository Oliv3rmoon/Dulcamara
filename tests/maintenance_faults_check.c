#include "maintenance.h"
#include "saves.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
static const char *fault_mode = "";
static bool cleanup_fault;
int probe_test_mkdir(const char *path, mode_t mode)
{
    int result = mkdir(path, mode);
    if (!result && !strcmp(fault_mode, "probe-empty") && strstr(path, ".eclipse-names-"))
        raise(SIGSTOP);
    return result;
}
int probe_test_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    int fd = open(path, flags, mode);
    if (fd >= 0 && (flags & O_CREAT) && !strcmp(fault_mode, "probe-filled") &&
        strstr(path, ".eclipse-names-") && strncmp(strrchr(path, '/') + 1, ".owner-", 7))
        raise(SIGSTOP);
    return fd;
}
int probe_test_unlink(const char *path)
{
    if (cleanup_fault && strstr(path, ".eclipse-names-") &&
        strncmp(strrchr(path, '/') + 1, ".owner-", 7))
    {
        errno = EIO;
        return -1;
    }
    int result = unlink(path);
    if (!result && ((!strcmp(fault_mode, "probe-plan-removed") &&
                     !strcmp(strrchr(path, '/') + 1, "probe-plan")) ||
                    (!strcmp(fault_mode, "probe-journal-removed") &&
                     !strcmp(strrchr(path, '/') + 1, "journal"))))
        raise(SIGSTOP);
    return result;
}
static int owned_probes(const char *parent)
{
    DIR *d = opendir(parent);
    assert(d);
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, ".eclipse-names-", 15) &&
            strcmp(e->d_name, ".eclipse-names-unrelated"))
            ++count;
    closedir(d);
    return count;
}
static void put(const char *path, const char *bytes)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes));
    assert(!fclose(f));
}
static int same_bytes(const char *path, const char *bytes)
{
    char b[64] = {0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    return n == strlen(bytes) && !memcmp(b, bytes, n);
}
static void join(char *out, const char *a, const char *b)
{
    assert(mt_path(out, a, b));
}
static void halt(void *context, MtPhase phase, uint64_t done, uint64_t total)
{
    (void)context;
    (void)total;
    if (phase == MT_APPLYING && done == 1)
        raise(SIGSTOP);
}
static void halt_prepared(void *ctx, MtPhase phase, uint64_t done, uint64_t total)
{
    (void)ctx;
    (void)total;
    if (phase == MT_PREPARING && done == 1)
        raise(SIGSTOP);
}
static int callback_calls;
static bool extra_recovery(void *ctx, bool *restart, char *error, size_t size)
{
    (void)ctx;
    (void)error;
    (void)size;
    assert(!maintenance_ready() && !maintenance_launch() && !maintenance_restart_ready());
    assert(!maintenance_acquire(MAINT_UPDATE));
    *restart = callback_calls++ == 0;
    return callback_calls > 1;
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    char root[MT_PATH_MAX];
    assert(mt_canonical(argv[1], root));
    assert(mt_mkdirs(root));
    if (!strcmp(argv[2], "barrier"))
    {
        maintenance_recovery_begin(MAINT_RECOVER_SAVES | MAINT_RECOVER_UPDATE);
        maintenance_recovered(true);
        assert(!maintenance_launch());
        maintenance_recovery_finish(MAINT_RECOVER_UPDATE, false);
        assert(!maintenance_launch());
        maintenance_recovery_finish(MAINT_RECOVER_UPDATE, true);
        assert(maintenance_launch());
        maintenance_runtime(false, false, false);
        maintenance_recovery_begin(MAINT_RECOVER_SAVES | MAINT_RECOVER_UPDATE);
        assert(!saves_init("relative-invalid-root"));
        assert(!maintenance_launch());
        saves_set_update_recovery(extra_recovery, NULL);
        char why[256];
        assert(!save_recover(root, NULL, why, sizeof(why)));
        assert(!maintenance_launch() && !maintenance_restart_ready());
        assert(save_recover(root, NULL, why, sizeof(why)));
        assert(callback_calls == 2);
        assert(!maintenance_launch() && maintenance_restart_ready());
        puts("PASS first-finisher/failure/start-failure/retry and sticky restart obligation");
        return 0;
    }
    char native[MT_PATH_MAX], journal[MT_PATH_MAX], sources[2][MT_PATH_MAX];
    join(native, root, "native");
    assert(mt_mkdirs(native));
    join(journal, root, "journal");
    struct dr_save_entry scope = {
        .kind = DR_SAVE_DIRECTORY, .scope = DR_SAVE_GAME, .policy = DR_SAVE_READ_WRITE};
    strcpy(scope.path, native);
    MtTarget t[2] = {0};
    for (int i = 0; i < 2; ++i)
    {
        t[i].scope = scope;
        t[i].mode = 0600;
        join(sources[i], root, i ? "source-b" : "source-a");
        put(sources[i], i ? "new-b" : "new-a");
        strcpy(t[i].source, sources[i]);
    }
    join(t[0].target, native, strstr(argv[2], "unicode") ? "é" : "a");
    join(t[1].target, native,
         !strncmp(argv[2], "collision", 9) ? (strstr(argv[2], "long-s")    ? "a.eclipſe-stage"
                                              : strstr(argv[2], "unicode") ? "É.eclipse-stage"
                                                                           : "a.eclipse-stage")
                                           : "b");
    char error[256] = "";
    if (!strcmp(argv[2], "probe-recovery"))
    {
        put(t[0].target, "old-a");
        put(t[1].target, "old-b");
        for (unsigned step = 0; step < 2; ++step)
        {
            pid_t pid = fork();
            assert(pid >= 0);
            if (!pid)
            {
                if (!step)
                {
                    MtObserver o = {.progress = halt_prepared};
                    _exit(mt_transaction(journal, t, 2, &o, error, sizeof(error)) ? 0 : 2);
                }
                fault_mode = "probe-filled";
                _exit(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)) ? 0 : 2);
            }
            int status;
            assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
            kill(pid, SIGKILL);
            assert(waitpid(pid, &status, 0) == pid);
        }
        assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
        char artifact[MT_PATH_MAX];
        join(artifact, native, "a.eclipse-stage");
        assert(access(artifact, F_OK));
        join(artifact, journal, "0.old");
        assert(access(artifact, F_OK));
        assert(same_bytes(t[0].target, "old-a") && same_bytes(t[1].target, "old-b"));
        assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
        puts("PASS interrupted recovery probe retains staged/original cleanup obligation");
        return 0;
    }
    if (!strncmp(argv[2], "probe-", 6))
    {
        char unrelated[MT_PATH_MAX], keep[MT_PATH_MAX];
        join(unrelated, native, ".eclipse-names-unrelated");
        assert(mt_mkdirs(unrelated));
        join(keep, unrelated, "keep");
        put(keep, "preserve");
        put(t[0].target, "old-a");
        put(t[1].target, "old-b");
        for (unsigned retry = 0; retry < 3; ++retry)
        {
            if (!strcmp(argv[2], "probe-cleanup"))
            {
                cleanup_fault = true;
                assert(!mt_transaction(journal, t, 2, NULL, error, sizeof(error)));
                assert(owned_probes(native) == 1);
                assert(!mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
                cleanup_fault = false;
            }
            else
            {
                pid_t pid = fork();
                assert(pid >= 0);
                if (!pid)
                {
                    fault_mode = argv[2];
                    _exit(mt_transaction(journal, t, 2, NULL, error, sizeof(error)) ? 0 : 2);
                }
                int status;
                assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
                kill(pid, SIGKILL);
                assert(waitpid(pid, &status, 0) == pid);
                assert(owned_probes(native) == 1);
            }
            assert(same_bytes(t[0].target, "old-a") && same_bytes(t[1].target, "old-b"));
            assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
            assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
            assert(owned_probes(native) == 0 && same_bytes(keep, "preserve"));
            assert(same_bytes(t[0].target, "old-a") && same_bytes(t[1].target, "old-b"));
        }
        puts("PASS registered probe interruption/cleanup retry is bounded; targets and unrelated "
             "prefix preserved");
        return 0;
    }
    if (strstr(argv[2], "removed"))
    {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid)
        {
            fault_mode =
                strstr(argv[2], "journal") ? "probe-journal-removed" : "probe-plan-removed";
            _exit(mt_transaction(journal, t, 2, NULL, error, sizeof(error)) ? 0 : 2);
        }
        int status;
        assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
        kill(pid, SIGKILL);
        assert(waitpid(pid, &status, 0) == pid);
        assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
        assert(mt_recover(journal, &scope, 1, NULL, error, sizeof(error)));
        assert(access(t[0].target, F_OK) && access(t[1].target, F_OK));
        assert(owned_probes(native) == 0);
        puts("PASS interrupted collision rejection finishes metadata without touching targets");
        return 0;
    }
    if (!strncmp(argv[2], "collision", 9))
    {
        char unrelated[MT_PATH_MAX];
        join(unrelated, native, "unrelated");
        put(unrelated, "preserve");
        if (strstr(argv[2], "reverse"))
        {
            MtTarget swap = t[0];
            t[0] = t[1];
            t[1] = swap;
        }
        int ok = mt_transaction(journal, t, 2, NULL, error, sizeof(error));
        int a = same_bytes(t[0].target, "new-a"), b = same_bytes(t[1].target, "new-b");
        printf("{\"case\":\"stage_destination_collision\",\"transaction_ok\":%d,\"a_new\":%d,\"b_"
               "new\":%d,\"b_exists\":%d}\n",
               ok, a, b, access(t[1].target, F_OK) == 0);
        assert(same_bytes(unrelated, "preserve"));
        return (ok ? (a && b) : (access(t[0].target, F_OK) != 0 && access(t[1].target, F_OK) != 0))
                   ? 0
                   : 1;
    }
    put(t[0].target, "old-a");
    put(t[1].target, "old-b");
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid)
    {
        MtObserver observer = {.progress = halt};
        _exit(mt_transaction(journal, t, 2, &observer, error, sizeof(error)) ? 0 : 2);
    }
    int status;
    assert(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
    assert(!kill(pid, SIGKILL));
    assert(waitpid(pid, &status, 0) == pid);
    char record[MT_PATH_MAX], saved[MT_PATH_MAX];
    join(record, journal, "journal");
    join(saved, root, "preserved-journal.bin");
    assert(mt_copy(record, saved, NULL));
    assert(!unlink(record));
    assert(mt_sync_dir(journal));
    maintenance_recovered(false);
    assert(maintenance_acquire(MAINT_RECOVERY));
    int ok = mt_recover(journal, &scope, 1, NULL, error, sizeof(error));
    maintenance_recovered(ok);
    maintenance_release(MAINT_RECOVERY, false);
    int a = same_bytes(t[0].target, "new-a"), b = same_bytes(t[1].target, "old-b"),
        ready = maintenance_ready();
    printf("{\"case\":\"missing_pending_journal\",\"recover_ok\":%d,\"first_new\":%d,\"second_"
           "old\":%d,\"consumer_gate_ready\":%d}\n",
           ok, a, b, ready);
    return !ok && !ready ? 0 : 1;
}
