/*
 * Dulcamara - the little of SDL2 that src/update.c and src/insider.c use (atomics, detached threads, ticks), on C11
 * atomics and POSIX threads, so that tests/test_insider.py runs them on the build machine without SDL2 itself (whose
 * library can need a screen to load).
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_HOST_SDL_H
#define DULCAMARA_HOST_SDL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef uint32_t Uint32;

typedef struct
{
    atomic_int value;
} SDL_atomic_t;

static inline int SDL_AtomicSet(SDL_atomic_t *a, int v)
{
    return atomic_exchange(&a->value, v);
}

static inline int SDL_AtomicGet(SDL_atomic_t *a)
{
    return atomic_load(&a->value);
}

static inline bool SDL_AtomicCAS(SDL_atomic_t *a, int old_value, int new_value)
{
    return atomic_compare_exchange_strong(&a->value, &old_value, new_value);
}

typedef struct SDL_Thread SDL_Thread; /* a pthread_t, allocated */

typedef struct
{
    int (*function)(void *);
    void *data;
} HostSdlStart;

static inline void *host_sdl_start(void *p)
{
    const HostSdlStart start = *(HostSdlStart *)p;
    free(p);
    start.function(start.data);
    return NULL;
}

static inline SDL_Thread *SDL_CreateThread(int (*function)(void *), const char *name, void *data)
{
    (void)name;
    HostSdlStart *start = malloc(sizeof(*start));
    pthread_t *thread = malloc(sizeof(*thread));
    if (!start || !thread)
    {
        free(start);
        free(thread);
        return NULL;
    }
    start->function = function;
    start->data = data;
    if (pthread_create(thread, NULL, host_sdl_start, start) != 0)
    {
        free(start);
        free(thread);
        return NULL;
    }
    return (SDL_Thread *)thread;
}

static inline void SDL_DetachThread(SDL_Thread *thread)
{
    pthread_detach(*(pthread_t *)thread);
    free(thread);
}

static inline Uint32 SDL_GetTicks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (Uint32)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

#endif
