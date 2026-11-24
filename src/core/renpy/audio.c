/*
 * Dulcamara - Ren'Py's core: the sound (audio.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio.h"

#include "log.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

/* a hold of the lock this long (by Ren'Py's thread, starting a sound, say) is logged: the frontend's top-ups are
 * skipped all the while (audio_try_render), and its queue of sound runs down */
#define LONG_HOLD_US 20000

static pthread_mutex_t lock;
static pthread_once_t lock_made = PTHREAD_ONCE_INIT;
static dr_mix_fn mixer;
static void *mixer_data;
static bool paused = true;
/* the lock's holder: how many times it has taken it (SDL's lock may be taken again inside) and since when; changed
 * only while the lock is held */
static int depth;
static uint64_t held_since;
static struct dr_audio_stats stats; /* atomics: read without the lock (audio_stats), which a stuck holder may keep */

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void make_lock(void)
{
    pthread_mutexattr_t recursive;
    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &recursive);
    pthread_mutexattr_destroy(&recursive);
}

static void taken(void)
{
    if (depth++ == 0)
        held_since = now_us();
}

void audio_lock(void)
{
    pthread_once(&lock_made, make_lock);
    pthread_mutex_lock(&lock);
    taken();
}

void audio_unlock(void)
{
    uint64_t held = 0, long_holds = 0;
    if (--depth == 0)
    {
        held = now_us() - held_since;
        if (held > __atomic_load_n(&stats.longest_hold_us, __ATOMIC_RELAXED))
            __atomic_store_n(&stats.longest_hold_us, held, __ATOMIC_RELAXED);
        if (held >= LONG_HOLD_US)
            long_holds = __atomic_add_fetch(&stats.long_holds, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&lock);
    if (long_holds && (long_holds <= 20 || long_holds % 100 == 0))
        log_queue(1, "renpy: the sound's lock was held %.1f ms (%llu holds of %d ms or more so far): the frontend's "
                     "sound was not topped up meanwhile",
                  held / 1000.0, (unsigned long long)long_holds, LONG_HOLD_US / 1000);
}

int audio_open(int *freq, uint8_t *channels, dr_mix_fn fn, void *userdata)
{
    audio_lock();
    *freq = DR_AUDIO_RATE;
    *channels = 2;
    mixer = fn;
    mixer_data = userdata;
    paused = true;
    audio_unlock();
    return 0;
}

void audio_pause(bool on)
{
    audio_lock();
    paused = on;
    audio_unlock();
}

void audio_close(void)
{
    audio_lock();
    mixer = NULL;
    mixer_data = NULL;
    paused = true;
    audio_unlock();
}

void audio_float_to_s16(const float *in, int16_t *out, size_t samples)
{
    for (size_t i = 0; i < samples; ++i)
    {
        const float v = in[i] * 32768.0f;
        out[i] = v != v ? 0 : v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
    }
}

void audio_render(int16_t *out, size_t frames)
{
    float mixed[1024 * 2];
    while (frames)
    {
        const size_t n = frames < 1024 ? frames : 1024;
        audio_lock();
        const bool playing = mixer && !paused;
        if (playing)
        {
            memset(mixed, 0, n * 2 * sizeof(float));
            mixer(mixer_data, (uint8_t *)mixed, (int)(n * 2 * sizeof(float)));
        }
        audio_unlock();
        if (playing)
            audio_float_to_s16(mixed, out, n * 2);
        else
            memset(out, 0, n * 2 * sizeof(int16_t));
        out += n * 2;
        frames -= n;
    }
}

bool audio_try_render(int16_t *out, size_t frames)
{
    pthread_once(&lock_made, make_lock);
    if (pthread_mutex_trylock(&lock) != 0)
    {
        const uint64_t skipped = __atomic_add_fetch(&stats.skipped, 1, __ATOMIC_RELAXED);
        if (skipped <= 20 || skipped % 1000 == 0)
            log_queue(1, "renpy: the sound's lock was busy: a top-up of the frontend's sound skipped (%llu so far), "
                         "not waited for",
                      (unsigned long long)skipped);
        return false;
    }
    taken();
    audio_render(out, frames); /* the lock is taken again inside, as SDL's may be */
    audio_unlock();
    return true;
}

void audio_stats(struct dr_audio_stats *out)
{
    out->longest_hold_us = __atomic_load_n(&stats.longest_hold_us, __ATOMIC_RELAXED);
    out->long_holds = __atomic_load_n(&stats.long_holds, __ATOMIC_RELAXED);
    out->skipped = __atomic_load_n(&stats.skipped, __ATOMIC_RELAXED);
}
