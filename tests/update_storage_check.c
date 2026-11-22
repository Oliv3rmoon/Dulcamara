/* Production installer driver; temporary roots supplied by Python. */
#include "update_storage.h"
#include "maintenance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <assert.h>
static int low_space, write_fault, apply_fault, read_fault, sync_fault;
static int cleanup_fault, cleanup_failures;
static struct stat installed_stat;
static char workspace[MT_PATH_MAX];
int update_test_unlink(const char *path)
{
    size_t n = strlen(path);
    if (cleanup_fault && n >= 4 && !strcmp(path + n - 4, ".old"))
    {
        ++cleanup_failures;
        if (cleanup_fault == 1)
            cleanup_fault = 0;
        errno = EIO;
        return -1;
    }
    return unlink(path);
}
struct dirent *update_test_readdir(DIR *d)
{
    if (read_fault)
    {
        read_fault = 0;
        errno = EIO;
        return NULL;
    }
    return readdir(d);
}
int update_test_fsync(int fd)
{
    struct stat st;
    if (sync_fault && !fstat(fd, &st) && st.st_dev == installed_stat.st_dev &&
        st.st_ino == installed_stat.st_ino && access(workspace, F_OK))
    {
        if (sync_fault == 1)
            sync_fault = 0;
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
int update_test_rename(const char *a, const char *b)
{
    if (apply_fault && strstr(a, ".eclipse-stage") && strstr(b, "eboot.bin"))
    {
        apply_fault = 0;
        errno = EIO;
        return -1;
    }
    return rename(a, b);
}
int update_test_statvfs(const char *p, struct statvfs *s)
{
    int n = statvfs(p, s);
    if (!n && low_space)
        s->f_bavail =
            low_space == 2 ? (1024ull * 1024 * 1024) / (s->f_frsize ? s->f_frsize : s->f_bsize) : 1;
    return n;
}
ssize_t update_test_write(int fd, const void *b, size_t n)
{
    if (write_fault)
    {
        errno = ENOSPC;
        return -1;
    }
    return write(fd, b, n);
}
static void progress(void *arg, MtPhase phase, uint64_t done, uint64_t total)
{
    const char *mode = arg;
    printf("phase %d %llu/%llu\n", phase, (unsigned long long)done, (unsigned long long)total);
    fflush(stdout);
    if (phase == MT_COMMITTED && !strncmp(mode, "cleanup-", 8))
        cleanup_fault = !strcmp(mode, "cleanup-once") ? 1 : 2;
    if ((!strcmp(mode, "stage") && phase == MT_PREPARING && done == 1) ||
        (!strcmp(mode, "apply") && phase == MT_APPLYING && done == 1) ||
        (!strcmp(mode, "commit") && phase == MT_COMMITTED))
        raise(SIGSTOP);
}
int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    if (stat(argv[1], &installed_stat) || !mt_path(workspace, argv[1], ".eclipse-update"))
        return 3;
    read_fault = !strcmp(argv[3], "recover-readdir");
    sync_fault = !strcmp(argv[3], "recover-sync")          ? 1
                 : !strcmp(argv[3], "recover-sync-always") ? 2
                                                           : 0;
    char error[256] = "";
    bool restart = false, ok;
    maintenance_recovery_begin(MAINT_RECOVER_SAVES | MAINT_RECOVER_UPDATE);
    maintenance_recovered(true);
    maintenance_recovery_finish(MAINT_RECOVER_UPDATE, true);
    if (!strcmp(argv[3], "runtime"))
        maintenance_launch();
    if (!strcmp(argv[3], "restore"))
        maintenance_acquire(MAINT_RESTORE);
    apply_fault = !strcmp(argv[3], "apply-write");
    low_space = !strcmp(argv[3], "space-large") ? 2 : !strcmp(argv[3], "space");
    write_fault = !strcmp(argv[3], "write");
    MtObserver o = {.progress = progress, .context = argv[3]};
    if (!strncmp(argv[3], "recover", 7))
    {
        maintenance_recovered(false);
        maintenance_acquire(MAINT_RECOVERY);
        ok = update_storage_recover(argv[1], &restart, error, sizeof(error));
        maintenance_recovered(ok);
        maintenance_release(MAINT_RECOVERY, restart);
    }
    else
        ok = update_storage_apply(argv[2], argv[1], "2.0.0", &o, error, sizeof(error));
    if (!strncmp(argv[3], "cleanup-", 8))
    {
        assert(cleanup_failures == (!strcmp(argv[3], "cleanup-once") ? 1 : 2));
        assert(!maintenance_launch());
        printf("cleanup failures %d launch locked\n", cleanup_failures);
    }
    printf("result %d ready %d restart_ready %d restart %d error %s\n", ok, maintenance_ready(),
           maintenance_restart_ready(), restart, error);
    return ok ? 0 : 1;
}
