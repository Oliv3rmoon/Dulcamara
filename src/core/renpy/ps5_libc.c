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
#include <limits.h>
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

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

/* UTF-8 for a wide string (n bytes at most, with its 0), and the other way (n wide characters at most, with their 0):
 * the lengths written, or -1 when out runs out first or a character has no UTF-8 */
static long utf8_of(const wchar_t *text, char *out, size_t n)
{
    size_t at = 0;
    for (; *text; ++text)
    {
        const unsigned long c = (unsigned long)*text;
        const size_t length = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : c < 0x110000 ? 4 : 0;
        if (!length || at + length >= n)
            return -1;
        if (length == 1)
            out[at++] = (char)c;
        else
        {
            static const unsigned char LEAD[5] = {0, 0, 0xC0, 0xE0, 0xF0};
            out[at++] = (char)(LEAD[length] | (c >> (6 * (length - 1))));
            for (size_t i = length - 1; i-- > 0;)
                out[at++] = (char)(0x80 | ((c >> (6 * i)) & 0x3F));
        }
    }
    if (at >= n)
        return -1;
    out[at] = '\0';
    return (long)at;
}

static long wide_of(const char *text, wchar_t *out, size_t n)
{
    size_t at = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++at)
    {
        if (at + 1 >= n)
            return -1;
        unsigned long c = *p++;
        const int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more)
            c &= 0x3F >> more;
        for (int i = 0; i < more && (*p & 0xC0) == 0x80; ++i)
            c = c << 6 | (*p++ & 0x3F);
        out[at] = (wchar_t)c;
    }
    if (at >= n)
        return -1;
    out[at] = L'\0';
    return (long)at;
}

/* Python's time.strftime calls wcsftime (Ren'Py's save times), which only libSceLibcInternal has and nothing on the
 * console has been seen to run; strftime runs there (EasyRPG Player's core): the wide format and text go through it */
size_t wcsftime(wchar_t *restrict text, size_t size, const wchar_t *restrict format, const struct tm *restrict when)
{
    const size_t format_size = wcslen(format) * 4 + 1, text_size = size * 4 + 1;
    char *narrow_format = malloc(format_size), *narrow_text = malloc(text_size);
    size_t length = 0;
    if (narrow_format && narrow_text && size && utf8_of(format, narrow_format, format_size) >= 0 &&
        strftime(narrow_text, text_size, narrow_format, when) > 0)
    {
        const long wide = wide_of(narrow_text, text, size);
        length = wide > 0 ? (size_t)wide : 0;
    }
    free(narrow_format);
    free(narrow_text);
    return length;
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

/* ---------------------------------------------------------------- the working folder: the core's own */

/* A title's libc has no getcwd, and libSceLibcInternal's, which a title's link takes, crashed Dulcamara 0.2.0 to 0.2.2
 * on the console as Python started: finding no "renpy" program to start from, getpath takes its folder as
 * abspath('.') (0.2.2's crash report, read at the console's context layout, has getcwd's buffer, its size and its end
 * in rdi, rsi and rbx). EasyRPG Player's core, which runs there, never calls it. The core keeps the working folder
 * itself: chdir (which each core's link wraps, tools/renpy/ps5.sh) changes the console's and notes the new one,
 * which getcwd and realpath answer from. Until a chdir it is "/". */
static pthread_mutex_t folder_lock = PTHREAD_MUTEX_INITIALIZER;
static char folder[PATH_MAX] = "/";

/* path as an absolute one (against the working folder), without ".", ".." or empty parts, in out (size bytes) */
static bool absolute_path(const char *path, char *out, size_t size)
{
    char joined[2 * PATH_MAX];
    pthread_mutex_lock(&folder_lock);
    const int n = path[0] == '/' ? snprintf(joined, sizeof(joined), "%s", path)
                                 : snprintf(joined, sizeof(joined), "%s/%s", folder, path);
    pthread_mutex_unlock(&folder_lock);
    if (n < 0 || (size_t)n >= sizeof(joined))
    {
        errno = ENAMETOOLONG;
        return false;
    }
    size_t at = 0;
    for (const char *part = joined; *part;)
    {
        if (*part == '/')
        {
            ++part;
            continue;
        }
        const size_t length = strcspn(part, "/");
        if (length == 2 && part[0] == '.' && part[1] == '.')
        {
            while (at > 0 && out[at - 1] != '/')
                --at;
            if (at > 0)
                --at; /* and its "/" */
        }
        else if (length != 1 || part[0] != '.')
        {
            if (at + 1 + length >= size)
            {
                errno = ENAMETOOLONG;
                return false;
            }
            out[at++] = '/';
            memcpy(out + at, part, length);
            at += length;
        }
        part += length;
    }
    if (at == 0)
        out[at++] = '/';
    out[at] = '\0';
    return true;
}

char *getcwd(char *buffer, size_t size)
{
    pthread_mutex_lock(&folder_lock);
    const size_t n = strlen(folder) + 1;
    char *result = NULL;
    if (!buffer)
        result = malloc(n > size ? n : size); /* as large as asked, or as needed */
    else if (size == 0)
        errno = EINVAL;
    else if (size < n)
        errno = ERANGE;
    else
        result = buffer;
    if (result)
        memcpy(result, folder, n);
    else if (!buffer)
        errno = ENOMEM;
    pthread_mutex_unlock(&folder_lock);
    return result;
}

int __real_chdir(const char *path);

int __wrap_chdir(const char *path)
{
    char target[PATH_MAX];
    if (!absolute_path(path, target, sizeof(target)) || __real_chdir(target) != 0)
        return -1;
    pthread_mutex_lock(&folder_lock);
    memcpy(folder, target, strlen(target) + 1);
    pthread_mutex_unlock(&folder_lock);
    return 0;
}

/* libSceLibcInternal's realpath would ask its getcwd for a relative path. A title has no links to follow (readlink
 * says so here), so the absolute path is the real one, when it is there. */
char *realpath(const char *path, char *resolved)
{
    if (!path || !*path)
    {
        errno = path ? ENOENT : EINVAL;
        return NULL;
    }
    char target[PATH_MAX];
    struct stat st;
    if (!absolute_path(path, target, sizeof(target)) || stat(target, &st) != 0)
        return NULL;
    if (!resolved && !(resolved = malloc(PATH_MAX)))
    {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(resolved, target, strlen(target) + 1);
    return resolved;
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

/* ---------------------------------------------------------------- libkernel_sys's */

/* Functions whose only stub is libkernel_sys's: a title's link takes them, but its import does not reach them at run
 * time (tooling/radv/radv-link.sh says so of readlink, link and symlink; the SDK this app pins has no platform
 * functions for them). Python's path calculation calls readlink as it starts (getpath's realpath; Python 2.7's
 * getpath too). Here a title has no links to read, make or follow, and no owners, groups, sessions or children to
 * change. */
ssize_t readlink(const char *path, char *buffer, size_t size)
{
    (void)path, (void)buffer, (void)size;
    errno = EINVAL; /* not a link */
    return -1;
}

int link(const char *from, const char *to)
{
    (void)from, (void)to;
    return unsupported();
}

int linkat(int from_dir, const char *from, int to_dir, const char *to, int flags)
{
    (void)from_dir, (void)from, (void)to_dir, (void)to, (void)flags;
    return unsupported();
}

int symlink(const char *target, const char *path)
{
    (void)target, (void)path;
    return unsupported();
}

int symlinkat(const char *target, int dir, const char *path)
{
    (void)target, (void)dir, (void)path;
    return unsupported();
}

int chown(const char *path, uid_t owner, gid_t group)
{
    (void)path, (void)owner, (void)group;
    return unsupported();
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd, (void)owner, (void)group;
    return unsupported();
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    (void)path, (void)owner, (void)group;
    return unsupported();
}

int fchownat(int dir, const char *path, uid_t owner, gid_t group, int flags)
{
    (void)dir, (void)path, (void)owner, (void)group, (void)flags;
    return unsupported();
}

int lchmod(const char *path, mode_t mode)
{
    (void)path, (void)mode;
    return unsupported();
}

int lchflags(const char *path, unsigned long flags)
{
    (void)path, (void)flags;
    return unsupported();
}

int chroot(const char *path)
{
    (void)path;
    return unsupported();
}

int fchdir(int fd)
{
    (void)fd;
    return unsupported();
}

long pathconf(const char *path, int name)
{
    (void)path, (void)name;
    errno = EINVAL;
    return -1;
}

long fpathconf(int fd, int name)
{
    (void)fd, (void)name;
    errno = EINVAL;
    return -1;
}

mode_t umask(mode_t mask)
{
    (void)mask;
    return 022; /* the mask there was, and is */
}

int setpgid(pid_t pid, pid_t group)
{
    (void)pid, (void)group;
    errno = EPERM;
    return -1;
}

pid_t setsid(void)
{
    errno = EPERM;
    return -1;
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage)
{
    (void)pid, (void)status, (void)options, (void)usage;
    errno = ECHILD; /* no children: fork makes none here */
    return -1;
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
