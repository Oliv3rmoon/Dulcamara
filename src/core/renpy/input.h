/*
 * Dulcamara - Ren'Py's core: the controller as Ren'Py's keyboard and mouse. Each RetroPad button is a key of Ren'Py's
 * keymap (Cross Return, Circle Escape, the D-pad the arrows, L1 and R1 Page Up and Down...) or a click, and a game's
 * own keys can be given to any of them (renpy_key_* options). L3 turns the cursor on: the left stick moves a pointer,
 * drawn on the picture, and Cross clicks. SDL's events carry them all, so no window's focus and no controller mapping
 * stands in their way; Ren'Py 7 takes them as Ren'Py 8 does.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_INPUT_H
#define DR_INPUT_H

#include <stdbool.h>
#include <stdint.h>

void input_attach(void); /* once SDL's events are up (Ren'Py's thread) */
bool input_is_ours(int device_index); /* no SDL controller is Ren'Py's: it has the keyboard and the mouse */
/* The frontend: what a RetroPad button (RETRO_DEVICE_ID_JOYPAD_*) does, from a renpy_key_* option's value:
 * "default", "none", a key ("return", "escape", "space", "tab", "pageup", "f1", "a", "0"...), "cursor", "click" or
 * "rightclick". False for a value it does not know (the button keeps its default). */
bool input_bind(unsigned button, const char *value);
/* The frontend: the RetroPad's buttons (bit = RETRO_DEVICE_ID_JOYPAD_*) and left stick, once a frame. */
void input_set(uint16_t retropad, int16_t left_x, int16_t left_y);
/* The frontend: fast-forward on or off. While on, Ren'Py's skip key (Left Ctrl) is held, so dialogue that was read
 * goes by, as players of visual novels expect fast-forward to do. */
void input_set_skip(bool on);
/* The frontend: the pointer (the touchpad), x and y from -32767 to 32767 across the picture, and whether it is pressed
 * (a tap). It is Ren'Py's mouse: moved when x or y change, its left button down while pressed. */
void input_set_pointer(int16_t x, int16_t y, bool pressed);
/* Ren'Py's thread, as it looks for events: the pointer's latest moves and clicks, given to SDL for `window` (w x h).
 * SDL's mouse is not to be moved from another thread. */
struct SDL_Window;
void input_pump(struct SDL_Window *window, int w, int h);
/* The cursor to draw on a w x h picture, if it is shown (cursor mode, or the touchpad moved it lately). */
bool input_cursor(int w, int h, int *x, int *y);
void input_detach(void);

#endif
