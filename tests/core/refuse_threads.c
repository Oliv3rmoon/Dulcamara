/*
 * Dulcamara - a test library that makes pthread_create refuse threads as the PS5 does when its system memory for them
 * runs out (EAGAIN), loaded into the headless Ren'Py frontend (DYLD_INSERT_LIBRARIES on macOS, LD_PRELOAD on Linux):
 *
 *   DULCAMARA_REFUSE_THREADS=only:<n>[,<m>...]  the n-th (m-th...) thread created is refused
 *   DULCAMARA_REFUSE_THREADS=after:<n>          every thread after the n-th is refused
 *   DULCAMARA_REFUSE_THREADS=file:<path>        every thread is refused while <path> exists
 *
 * Each refusal is written to standard error ("refuse-threads: refused thread <n>").
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(__APPLE__)
#include <dlfcn.h>
#endif

static int numbered; /* threads asked for so far */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int refuse(int n)
{
    const char *rule = getenv("DULCAMARA_REFUSE_THREADS");
    if (!rule)
        return 0;
    if (!strncmp(rule, "after:", 6))
        return n > atoi(rule + 6);
    if (!strncmp(rule, "file:", 5))
    {
        struct stat st;
        return stat(rule + 5, &st) == 0;
    }
    if (!strncmp(rule, "only:", 5))
        for (const char *at = rule + 5; *at; at = strchr(at, ',') ? strchr(at, ',') + 1 : at + strlen(at))
            if (atoi(at) == n)
                return 1;
    return 0;
}

static int refused(int n)
{
    char line[80];
    const int length = snprintf(line, sizeof(line), "refuse-threads: refused thread %d\n", n);
    (void)!write(2, line, (size_t)length);
    return EAGAIN;
}

#if defined(__APPLE__)
static int counted_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *arg)
{
    pthread_mutex_lock(&lock);
    const int n = ++numbered;
    pthread_mutex_unlock(&lock);
    if (refuse(n))
        return refused(n);
    return pthread_create(thread, attributes, start, arg);
}

__attribute__((used)) static struct
{
    const void *replacement, *original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {{(const void *)counted_create,
                                                                   (const void *)pthread_create}};
#else
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real)
        *(void **)&real = dlsym(RTLD_NEXT, "pthread_create");
    pthread_mutex_lock(&lock);
    const int n = ++numbered;
    pthread_mutex_unlock(&lock);
    if (refuse(n))
        return refused(n);
    return real(thread, attributes, start, arg);
}
#endif
