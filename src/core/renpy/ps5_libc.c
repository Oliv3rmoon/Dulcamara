/*
 * Dulcamara - Ren'Py's core on the PS5: the C library functions Python and its modules call that the console's libc
 * has not, or has only in libScePosixForWebKit (which a native title does not load, so its calls would jump to
 * address 0). tools/build-renpy.sh found them by what the core leaves undefined. Each does what a sandboxed console
 * can: the nearest call it has, or the error a POSIX system gives for what it cannot do. They are local to the core's
 * archive, so the app's own stand-ins (src/platform/ps5_shims.c: dirfd, nl_langinfo) are not touched.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- time */

/* gmtime and localtime return one shared struct: copied out under a lock */
static pthread_mutex_t time_lock = PTHREAD_MUTEX_INITIALIZER;

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    pthread_mutex_lock(&time_lock);
    const struct tm *shared = gmtime(t);
    if (shared)
        *out = *shared;
    pthread_mutex_unlock(&time_lock);
    return shared ? out : NULL;
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    pthread_mutex_lock(&time_lock);
    const struct tm *shared = localtime(t);
    if (shared)
        *out = *shared;
    pthread_mutex_unlock(&time_lock);
    return shared ? out : NULL;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain)
{
    struct timespec wait = *request;
    if (flags & TIMER_ABSTIME)
    {
        struct timespec now;
        if (clock_gettime(clock, &now) != 0)
            return errno;
        wait.tv_sec -= now.tv_sec;
        wait.tv_nsec -= now.tv_nsec;
        if (wait.tv_nsec < 0)
        {
            wait.tv_nsec += 1000000000;
            --wait.tv_sec;
        }
        if (wait.tv_sec < 0)
            return 0;
        remain = NULL;
    }
    return nanosleep(&wait, remain) == 0 ? 0 : errno;
}

clock_t times(struct tms *buffer)
{
    const clock_t used = clock();
    if (buffer)
    {
        buffer->tms_utime = used;
        buffer->tms_stime = 0;
        buffer->tms_cutime = 0;
        buffer->tms_cstime = 0;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (clock_t)((long long)now.tv_sec * CLOCKS_PER_SEC + (long long)now.tv_nsec / (1000000000 / CLOCKS_PER_SEC));
}

int __flt_rounds(void)
{
    return 1; /* to nearest */
}

int getloadavg(double loads[], int count)
{
    (void)loads, (void)count;
    return -1;
}

/* ---------------------------------------------------------------- files */

int pipe2(int fds[2], int flags)
{
    if (pipe(fds) != 0)
        return -1;
    for (int i = 0; i < 2; ++i)
    {
        if (flags & O_CLOEXEC)
            fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (flags & O_NONBLOCK)
            fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}

int faccessat(int dir, const char *path, int mode, int flags)
{
    (void)flags;
    if (dir != AT_FDCWD && path[0] != '/')
    {
        errno = ENOSYS;
        return -1;
    }
    return access(path, mode);
}

ssize_t readlinkat(int dir, const char *path, char *buffer, size_t size)
{
    if (dir != AT_FDCWD && path[0] != '/')
    {
        errno = ENOSYS;
        return -1;
    }
    return readlink(path, buffer, size);
}

int utimensat(int dir, const char *path, const struct timespec times[2], int flags)
{
    (void)flags;
    if (dir != AT_FDCWD && path[0] != '/')
    {
        errno = ENOSYS;
        return -1;
    }
    if (!times)
        return utimes(path, NULL);
    struct timeval tv[2] = {{times[0].tv_sec, times[0].tv_nsec / 1000}, {times[1].tv_sec, times[1].tv_nsec / 1000}};
    return utimes(path, tv);
}

int futimens(int fd, const struct timespec times[2])
{
    (void)fd, (void)times;
    errno = ENOSYS;
    return -1;
}

int posix_fadvise(int fd, off_t offset, off_t length, int advice)
{
    (void)fd, (void)offset, (void)length, (void)advice;
    return 0; /* advice only */
}

int posix_fallocate(int fd, off_t offset, off_t length)
{
    (void)fd, (void)offset, (void)length;
    return EOPNOTSUPP;
}

static int unsupported(void)
{
    errno = ENOSYS;
    return -1;
}

int mkfifo(const char *path, mode_t mode)
{
    (void)path, (void)mode;
    return unsupported();
}

int mkfifoat(int dir, const char *path, mode_t mode)
{
    (void)dir, (void)path, (void)mode;
    return unsupported();
}

int mknod(const char *path, mode_t mode, dev_t device)
{
    (void)path, (void)mode, (void)device;
    return unsupported();
}

int mknodat(int dir, const char *path, mode_t mode, dev_t device)
{
    (void)dir, (void)path, (void)mode, (void)device;
    return unsupported();
}

void closefrom(int lowest)
{
    for (int fd = lowest; fd < 1024; ++fd)
        close(fd);
}

int isatty(int fd)
{
    (void)fd;
    errno = ENOTTY;
    return 0;
}

/* ---------------------------------------------------------------- processes and users (there are none to change) */

int fexecve(int fd, char *const argv[], char *const envp[])
{
    (void)fd, (void)argv, (void)envp;
    return unsupported();
}

pid_t getpgrp(void)
{
    return getpid();
}

pid_t getpgid(pid_t pid)
{
    return pid ? pid : getpid();
}

int setgid(gid_t gid)
{
    (void)gid;
    errno = EPERM;
    return -1;
}

int setresgid(gid_t real, gid_t effective, gid_t saved)
{
    (void)real, (void)effective, (void)saved;
    errno = EPERM;
    return -1;
}

int setresuid(uid_t real, uid_t effective, uid_t saved)
{
    (void)real, (void)effective, (void)saved;
    errno = EPERM;
    return -1;
}

int getresgid(gid_t *real, gid_t *effective, gid_t *saved)
{
    *real = *effective = *saved = 0;
    return 0;
}

int getresuid(uid_t *real, uid_t *effective, uid_t *saved)
{
    *real = *effective = *saved = 0;
    return 0;
}

int getpwnam_r(const char *name, struct passwd *entry, char *buffer, size_t size, struct passwd **result)
{
    (void)name, (void)entry, (void)buffer, (void)size;
    *result = NULL; /* no such user */
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result)
{
    (void)uid, (void)entry, (void)buffer, (void)size;
    *result = NULL;
    return 0;
}

char *strsignal(int number)
{
    static char text[32];
    snprintf(text, sizeof(text), "Signal %d", number);
    return text;
}

/* ---------------------------------------------------------------- names: the console's resolver is not used */

int *__h_errno(void)
{
    static _Thread_local int error;
    return &error;
}

const char *hstrerror(int error)
{
    (void)error;
    return "Name resolution is not available";
}

struct hostent *gethostbyname(const char *name)
{
    (void)name;
    *__h_errno() = HOST_NOT_FOUND;
    return NULL;
}

struct hostent *gethostbyaddr(const void *address, socklen_t length, int family)
{
    (void)address, (void)length, (void)family;
    *__h_errno() = HOST_NOT_FOUND;
    return NULL;
}

struct protoent *getprotobyname(const char *name)
{
    (void)name;
    return NULL;
}

struct servent *getservbyname(const char *name, const char *protocol)
{
    (void)name, (void)protocol;
    return NULL;
}

struct servent *getservbyport(int port, const char *protocol)
{
    (void)port, (void)protocol;
    return NULL;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
    (void)node, (void)service, (void)hints;
    *result = NULL;
    return EAI_FAIL;
}

void freeaddrinfo(struct addrinfo *list)
{
    (void)list; /* getaddrinfo never makes one */
}

const char *gai_strerror(int error)
{
    (void)error;
    return "Name resolution is not available";
}

int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, size_t host_size, char *service,
                size_t service_size, int flags)
{
    (void)address, (void)length, (void)host, (void)host_size, (void)service, (void)service_size, (void)flags;
    return EAI_FAIL;
}

#endif
