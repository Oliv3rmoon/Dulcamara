/*
 * Dulcamara - Ren'Py's core: Ren'Py's own thread and the Python it runs in.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_PYTHON_H
#define DR_PYTHON_H

#include <stdbool.h>
#include <stddef.h>

/* Starts Ren'Py on its own thread for the game in game_dir (the folder that holds game/), with the engine folder
 * engine_dir (renpy8/) and its saves in saves_dir. False with a reason when it cannot start, and for every game
 * after the first in this process (python.c says why). */
bool python_start(const char *engine_dir, const char *game_dir, const char *saves_dir, char *error, size_t size);
/* Asks Ren'Py to quit at once and waits for its thread, ten seconds at most. */
void python_stop(void);
/* Ren'Py's thread, as it looks for events: whether Ren'Py wants text typed (it shows an input, and has asked
 * pygame's start_text_input), and where that input is: its top and bottom, from 0 to 1 down the picture. */
bool python_text_field(float *top, float *bottom);
/* Ren'Py's thread: text typed (UTF-8) to Ren'Py, as pygame's own TEXTINPUT event; false when it could not be. */
bool python_type_text(const char *utf8);

#endif
