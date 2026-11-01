/*
 * Dulcamara - the settings file (settings.ini in the data folder) and the data folder's layout.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_CONFIG_H
#define NP_CONFIG_H

#include "nightshade.h"

void config_defaults(Settings *s);
/* Reads the file over the defaults; a missing file or key keeps the default. */
void config_load(Settings *s, const char *path);
bool config_save(const Settings *s, const char *path);
/* The data folder's paths below root. */
void config_paths(Paths *p, const char *root);
/* The game buttons back to the core's defaults (cores.h). */
void config_default_buttons(Settings *s, int core);

/* A Ren'Py game's keys (Dulcamara's menu > Keys for this game): what each of the RetroPad's buttons (PAD_*) presses
 * in it, which the core's renpy_key_<button> options tell it. Kept in keys/<the game's id>.txt in the data folder,
 * one "button = value" a line for each button that does not do the core's own. */
typedef struct
{
    const char *value; /* the option's value: "default", "none", "return", "f1", "a", "click"... */
    const char *name;  /* what the keys page calls it */
    int kind;          /* the core's own, nothing and the mouse; keys; letters; digits; F keys (L1 / R1 jump) */
} KeyAction;

#define KEY_ACTION_COUNT 70
/* In the order the keys page goes through them; 0 is "default", the core's own. */
extern const KeyAction KEY_ACTIONS[KEY_ACTION_COUNT];

/* A RetroPad button's core option, "renpy_key_y"; NULL for R2 (the app's fast-forward) and anything that is not a
 * button. */
const char *config_key_option(int pad);
/* What the core's own is for a RetroPad button (input.c in src/core/renpy): its KEY_ACTIONS index. */
int config_key_default(int pad);
/* A game's keys file into keys (each RetroPad button's KEY_ACTIONS index): 0 for every button it does not name, and
 * for all of them when it is missing or unreadable. */
void config_load_keys(int8_t keys[MAP_BUTTONS], const char *path);
/* The buttons that are not at 0, under a line that names the game; with none, the file goes. */
bool config_save_keys(const int8_t keys[MAP_BUTTONS], const char *path, const char *title);

#endif
