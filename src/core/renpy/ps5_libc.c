/*
 * Dulcamara - Ren'Py's cores on the PS5: the C library functions Python and its modules call that the console's libc
 * has not, or has only in libScePosixForWebKit (which a native title does not load, so its calls would jump to
 * address 0). tools/build-renpy.sh and build-renpy7.sh found them by what each core leaves undefined (Python 3.12 and
 * 2.7 call different ones). Each does what a sandboxed console can: the nearest call it has, or the error a POSIX
 * system gives for what it cannot do. They are local to each core's archive, so the app's own stand-ins
 * (src/platform/ps5_shims.c, which the cores use too: dirfd, nl_langinfo, uselocale, strcasestr) are not touched.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <pwd.h>
#include <runetype.h>
#include <signal.h>
#include <stdbool.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- the environment: the core's own */

/* A title's environment is not one to rely on: libkernel's environ (which Python copies into os.environ as it starts,
 * reading every entry) is the console's to set up, and libSceLibcInternal's setenv keeps its own. Dulcamara 0.2.0
 * crashed on the console a moment after Ren'Py's thread started, in a system library reading a string through a bad
 * pointer, which is where that copy reads. The core keeps its own environment instead, for Python's os.environ,
 * SDL's hints and FFmpeg's settings alike: a few entries, its own strings. */
#define ENVIRONMENT_SIZE 64
static pthread_mutex_t environment_lock = PTHREAD_MUTEX_INITIALIZER;
static char *entries[ENVIRONMENT_SIZE + 1]; /* "NAME=value", then NULL */
char **environ = entries;

/* the entry for name (n bytes long), or -1 */
static int entry_of(const char *name, size_t n)
{
    for (int i = 0; entries[i]; ++i)
        if (!strncmp(entries[i], name, n) && entries[i][n] == '=')
            return i;
    return -1;
}

static bool valid_name(const char *name)
{
    return name && *name && !strchr(name, '=');
}

char *getenv(const char *name)
{
    if (!valid_name(name))
        return NULL;
    pthread_mutex_lock(&environment_lock);
    const size_t n = strlen(name);
    const int i = entry_of(name, n);
    char *value = i >= 0 ? entries[i] + n + 1 : NULL;
    pthread_mutex_unlock(&environment_lock);
    return value;
}

/* entry ("NAME=value", which the environment keeps) for name; an entry it replaces is kept too, as a getenv's answer
 * may still be in use */
static int put_entry(const char *name, size_t n, char *entry, bool overwrite)
{
    pthread_mutex_lock(&environment_lock);
    int i = entry_of(name, n), result = 0;
    if (i >= 0)
    {
        if (overwrite)
            entries[i] = entry;
    }
    else
    {
        for (i = 0; entries[i]; ++i)
            ;
        if (i < ENVIRONMENT_SIZE)
        {
            entries[i] = entry;
            entries[i + 1] = NULL;
        }
        else
        {
            errno = ENOMEM;
            result = -1;
        }
    }
    pthread_mutex_unlock(&environment_lock);
    return result;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!valid_name(name) || !value)
    {
        errno = EINVAL;
        return -1;
    }
    const size_t n = strlen(name), size = n + strlen(value) + 2;
    char *entry = malloc(size);
    if (!entry)
    {
        errno = ENOMEM;
        return -1;
    }
    snprintf(entry, size, "%s=%s", name, value);
    return put_entry(name, n, entry, overwrite != 0);
}

int putenv(char *string)
{
    const char *equals = string ? strchr(string, '=') : NULL;
    if (!equals || equals == string)
    {
        errno = EINVAL;
        return -1;
    }
    return put_entry(string, (size_t)(equals - string), string, true);
}

int unsetenv(const char *name)
{
    if (!valid_name(name))
    {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&environment_lock);
    const size_t n = strlen(name);
    for (int i = entry_of(name, n); i >= 0; i = entry_of(name, n))
        for (int j = i; entries[j]; ++j)
            entries[j] = entries[j + 1];
    pthread_mutex_unlock(&environment_lock);
    return 0;
}

/* ---------------------------------------------------------------- the locale */

/* The SDK's headers have no thread-local locale (__NO_TLS), so <ctype.h>'s inline tolower and isalpha (Python 2.7's
 * string methods) ask this for the locale; the console's libc has the locale but not the call. */
#undef _CurrentRuneLocale /* runetype.h makes the name this call: here it is the console's variable */
const _RuneLocale *__getCurrentRuneLocale(void)
{
    return _CurrentRuneLocale ? _CurrentRuneLocale : &_DefaultRuneLocale;
}

/* ---------------------------------------------------------------- time */

/* UTC by arithmetic alone (the days to a civil date, as Howard Hinnant's days_from_civil inverts them) */
struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    long long days = (long long)*t / 86400, seconds = (long long)*t % 86400;
    if (seconds < 0)
        seconds += 86400, --days;
    memset(out, 0, sizeof(*out));
    out->tm_hour = (int)(seconds / 3600);
    out->tm_min = (int)(seconds / 60 % 60);
    out->tm_sec = (int)(seconds % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */
    const long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const int day = (int)(doy - (153 * mp + 2) / 5 + 1), month = (int)(mp < 10 ? mp + 3 : mp - 9);
    const long long year = yoe + era * 400 + (month <= 2);
    static const int BEFORE[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    out->tm_year = (int)(year - 1900);
    out->tm_mon = month - 1;
    out->tm_mday = day;
    out->tm_yday = BEFORE[month - 1] + day - 1 + (leap && month > 2);
    return out;
}

/* localtime needs the console's time zone: its one shared struct is copied out under a lock (the app's own calls
 * are the frontend's, between frames) */
static pthread_mutex_t time_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* closefrom belongs in a child process, which a title never has: here it would close the app's own files */
void closefrom(int lowest)
{
    (void)lowest;
}

/* mkstemp is only in libScePosixForWebKit's stub (null in a title): FFmpeg's av_tempfile asks for it */
int mkstemp(char *pattern)
{
    static const char LETTERS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t n = strlen(pattern);
    if (n < 6 || strcmp(pattern + n - 6, "XXXXXX"))
    {
        errno = EINVAL;
        return -1;
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    unsigned long long seed = (unsigned long long)t.tv_nsec ^ ((unsigned long long)t.tv_sec << 20) ^ (unsigned)getpid();
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        for (size_t i = n - 6; i < n; ++i)
        {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            pattern[i] = LETTERS[(seed >> 33) % (sizeof(LETTERS) - 1)];
        }
        const int fd = open(pattern, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    errno = EEXIST;
    return -1;
}

FILE *tmpfile(void)
{
    errno = ENOSYS; /* Python's tempfile makes its files itself, in a folder it can write */
    return NULL;
}

char *tmpnam(char *buffer)
{
    (void)buffer;
    return NULL;
}

int isatty(int fd)
{
    (void)fd;
    errno = ENOTTY;
    return 0;
}

char *ttyname(int fd)
{
    (void)fd;
    errno = ENOTTY; /* there are no terminals */
    return NULL;
}

/* ---------------------------------------------------------------- processes and users (there are none to change) */

/* A title starts no processes: Python's subprocess, os.system and Ren'Py's editor fail with ENOSYS here, rather than
 * reach the console's fork. */
pid_t fork(void)
{
    return unsupported();
}

int execv(const char *path, char *const argv[])
{
    (void)path, (void)argv;
    return unsupported();
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    (void)path, (void)argv, (void)envp;
    return unsupported();
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
    (void)fd, (void)argv, (void)envp;
    return unsupported();
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attributes, char *const argv[], char *const envp[])
{
    (void)pid, (void)path, (void)actions, (void)attributes, (void)argv, (void)envp;
    return ENOSYS;
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes, char *const argv[], char *const envp[])
{
    (void)pid, (void)file, (void)actions, (void)attributes, (void)argv, (void)envp;
    return ENOSYS;
}

int system(const char *command)
{
    if (!command)
        return 0; /* no shell */
    return unsupported();
}

FILE *popen(const char *command, const char *mode)
{
    (void)command, (void)mode;
    errno = ENOSYS;
    return NULL;
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

struct passwd *getpwnam(const char *name)
{
    (void)name;
    return NULL; /* no such user */
}

struct passwd *getpwuid(uid_t uid)
{
    (void)uid;
    return NULL;
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

/* ---------------------------------------------------------------- sockets */

/* OpenSSL's datagram BIO batches with these, which the console's libc has not */
ssize_t recvmmsg(int fd, struct mmsghdr *messages, size_t count, int flags, const struct timespec *timeout)
{
    (void)fd, (void)messages, (void)count, (void)flags, (void)timeout;
    return unsupported();
}

ssize_t sendmmsg(int fd, struct mmsghdr *messages, size_t count, int flags)
{
    (void)fd, (void)messages, (void)count, (void)flags;
    return unsupported();
}

/* ---------------------------------------------------------------- the pads: the app's, never the core's */

/* The core's controller is the frontend's (input.c's virtual one). SDL's PS5 joystick driver, inside the core, would
 * read the app's pads besides, light and shake them, and close the app's handles as SDL quits (it takes one the app
 * holds through scePadGetHandle): here it finds none. */
#define NO_PAD ((int)0x80920001) /* an error, as the console's calls return them */

int scePadInit(void)
{
    return 0;
}

int scePadOpen(int user, int type, int index, const void *parameters)
{
    (void)user, (void)type, (void)index, (void)parameters;
    return NO_PAD;
}

int scePadGetHandle(int user, int type, int index)
{
    (void)user, (void)type, (void)index;
    return NO_PAD;
}

int scePadClose(int handle)
{
    (void)handle;
    return 0;
}

int scePadReadState(int handle, void *state)
{
    (void)handle, (void)state;
    return NO_PAD;
}

int scePadSetLightBar(int handle, const void *colour)
{
    (void)handle, (void)colour;
    return NO_PAD;
}

int scePadSetVibration(int handle, const void *vibration)
{
    (void)handle, (void)vibration;
    return NO_PAD;
}

int scePadSetVibrationMode(int handle, int mode)
{
    (void)handle, (void)mode;
    return NO_PAD;
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
