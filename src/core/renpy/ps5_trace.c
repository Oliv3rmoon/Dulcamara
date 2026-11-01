/*
 * Dulcamara - Ren'Py's cores on the PS5: a trace of what ends the app without a crash report. 0.2.6 and 0.2.7 ended
 * on a game's first hover, as Ren'Py started its first OGG (its hover sound) beside an MP3 and a WebM, with no signal
 * the crash handler saw, even with a signal stack on every thread; the build machine plays the same. So each core's
 * relocatable link (--wrap, tools/renpy/ps5.sh and tools/renpy7/ps5.sh) sends here:
 *  - abort, exit, _exit and _Exit: the call is logged, with where it came from, before it ends the app;
 *  - FFmpeg's steps for each stream Ren'Py opens: opening it, reading its streams, opening its codec, and the first
 *    packet, frame and conversion of each, logged before and after, so the log ends at the step that never returned;
 *  - the threads FFmpeg asks for a codec, fewer of them (each codec's opening goes through here already).
 * Each line is written at once (log_now). The FFmpeg types are left opaque: an AVCodec's first field is its name.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include "log.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>

#define TRACE_LINES 600

static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static int traced;

#define TRACE(...)                                                                                                 \
    do                                                                                                             \
    {                                                                                                              \
        pthread_mutex_lock(&trace_lock);                                                                           \
        const bool trace_on = traced < TRACE_LINES;                                                                \
        traced += trace_on;                                                                                        \
        pthread_mutex_unlock(&trace_lock);                                                                         \
        if (trace_on)                                                                                              \
            log_now(1, __VA_ARGS__);                                                                               \
    } while (0)

/* ---------------------------------------------------------------- what ends the app */

noreturn void __real_abort(void);
noreturn void __real_exit(int status);
noreturn void __real__exit(int status);
noreturn void __real__Exit(int status);

noreturn void __wrap_abort(void)
{
    log_now(2, "renpy: trace: abort() called from %p", __builtin_return_address(0));
    __real_abort();
}

noreturn void __wrap_exit(int status)
{
    log_now(2, "renpy: trace: exit(%d) called from %p", status, __builtin_return_address(0));
    __real_exit(status);
}

noreturn void __wrap__exit(int status)
{
    log_now(2, "renpy: trace: _exit(%d) called from %p", status, __builtin_return_address(0));
    __real__exit(status);
}

noreturn void __wrap__Exit(int status)
{
    log_now(2, "renpy: trace: _Exit(%d) called from %p", status, __builtin_return_address(0));
    __real__Exit(status);
}

/* ---------------------------------------------------------------- FFmpeg's steps */

/* the contexts whose first packet, frame, read or conversion was traced: each is traced once until it is freed */
#define SEEN 128
static const void *seen[SEEN][4];

enum
{
    FIRST_PACKET,
    FIRST_FRAME,
    FIRST_READ,
    FIRST_CONVERSION,
};

static bool first(const void *context, int what)
{
    bool is_first = true;
    int free_slot = -1;
    pthread_mutex_lock(&trace_lock);
    for (int i = 0; i < SEEN && is_first; ++i)
    {
        if (seen[i][what] == context)
            is_first = false;
        else if (!seen[i][what] && free_slot < 0)
            free_slot = i;
    }
    if (is_first && free_slot >= 0)
        seen[free_slot][what] = context;
    pthread_mutex_unlock(&trace_lock);
    return is_first;
}

static void forget(const void *context)
{
    pthread_mutex_lock(&trace_lock);
    for (int i = 0; i < SEEN; ++i)
        for (int w = 0; w < 4; ++w)
            if (seen[i][w] == context)
                seen[i][w] = NULL;
    pthread_mutex_unlock(&trace_lock);
}

static const char *codec_name(const void *codec)
{
    return codec ? *(const char *const *)codec : "?";
}

int __real_avformat_open_input(void **context, const char *url, const void *format, void **options);
int __real_avformat_find_stream_info(void *context, void **options);
int __real_avcodec_open2(void *codec_context, const void *codec, void **options);
int __real_avcodec_send_packet(void *codec_context, const void *packet);
int __real_avcodec_receive_frame(void *codec_context, void *frame);
int __real_av_read_frame(void *context, void *packet);
int __real_swr_convert_frame(void *resampler, void *out, const void *in);
void __real_avcodec_free_context(void **codec_context);
void __real_avformat_close_input(void **context);
void __real_swr_free(void **resampler);

int __wrap_avformat_open_input(void **context, const char *url, const void *format, void **options)
{
    TRACE("renpy: trace: opening %s", url ? url : "a stream of Ren'Py's");
    const int rc = __real_avformat_open_input(context, url, format, options);
    TRACE("renpy: trace: opened (%d)", rc);
    return rc;
}

int __wrap_avformat_find_stream_info(void *context, void **options)
{
    TRACE("renpy: trace: reading the streams of %p", context);
    const int rc = __real_avformat_find_stream_info(context, options);
    TRACE("renpy: trace: read the streams of %p (%d)", context, rc);
    return rc;
}

/* Ren'Py opens each codec with "threads" "auto": a thread for each of the console's cores, 16 for each movie, each
 * with a core thread's 2 MB of stack (ps5_threads.c). 0.2.8's log had the console refuse them (EAGAIN) for a menu's
 * movie opened a second time while the first still decoded: its picture's codec never opened. A codec gets
 * DULCAMARA_CODEC_THREADS threads (4 unless that is set) at most. */
typedef struct
{
    char *key, *value;
} Entry;
Entry *av_dict_get(const void *dictionary, const char *key, const Entry *previous, int flags);
int av_dict_set(void **dictionary, const char *key, const char *value, int flags);

static void fewer_threads(void **options)
{
    const char *setting = getenv("DULCAMARA_CODEC_THREADS");
    const int most = setting && atoi(setting) > 0 ? atoi(setting) : 4;
    const Entry *threads = options && *options ? av_dict_get(*options, "threads", NULL, 0) : NULL;
    if (!threads || (atoi(threads->value) > 0 && atoi(threads->value) <= most))
        return;
    static bool noted;
    if (!noted)
    {
        noted = true;
        log_queue(1, "renpy: FFmpeg asked for %s threads for a codec; it gets %d", threads->value, most);
    }
    char value[12];
    snprintf(value, sizeof value, "%d", most);
    av_dict_set(options, "threads", value, 0);
}

int __wrap_avcodec_open2(void *codec_context, const void *codec, void **options)
{
    fewer_threads(options);
    TRACE("renpy: trace: opening the %s codec (%p)", codec_name(codec), codec_context);
    const int rc = __real_avcodec_open2(codec_context, codec, options);
    TRACE("renpy: trace: opened the %s codec (%d)", codec_name(codec), rc);
    return rc;
}

int __wrap_avcodec_send_packet(void *codec_context, const void *packet)
{
    const bool trace = first(codec_context, FIRST_PACKET);
    if (trace)
        TRACE("renpy: trace: the first packet into %p", codec_context);
    const int rc = __real_avcodec_send_packet(codec_context, packet);
    if (trace)
        TRACE("renpy: trace: the first packet into %p taken (%d)", codec_context, rc);
    return rc;
}

int __wrap_avcodec_receive_frame(void *codec_context, void *frame)
{
    const bool trace = first(codec_context, FIRST_FRAME);
    if (trace)
        TRACE("renpy: trace: the first frame from %p", codec_context);
    const int rc = __real_avcodec_receive_frame(codec_context, frame);
    if (trace)
        TRACE("renpy: trace: the first frame from %p given (%d)", codec_context, rc);
    return rc;
}

int __wrap_av_read_frame(void *context, void *packet)
{
    const bool trace = first(context, FIRST_READ);
    if (trace)
        TRACE("renpy: trace: the first read of %p", context);
    const int rc = __real_av_read_frame(context, packet);
    if (trace)
        TRACE("renpy: trace: the first read of %p done (%d)", context, rc);
    return rc;
}

int __wrap_swr_convert_frame(void *resampler, void *out, const void *in)
{
    const bool trace = first(resampler, FIRST_CONVERSION);
    if (trace)
        TRACE("renpy: trace: the first conversion by %p", resampler);
    const int rc = __real_swr_convert_frame(resampler, out, in);
    if (trace)
        TRACE("renpy: trace: the first conversion by %p done (%d)", resampler, rc);
    return rc;
}

void __wrap_avcodec_free_context(void **codec_context)
{
    if (codec_context && *codec_context)
    {
        forget(*codec_context);
        TRACE("renpy: trace: freeing the codec %p", *codec_context);
    }
    __real_avcodec_free_context(codec_context);
}

void __wrap_avformat_close_input(void **context)
{
    if (context && *context)
    {
        forget(*context);
        TRACE("renpy: trace: closing %p", *context);
    }
    __real_avformat_close_input(context);
}

void __wrap_swr_free(void **resampler)
{
    if (resampler && *resampler)
        forget(*resampler);
    __real_swr_free(resampler);
}

#endif
