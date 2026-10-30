/*
 * Dulcamara - the checks of Ren'Py's core that need neither Ren'Py nor Python: the handoff between the frontend and
 * Ren'Py's thread and its clock (handoff.c), the pictures (frames.c), the sound (audio.c) and, when this computer
 * has OpenGL without a screen, the read-back (gl.c). tests/test_renpy_core.py runs it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../src/core/renpy/handoff.h"

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

int main(void)
{
    check_lockstep();
    check_a_busy_ren_py();
    check_stop();
    printf("%d checks, %d failed\n", checks, failures);
    printf(failures ? "FAIL\n" : "PASS\n");
    return failures ? 1 : 0;
}
