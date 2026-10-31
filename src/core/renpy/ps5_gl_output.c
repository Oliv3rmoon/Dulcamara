/*
 * Dulcamara - Ren'Py's cores on the PS5: ps5-opengl's TV output, the core's stand-in. ps5-opengl's native runtime
 * opens the console's video output (sceVideoOutOpen) and registers its own scanout buffers on it before its first
 * draw, even one into an offscreen framebuffer, and drops every draw until that succeeds. The app already holds the
 * output (its Vulkan screen), and the core only reads Ren'Py's pictures back (gl.c): it never swaps a window surface,
 * the only way ps5-opengl shows anything on the TV. So each core's relocatable link sends ps5-opengl's VideoOut calls
 * here (--wrap, tools/renpy/ps5.sh and tools/renpy7/ps5.sh): a port that is never the TV's, buffers that are noted and
 * never shown, and flips that are done as soon as they are asked for. sceVideoOutSetBufferAttribute2 only fills in a
 * struct and stays the console's.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include "log.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define STAND_IN 0x5D17 /* the stand-in port's handle: positive, as an open port's is */

static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static bool opened;
static uint64_t flips;
static int64_t last_flip_arg;
static int32_t last_buffer = -1;

int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *param)
{
    (void)user, (void)bus, (void)index, (void)param;
    pthread_mutex_lock(&output_lock);
    const bool first = !opened;
    opened = true;
    pthread_mutex_unlock(&output_lock);
    if (first)
        log_queue(1, "renpy: OpenGL asked for the TV output and has the core's stand-in (the app keeps the screen)");
    return STAND_IN;
}

int __wrap_sceVideoOutClose(int32_t handle)
{
    (void)handle;
    return 0;
}

int __wrap_sceVideoOutSetFlipRate(int32_t handle, int32_t rate)
{
    (void)handle, (void)rate;
    return 0;
}

int __wrap_sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index_start, void *buffers,
                                       int32_t count, void *attribute, int32_t category, void *option)
{
    (void)handle, (void)set_index, (void)buffer_index_start, (void)buffers, (void)count, (void)attribute,
        (void)category, (void)option;
    return 0;
}

int __wrap_sceVideoOutUnregisterBuffers(int32_t handle, int32_t set_index)
{
    (void)handle, (void)set_index;
    return 0;
}

int __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t flip_mode, int64_t flip_arg)
{
    (void)handle, (void)flip_mode;
    pthread_mutex_lock(&output_lock);
    ++flips;
    last_flip_arg = flip_arg;
    last_buffer = buffer_index;
    pthread_mutex_unlock(&output_lock);
    return 0;
}

int __wrap_sceVideoOutIsFlipPending(int32_t handle)
{
    (void)handle;
    return 0;
}

int __wrap_sceVideoOutWaitVblank(int32_t handle)
{
    (void)handle;
    return 0;
}

/* SceVideoOutFlipStatus: count, process time, tsc, the last flip's argument, submit tsc, a reserved word, then the
 * GPU queue's and pending flips' counts and the buffer shown (ps5-opengl reads the argument, its fourth word) */
int __wrap_sceVideoOutGetFlipStatus(int32_t handle, void *status)
{
    (void)handle;
    uint64_t words[8] = {0};
    pthread_mutex_lock(&output_lock);
    words[0] = flips;
    words[3] = (uint64_t)last_flip_arg;
    const int32_t shown = last_buffer;
    pthread_mutex_unlock(&output_lock);
    int32_t counts[4] = {0, 0, shown, 0};
    memcpy(&words[6], counts, sizeof(counts));
    memcpy(status, words, sizeof(words));
    return 0;
}

int __wrap_sceVideoOutAddFlipEvent(void *queue, int32_t handle, void *data)
{
    (void)queue, (void)handle, (void)data;
    return 0;
}

int __wrap_sceVideoOutDeleteFlipEvent(void *queue, int32_t handle)
{
    (void)queue, (void)handle;
    return 0;
}

/* the TV's modes stay the app's: no high refresh to offer, nothing to configure, no status to give */
int __wrap_sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void *a, const void *b, const void *c)
{
    (void)handle, (void)mode, (void)a, (void)b, (void)c;
    return 0;
}

int __wrap_sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void *a, const void *b, const void *c)
{
    (void)handle, (void)mode, (void)a, (void)b, (void)c;
    return 0;
}

int __wrap_sceVideoOutGetOutputStatus(int32_t handle, void *status)
{
    (void)handle, (void)status;
    return 0;
}

int __wrap_sceVideoOutGetResolutionStatus(int32_t handle, void *status)
{
    (void)handle, (void)status;
    return 0;
}

#endif
