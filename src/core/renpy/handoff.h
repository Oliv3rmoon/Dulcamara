/*
 * Dulcamara - Ren'Py's core: control handed between the frontend's retro_run and Ren'Py's own thread, and the clock
 * Ren'Py keeps time by. Ren'Py runs until it shows a frame or waits, then gives control back. The clock moves only
 * when the frontend runs a frame, by that frame's time, so fast-forward runs Ren'Py's transitions faster.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_HANDOFF_H
#define DR_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

/* Before Ren'Py's thread starts: no frames, the clock at zero, not stopping. */
void handoff_reset(void);

/* The frontend: one frame. The clock moves on by frame_usec and Ren'Py runs until it gives control back, for at
 * most wait_usec of real time. True when it gave control back (or has ended) in that time; false when it is still
 * busy (starting, loading), and then it carries on by itself and gives control back at a later frame. */
bool handoff_frame(int64_t frame_usec, int64_t wait_usec);
/* The frontend: from now on Ren'Py never waits again (its game is being closed). */
void handoff_stop(void);

/* Ren'Py's thread, first: marks it as the thread that hands control back. It runs at once, without a frame. */
void handoff_engine_begin(void);
/* Ren'Py's thread: gives control back and waits for the next frame. False once the core is stopping, and then
 * at once on every later call. */
bool handoff_yield(void);
/* Ren'Py's thread, last. */
void handoff_engine_end(void);

bool handoff_on_engine_thread(void);
bool handoff_stopping(void);
bool handoff_engine_ended(void);
int64_t handoff_now_usec(void); /* the clock: microseconds of the game's time since it started */

#endif
