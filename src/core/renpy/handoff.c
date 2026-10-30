/*
 * Dulcamara - Ren'Py's core: the handoff (handoff.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two counters keep the threads apart: `frames` (the frontend's frames) and `yields` (the times Ren'Py gave control
 * back). A frame waits for `yields` to move; Ren'Py, giving control back, waits for `frames` to move. Ren'Py that
 * was busy through several frames carries on at once when it gives control back, at the clock's latest time.
 */
#include "handoff.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t to_engine = PTHREAD_COND_INITIALIZER;
static pthread_cond_t to_frontend = PTHREAD_COND_INITIALIZER;

/* The waits time out by the monotonic clock, which a change of the system's time does not move; macOS has no
 * pthread_condattr_setclock, so there they keep the real-time clock pthread_cond_timedwait uses by default. */
#if defined(__APPLE__)
#define WAIT_CLOCK CLOCK_REALTIME
static void use_wait_clock(void)
{
}
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
static void use_wait_clock(void)
{
    pthread_condattr_t monotonic;
    pthread_condattr_init(&monotonic);
    pthread_condattr_setclock(&monotonic, CLOCK_MONOTONIC);
    pthread_cond_init(&to_engine, &monotonic);
    pthread_cond_init(&to_frontend, &monotonic);
    pthread_condattr_destroy(&monotonic);
}
#endif
static pthread_once_t wait_clock_once = PTHREAD_ONCE_INIT;
static pthread_t engine;
static bool engine_known, stopping, ended;
static int64_t now_usec;
static uint64_t frames, engine_frame, yields;

void handoff_reset(void)
{
    pthread_once(&wait_clock_once, use_wait_clock); /* before either thread waits: every user resets first */
    pthread_mutex_lock(&lock);
    engine_known = stopping = ended = false;
    now_usec = 0;
    frames = engine_frame = yields = 0;
    pthread_mutex_unlock(&lock);
}

/* wait_usec from now, on the clock pthread_cond_timedwait uses */
static struct timespec deadline(int64_t wait_usec)
{
    struct timespec t;
    clock_gettime(WAIT_CLOCK, &t);
    const int64_t ns = (int64_t)t.tv_nsec + (wait_usec % 1000000) * 1000;
    t.tv_sec += (time_t)(wait_usec / 1000000 + ns / 1000000000);
    t.tv_nsec = (long)(ns % 1000000000);
    return t;
}

bool handoff_frame(int64_t frame_usec, int64_t wait_usec)
{
    pthread_mutex_lock(&lock);
    now_usec += frame_usec;
    ++frames;
    const uint64_t before = yields;
    pthread_cond_broadcast(&to_engine);
    const struct timespec until = deadline(wait_usec);
    while (yields == before && !ended)
        if (pthread_cond_timedwait(&to_frontend, &lock, &until) == ETIMEDOUT)
            break;
    const bool back = yields != before || ended;
    pthread_mutex_unlock(&lock);
    return back;
}

void handoff_stop(void)
{
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_broadcast(&to_engine);
    pthread_cond_broadcast(&to_frontend);
    pthread_mutex_unlock(&lock);
}

void handoff_engine_begin(void)
{
    pthread_mutex_lock(&lock);
    engine = pthread_self();
    engine_known = true;
    engine_frame = frames;
    pthread_mutex_unlock(&lock);
}

bool handoff_yield(void)
{
    pthread_mutex_lock(&lock);
    ++yields;
    pthread_cond_broadcast(&to_frontend);
    while (!stopping && frames == engine_frame)
        pthread_cond_wait(&to_engine, &lock);
    engine_frame = frames;
    const bool go_on = !stopping;
    pthread_mutex_unlock(&lock);
    return go_on;
}

void handoff_engine_end(void)
{
    pthread_mutex_lock(&lock);
    ended = true;
    pthread_cond_broadcast(&to_frontend);
    pthread_mutex_unlock(&lock);
}

bool handoff_on_engine_thread(void)
{
    pthread_mutex_lock(&lock);
    const bool is = engine_known && !ended && pthread_equal(engine, pthread_self());
    pthread_mutex_unlock(&lock);
    return is;
}

bool handoff_stopping(void)
{
    pthread_mutex_lock(&lock);
    const bool is = stopping;
    pthread_mutex_unlock(&lock);
    return is;
}

bool handoff_engine_ended(void)
{
    pthread_mutex_lock(&lock);
    const bool is = ended;
    pthread_mutex_unlock(&lock);
    return is;
}

int64_t handoff_now_usec(void)
{
    pthread_mutex_lock(&lock);
    const int64_t now = now_usec;
    pthread_mutex_unlock(&lock);
    return now;
}
