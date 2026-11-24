/*
 * Dulcamara - a test library that makes starting a thread slow, as it may be on a console busy elsewhere, loaded into
 * the headless Ren'Py frontend (DYLD_INSERT_LIBRARIES on macOS, LD_PRELOAD on Linux):
 *
 *   DULCAMARA_SLOW_THREAD_START=<ms>  each pthread_create waits that long, in the thread that calls it, before the
 *                                     thread is made (on the PS5, ps5_threads.c's wrapper writes a log line at once for
 *                                     each of the first 200 and the console then makes it). Ren'Py starts a decode
 *                                     thread for every sound it plays (ffmedia.c's media_start).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#if !defined(__APPLE__)
#include <dlfcn.h>
#endif

static void slow(void)
{
    const char *ms = getenv("DULCAMARA_SLOW_THREAD_START");
    if (ms && atoi(ms) > 0)
        usleep((useconds_t)atoi(ms) * 1000);
}

#if defined(__APPLE__)
static int slow_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    slow();
    return pthread_create(thread, attr, start, arg);
}

__attribute__((used)) static struct
{
    const void *replacement, *original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {{(const void *)slow_create,
                                                                   (const void *)pthread_create}};
#else
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real)
        *(void **)&real = dlsym(RTLD_NEXT, "pthread_create");
    slow();
    return real(thread, attr, start, arg);
}
#endif
