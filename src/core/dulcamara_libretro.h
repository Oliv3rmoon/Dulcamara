/*
 * Dulcamara - its own environment call, beyond libretro's (RETRO_ENVIRONMENT_PRIVATE): what a core of Dulcamara's
 * tells its frontend (core/host.c, and the cores' headless checks) that libretro has no call for. The cores' own C
 * includes it too.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_DULCAMARA_LIBRETRO_H
#define NP_DULCAMARA_LIBRETRO_H

#include "libretro.h"

#include <stdbool.h>

/* const struct retro_dulcamara_text_input *: whether the game wants text typed (a Ren'Py game shows an input: a name
 * to enter), sent by the core each time that changes. Meanwhile the frontend's on-screen keyboard has the controller
 * and types into the keyboard callback the core set (RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK): a character as its
 * character (keycode RETROK_UNKNOWN), Backspace and Return as their keycodes. The console's own keyboard cannot be
 * used: a title that imports it does not start (src/platform/ps5_shims.c). */
#define RETRO_ENVIRONMENT_DULCAMARA_TEXT_INPUT (1 | RETRO_ENVIRONMENT_PRIVATE)

struct retro_dulcamara_text_input
{
    bool wanted;
    /* the game's own input, from 0 (the picture's top) to 1 (its foot), for a keyboard that leaves it in view; both 0
     * when the core does not know */
    float top, bottom;
};

#endif
