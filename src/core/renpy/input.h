/*
 * Dulcamara - Ren'Py's core: the controller Ren'Py sees, an SDL virtual controller the RetroPad drives. Ren'Py's
 * own controller bindings then apply: SDL's A (the bottom button, Cross) confirms, B (Circle) opens the game menu,
 * the shoulders roll back and forward, the D-pad moves the focus.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_INPUT_H
#define DR_INPUT_H

#include <stdbool.h>
#include <stdint.h>

void input_attach(void); /* once SDL's joysticks are up (Ren'Py's thread) */
bool input_is_ours(int device_index);
/* The frontend: the RetroPad's buttons (bit = RETRO_DEVICE_ID_JOYPAD_*) and left stick. */
void input_set(uint16_t retropad, int16_t left_x, int16_t left_y);
void input_detach(void);

#endif
