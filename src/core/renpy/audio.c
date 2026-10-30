/*
 * Dulcamara - Ren'Py's core: the sound (audio.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock;
static pthread_once_t lock_made = PTHREAD_ONCE_INIT;
static dr_mix_fn mixer;
static void *mixer_data;
static bool paused = true;

static void make_lock(void)
{
    pthread_mutexattr_t recursive;
    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &recursive);
    pthread_mutexattr_destroy(&recursive);
}

void audio_lock(void)
{
    pthread_once(&lock_made, make_lock);
    pthread_mutex_lock(&lock);
}

void audio_unlock(void)
{
    pthread_mutex_unlock(&lock);
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
