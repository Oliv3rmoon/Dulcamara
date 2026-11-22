/*
 * Dulcamara - a test library that makes Ren'Py's decode threads (ffmedia.c's, named "decode: <file>") slow to close
 * their sound, as a console busy elsewhere may make them, loaded into the headless Ren'Py frontend
 * (DYLD_INSERT_LIBRARIES on macOS, LD_PRELOAD on Linux):
 *
 *   DULCAMARA_SLOW_MEDIA_CLOSE=<ms>  each decode thread waits that long as it ends (in deallocate, as it destroys its
 *                                    first lock, before it closes its file through Python); once a thread only, as
 *                                    Python destroys locks of its own in that thread later, with the GIL held
 *
 * As the process ends it waits twice as long, so that a close let run after Python has ended happens while the process
 * still runs, as it does on the console, where Dulcamara goes on after a game (Python's end and the process's are
 * otherwise milliseconds apart, and the close would be lost with the process).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(__APPLE__)
#include <dlfcn.h>
#endif

static int slow_ms(void)
{
    const char *ms = getenv("DULCAMARA_SLOW_MEDIA_CLOSE");
    return ms ? atoi(ms) : 0;
}

static void slow_if_decoding(void)
{
    static __thread int slowed;
    char name[64] = "";
    if (slowed || slow_ms() <= 0 || pthread_getname_np(pthread_self(), name, sizeof(name)) != 0 ||
        strncmp(name, "decode", 6) != 0)
        return;
    slowed = 1;
    usleep((useconds_t)slow_ms() * 1000);
}

__attribute__((destructor)) static void linger(void)
{
    if (slow_ms() > 0)
        usleep((useconds_t)slow_ms() * 2000);
}

#if defined(__APPLE__)
static int slow_destroy(pthread_mutex_t *mutex)
{
    slow_if_decoding();
    return pthread_mutex_destroy(mutex);
}

__attribute__((used)) static struct
{
    const void *replacement, *original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {{(const void *)slow_destroy,
                                                                   (const void *)pthread_mutex_destroy}};
#else
int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    static int (*real)(pthread_mutex_t *);
    if (!real)
        *(void **)&real = dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    slow_if_decoding();
    return real(mutex);
}
#endif
