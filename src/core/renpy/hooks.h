/*
 * Dulcamara - Ren'Py's core: what the SDL calls renamed in Ren'Py's code (sdl_renames.h) do instead (hooks.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_HOOKS_H
#define DR_HOOKS_H

#include <stdint.h>

#define DR_MAX_W 1920 /* the largest picture */
#define DR_MAX_H 1080
#define DR_DISPLAY_W 3840 /* the display Ren'Py is told it has: room for any window it asks for */
#define DR_DISPLAY_H 2160

void hooks_reset(void); /* before a game: no timers, no window, GL's attributes at SDL's defaults */
/* Ren'Py's thread: waits until the clock reaches until_usec, firing its timers and giving control back each frame. */
void hooks_sleep_until(int64_t until_usec);

#endif
