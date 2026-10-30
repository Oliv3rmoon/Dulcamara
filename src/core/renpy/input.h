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
/* The frontend: fast-forward on or off. While on, Ren'Py's skip key (Left Ctrl) is held, so dialogue that was read
 * goes by, as players of visual novels expect fast-forward to do. */
void input_set_skip(bool on);
/* The frontend: the pointer (the touchpad), x and y from -32767 to 32767 across the picture, and whether it is pressed
 * (a tap). It is Ren'Py's mouse: moved when x or y change, its left button down while pressed. */
void input_set_pointer(int16_t x, int16_t y, bool pressed);
/* Ren'Py's thread, as it looks for events: the pointer's latest moves, given to SDL for `window` (w x h). SDL's mouse
 * is not to be moved from another thread. */
struct SDL_Window;
void input_pump(struct SDL_Window *window, int w, int h);
void input_detach(void);

#endif
