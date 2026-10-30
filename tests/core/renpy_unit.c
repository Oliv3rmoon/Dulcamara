/*
 * Dulcamara - the checks of Ren'Py's core that need neither Ren'Py nor Python: the handoff between the frontend and
 * Ren'Py's thread and its clock (handoff.c), the pictures (frames.c), the sound (audio.c) and, when this computer
 * has OpenGL without a screen, the read-back (gl.c). tests/test_renpy_core.py runs it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../src/core/renpy/audio.h"
#include "../../src/core/renpy/frames.h"
#include "../../src/core/renpy/handoff.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
    do                                                                                                             \
    {                                                                                                              \
        ++checks;                                                                                                  \
        if (!(cond))                                                                                               \
        {                                                                                                          \
            ++failures;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static int64_t real_usec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* ---------------------------------------------------------------- the handoff */

static int64_t clock_seen[8];
static int resumed;

static void *five_frames(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    for (int i = 0; i < 5 && handoff_yield(); ++i)
        clock_seen[resumed++] = handoff_now_usec();
    handoff_engine_end();
    return NULL;
}

/* Each frame lets Ren'Py run once, and Ren'Py sees the clock move by exactly one frame each time. */
static void check_lockstep(void)
{
    handoff_reset();
    resumed = 0;
    pthread_t thread;
    pthread_create(&thread, NULL, five_frames, NULL);
    for (int i = 0; i < 6; ++i)
        CHECK(handoff_frame(16667, 2000000), "frame %d: Ren'Py's thread gave control back", i);
    pthread_join(thread, NULL);
    CHECK(resumed == 5, "Ren'Py's thread ran five frames, not %d", resumed);
    CHECK(clock_seen[0] >= 16667, "the clock had moved when Ren'Py first ran (%lld)", (long long)clock_seen[0]);
    for (int i = 1; i < resumed; ++i)
        CHECK(clock_seen[i] - clock_seen[i - 1] == 16667, "one frame between runs %d and %d (%lld us)", i - 1, i,
              (long long)(clock_seen[i] - clock_seen[i - 1]));
    CHECK(!handoff_on_engine_thread(), "the frontend's thread is not Ren'Py's");
}

static void *busy_then_yield(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    usleep(300000); /* Ren'Py starting: a long time without giving control back */
    while (handoff_yield())
    {
    }
    handoff_engine_end();
    return NULL;
}

/* A frame does not wait for Ren'Py longer than it may: the frontend stays responsive while Ren'Py starts. */
static void check_a_busy_ren_py(void)
{
    handoff_reset();
    pthread_t thread;
    pthread_create(&thread, NULL, busy_then_yield, NULL);
    const int64_t start = real_usec();
    const bool back = handoff_frame(16667, 20000);
    const int64_t took = real_usec() - start;
    CHECK(!back, "a frame while Ren'Py is busy does not get control back");
    CHECK(took < 150000, "a frame while Ren'Py is busy returns in its time (%lld us)", (long long)took);
    CHECK(handoff_now_usec() == 16667, "the clock moved by one frame (%lld)", (long long)handoff_now_usec());
    bool later = false;
    for (int i = 0; i < 100 && !later; ++i)
        later = handoff_frame(16667, 20000);
    CHECK(later, "once Ren'Py gives control back, the next frame gets it");
    handoff_stop();
    pthread_join(thread, NULL);
}

static bool yielded_after_stop = true;

static void *wait_until_stopped(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    while (handoff_yield())
    {
    }
    yielded_after_stop = handoff_yield();
    handoff_engine_end();
    return NULL;
}

/* Closing the game while Ren'Py waits: the wait ends, every later one returns at once, and the end is seen. */
static void check_stop(void)
{
    handoff_reset();
    pthread_t thread;
    pthread_create(&thread, NULL, wait_until_stopped, NULL);
    handoff_frame(16667, 200000);
    handoff_stop();
    CHECK(handoff_stopping(), "stopping");
    pthread_join(thread, NULL);
    CHECK(!yielded_after_stop, "a wait after the stop returns false at once");
    CHECK(handoff_engine_ended(), "Ren'Py's thread ended");
    const int64_t start = real_usec();
    CHECK(handoff_frame(16667, 2000000), "a frame after the end gets control back");
    CHECK(real_usec() - start < 100000, "and does not wait");
}

/* ---------------------------------------------------------------- the pictures */

static void check_frames(void)
{
    frames_reset();
    int w = 0, h = 0;
    bool fresh = true;
    CHECK(frames_take(&w, &h, &fresh) == NULL && !fresh, "no picture before the first");

    uint32_t *back = frames_back(4, 2);
    for (int i = 0; i < 8; ++i)
        back[i] = (uint32_t)i;
    frames_publish();
    const uint32_t *shown = frames_take(&w, &h, &fresh);
    CHECK(shown && w == 4 && h == 2 && fresh && shown[7] == 7, "the first picture, 4x2 and fresh");
    shown = frames_take(&w, &h, &fresh);
    CHECK(shown && !fresh, "the same picture again is not fresh");

    /* the frontend's picture stays as it was while Ren'Py draws the next two */
    for (int n = 1; n <= 2; ++n)
    {
        back = frames_back(4, 2);
        for (int i = 0; i < 8; ++i)
            back[i] = (uint32_t)(n * 100 + i);
        frames_publish();
    }
    CHECK(shown[0] == 0, "the frontend's picture is untouched while Ren'Py draws (%u)", shown[0]);
    shown = frames_take(&w, &h, &fresh);
    CHECK(shown && fresh && shown[0] == 200, "the newest picture comes next");

    const uint32_t gl_rows[6] = {1, 2, 3, 4, 5, 6}; /* bottom row first */
    uint32_t rows[6];
    frames_copy_flipped(rows, gl_rows, 3, 2);
    CHECK(rows[0] == 4 && rows[2] == 6 && rows[3] == 1, "rows turned over");

    int fw, fh;
    frames_fit(1280, 720, 1920, 1080, &fw, &fh);
    CHECK(fw == 1280 && fh == 720, "1280x720 fits as it is");
    frames_fit(1920, 1080, 1920, 1080, &fw, &fh);
    CHECK(fw == 1920 && fh == 1080, "1920x1080 fits as it is");
    frames_fit(2560, 1440, 1920, 1080, &fw, &fh);
    CHECK(fw == 1920 && fh == 1080, "2560x1440 becomes 1920x1080 (%dx%d)", fw, fh);
    frames_fit(1080, 1920, 1920, 1080, &fw, &fh);
    CHECK(fw == 608 && fh == 1080, "a portrait 1080x1920 becomes 608x1080 (%dx%d)", fw, fh);
    frames_fit(3000, 1000, 1920, 1080, &fw, &fh);
    CHECK(fw == 1920 && fh == 640, "3000x1000 becomes 1920x640 (%dx%d)", fw, fh);
    frames_reset();
}

/* ---------------------------------------------------------------- the sound */

static float mixed;

static void fake_mixer(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    float *samples = (float *)(void *)stream;
    for (int i = 0; i < len / (int)sizeof(float); ++i)
        samples[i] += mixed; /* Ren'Py's mixer adds into a stream SDL has filled with silence */
}

static void check_audio(void)
{
    int freq = 44100;
    uint8_t channels = 1;
    CHECK(audio_open(&freq, &channels, fake_mixer, NULL) == 0, "opened");
    CHECK(freq == DR_AUDIO_RATE && channels == 2, "Ren'Py is told 48000 Hz stereo (%d Hz, %u channels)", freq,
          channels);
    static int16_t out[1600 * 2];
    mixed = 0.5f;
    audio_render(out, 1600);
    CHECK(out[0] == 0 && out[3199] == 0, "silent until Ren'Py starts the sound");
    audio_pause(false);
    audio_render(out, 1600);
    CHECK(out[0] == 16384 && out[3199] == 16384, "half volume is 16384 (%d, %d)", out[0], out[3199]);
    mixed = 2.0f;
    audio_render(out, 4);
    CHECK(out[0] == 32767, "too loud is clipped (%d)", out[0]);
    mixed = -2.0f;
    audio_render(out, 4);
    CHECK(out[0] == -32768, "too loud the other way is clipped (%d)", out[0]);
    audio_lock();
    audio_lock(); /* SDL_LockAudio may be taken twice */
    audio_unlock();
    audio_unlock();
    audio_close();
    audio_render(out, 4);
    CHECK(out[0] == 0, "silent once closed");
    const float odd[3] = {0.0f, NAN, -1.0f};
    int16_t converted[3];
    audio_float_to_s16(odd, converted, 3);
    CHECK(converted[1] == 0 && converted[2] == -32768, "NaN is silence, -1 is -32768");
}

int main(void)
{
    check_lockstep();
    check_a_busy_ren_py();
    check_stop();
    check_frames();
    check_audio();
    printf("%d checks, %d failed\n", checks, failures);
    printf(failures ? "FAIL\n" : "PASS\n");
    return failures ? 1 : 0;
}
