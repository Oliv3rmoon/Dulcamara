/*
 * Dulcamara - the settings file (config.h): one "key = value" a line.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.h"

#include "core/cores.h"
#include "ui/sfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_default_buttons(Settings *s, int core)
{
    core = core >= 0 && core < CORE_COUNT ? core : CORE_EASYRPG;
    memcpy(s->button_map[core], CORES[core].buttons, sizeof(s->button_map[core]));
}

/* "button_<n>" is EasyRPG Player's (the key the settings file has always had), "<core>_button_<n>" the others'. */
static void button_key(char *out, size_t size, int core, int button)
{
    if (core == CORE_EASYRPG)
        snprintf(out, size, "button_%d", button);
    else
        snprintf(out, size, "%s_button_%d", CORES[core].id, button);
}

void config_defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->integer_scale = false;
    s->smooth = false; /* sharp pixels: RPG Maker's art is pixel art */
    s->shader = 0;
    s->mc_w = s->mc_h = 100;
    s->aspect = 0;
    s->upscale = 1;
    s->upscale_filter = UPSCALE_SHARP;
    s->ff_speed = 1; /* x3 */
    s->ff_toggle = 0;
    s->stick_dpad = true;
    s->show_fps = false;
    s->renpy_skip_unseen = false;
    for (int core = 0; core < CORE_COUNT; ++core)
        config_default_buttons(s, core);
    s->ui_sound = SFX_STYLE_SOFT;
    s->ui_volume = 2;
    s->game_volume = 10;
    s->update_check = true;
    s->update_test_builds = false;
    s->sort_mode = 0;
    s->last_game = 0;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void config_load(Settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64], value[128];
        if (sscanf(line, " %63[^= ] = %127[^\r\n]", key, value) != 2)
            continue;
        const int v = atoi(value);
        if (!strcmp(key, "integer_scale"))
            s->integer_scale = v != 0;
        else if (!strcmp(key, "smooth"))
            s->smooth = v != 0;
        else if (!strcmp(key, "shader"))
            s->shader = clamp(v, 0, 99);
        else if (!strcmp(key, "aspect"))
            s->aspect = clamp(v, 0, 1);
        else if (!strcmp(key, "upscale"))
            s->upscale = clamp(v, 1, 3);
        else if (!strcmp(key, "upscale_filter"))
            s->upscale_filter = clamp(v, 0, UPSCALE_FILTER_COUNT - 1);
        else if (!strcmp(key, "ff_speed"))
            s->ff_speed = clamp(v, 0, 4);
        else if (!strcmp(key, "ff_toggle"))
            s->ff_toggle = v != 0;
        else if (!strcmp(key, "stick_dpad"))
            s->stick_dpad = v != 0;
        else if (!strcmp(key, "show_fps"))
            s->show_fps = v != 0;
        else if (!strcmp(key, "renpy_skip_unseen"))
            s->renpy_skip_unseen = v != 0;
        else if (!strcmp(key, "ui_sound"))
            s->ui_sound = clamp(v, 0, SFX_STYLE_OFF);
        else if (!strcmp(key, "ui_volume"))
            s->ui_volume = clamp(v, 0, 3);
        else if (!strcmp(key, "game_volume"))
            s->game_volume = clamp(v, 0, 10);
        else if (!strcmp(key, "update_check"))
            s->update_check = v != 0;
        else if (!strcmp(key, "update_test_builds"))
            s->update_test_builds = v != 0;
        else if (!strcmp(key, "community_seen"))
            s->community_seen = v != 0;
        else if (!strcmp(key, "sort_mode"))
            s->sort_mode = clamp(v, 0, 1);
        else if (!strcmp(key, "last_game"))
            s->last_game = v < 0 ? 0 : v;
        else
            for (int core = 0; core < CORE_COUNT; ++core)
                for (int b = 0; b < MAP_BUTTONS; ++b)
                {
                    char name[48];
                    button_key(name, sizeof(name), core, b);
                    if (!strcmp(key, name))
                        s->button_map[core][b] = (int8_t)clamp(v, -1, 15);
                }
    }
    fclose(f);
}

bool config_save(const Settings *s, const char *path)
{
    char temp[NP_PATH_MAX];
    snprintf(temp, sizeof(temp), "%s.new", path);
    FILE *f = fopen(temp, "w");
    if (!f)
        return false;
    fprintf(f, "# Dulcamara %s settings\n", NP_VERSION);
    fprintf(f, "version = %s\n", NP_VERSION);
    fprintf(f, "integer_scale = %d\nsmooth = %d\nshader = %d\naspect = %d\nupscale = %d\nupscale_filter = %d\n",
            s->integer_scale, s->smooth, s->shader, s->aspect, s->upscale, s->upscale_filter);
    fprintf(f, "ff_speed = %d\nff_toggle = %d\nstick_dpad = %d\nshow_fps = %d\nrenpy_skip_unseen = %d\n", s->ff_speed,
            s->ff_toggle, s->stick_dpad, s->show_fps, s->renpy_skip_unseen);
    for (int core = 0; core < CORE_COUNT; ++core)
        for (int b = 0; b < MAP_BUTTONS; ++b)
        {
            char name[48];
            button_key(name, sizeof(name), core, b);
            fprintf(f, "%s = %d\n", name, s->button_map[core][b]);
        }
    fprintf(f, "ui_sound = %d\nui_volume = %d\ngame_volume = %d\nupdate_check = %d\nsort_mode = %d\nlast_game = %d\n",
            s->ui_sound, s->ui_volume, s->game_volume, s->update_check, s->sort_mode, s->last_game);
    fprintf(f, "community_seen = %d\n", s->community_seen);
    fprintf(f, "update_test_builds = %d\n", s->update_test_builds);
    const bool ok = fclose(f) == 0;
    if (ok)
    {
        remove(path);
        rename(temp, path);
    }
    return ok;
}

void config_paths(Paths *p, const char *root)
{
    memset(p, 0, sizeof(*p));
    str_copy(p->root, sizeof(p->root), root);
    path_join(p->games, sizeof(p->games), root, "games");
    path_join(p->rtp, sizeof(p->rtp), root, "rtp");
    path_join(p->logs, sizeof(p->logs), root, "logs");
    path_join(p->cache, sizeof(p->cache), root, "cache");
    path_join(p->config, sizeof(p->config), root, "settings.ini");
    path_join(p->played, sizeof(p->played), root, "played.txt");
    path_join(p->keys, sizeof(p->keys), root, "keys");
}

/* ---------------------------------------------------------------- a Ren'Py game's keys */

#define LETTER(c, C) {#c, #C, 2}
#define DIGIT(d) {#d, #d, 3}
#define F_KEY(n) {"f" #n, "F" #n, 4}

/* what src/core/renpy/input.c takes: the core's own, nothing and the mouse; then the keys, the letters, the digits and
 * the F keys */
const KeyAction KEY_ACTIONS[] = {
    {"default", "Default", 0},       {"none", "Nothing", 0},          {"click", "Left click", 0},
    {"rightclick", "Right click", 0}, {"cursor", "Cursor on and off", 0},
    {"return", "Return", 1},         {"escape", "Escape", 1},         {"space", "Space", 1},
    {"tab", "Tab", 1},               {"backspace", "Backspace", 1},   {"delete", "Delete", 1},
    {"shift", "Shift", 1},           {"ctrl", "Ctrl", 1},             {"alt", "Alt", 1},
    {"up", "Up arrow", 1},           {"down", "Down arrow", 1},       {"left", "Left arrow", 1},
    {"right", "Right arrow", 1},     {"pageup", "Page Up", 1},        {"pagedown", "Page Down", 1},
    {"home", "Home", 1},             {"end", "End", 1},
    LETTER(a, A), LETTER(b, B), LETTER(c, C), LETTER(d, D), LETTER(e, E), LETTER(f, F), LETTER(g, G),
    LETTER(h, H), LETTER(i, I), LETTER(j, J), LETTER(k, K), LETTER(l, L), LETTER(m, M), LETTER(n, N),
    LETTER(o, O), LETTER(p, P), LETTER(q, Q), LETTER(r, R), LETTER(s, S), LETTER(t, T), LETTER(u, U),
    LETTER(v, V), LETTER(w, W), LETTER(x, X), LETTER(y, Y), LETTER(z, Z),
    DIGIT(0), DIGIT(1), DIGIT(2), DIGIT(3), DIGIT(4), DIGIT(5), DIGIT(6), DIGIT(7), DIGIT(8), DIGIT(9),
    F_KEY(1), F_KEY(2), F_KEY(3), F_KEY(4), F_KEY(5), F_KEY(6), F_KEY(7), F_KEY(8), F_KEY(9), F_KEY(10),
    F_KEY(11), F_KEY(12),
};
_Static_assert(sizeof(KEY_ACTIONS) / sizeof(KEY_ACTIONS[0]) == KEY_ACTION_COUNT, "KEY_ACTION_COUNT (config.h)");

/* each RetroPad button's option (PAD_* order; R2 is the app's fast-forward), and the core's own for it: Cross Return,
 * Square H (Ren'Py hides its text), Options and Circle Escape (the menu), the D-pad the arrows, Triangle Tab (skip),
 * L1 and R1 Page Up and Down (back and forward), L3 the cursor; Create, L2 and R3 nothing */
#define KEY_PREFIX "renpy_key_" /* the file names a button by the rest: "y = i" */
static const char *const KEY_OPTIONS[MAP_BUTTONS] = {
    KEY_PREFIX "b",    KEY_PREFIX "y",     KEY_PREFIX "select", KEY_PREFIX "start", KEY_PREFIX "up", KEY_PREFIX "down",
    KEY_PREFIX "left", KEY_PREFIX "right", KEY_PREFIX "a",      KEY_PREFIX "x",     KEY_PREFIX "l",  KEY_PREFIX "r",
    KEY_PREFIX "l2",   NULL,               KEY_PREFIX "l3",     KEY_PREFIX "r3"};
static const char *const KEY_DEFAULTS[MAP_BUTTONS] = {"return", "h",     "none",   "escape", "up",     "down",
                                                      "left",   "right", "escape", "tab",    "pageup", "pagedown",
                                                      "none",   "none",  "cursor", "none"};

static int key_action(const char *value)
{
    for (int i = 0; i < KEY_ACTION_COUNT; ++i)
        if (!strcmp(KEY_ACTIONS[i].value, value))
            return i;
    return -1;
}

const char *config_key_option(int pad)
{
    return pad >= 0 && pad < MAP_BUTTONS ? KEY_OPTIONS[pad] : NULL;
}

int config_key_default(int pad)
{
    const int action = pad >= 0 && pad < MAP_BUTTONS ? key_action(KEY_DEFAULTS[pad]) : -1;
    return action < 0 ? 0 : action;
}

void config_load_keys(int8_t keys[MAP_BUTTONS], const char *path)
{
    memset(keys, 0, MAP_BUTTONS);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char button[32], value[32];
        if (line[0] == '#' || sscanf(line, " %31[^= ] = %31s", button, value) != 2)
            continue;
        const int action = key_action(value);
        for (int pad = 0; pad < MAP_BUTTONS && action >= 0; ++pad)
            if (KEY_OPTIONS[pad] && !strcmp(KEY_OPTIONS[pad] + strlen(KEY_PREFIX), button))
                keys[pad] = (int8_t)action;
    }
    fclose(f);
}

bool config_save_keys(const int8_t keys[MAP_BUTTONS], const char *path, const char *title)
{
    bool any = false;
    for (int pad = 0; pad < MAP_BUTTONS; ++pad)
        any |= KEY_OPTIONS[pad] && keys[pad] > 0 && keys[pad] < KEY_ACTION_COUNT;
    if (!any)
        return remove(path) == 0 || !path_exists(path);
    char temp[NP_PATH_MAX], name[160];
    snprintf(temp, sizeof(temp), "%s.new", path);
    FILE *f = fopen(temp, "w");
    if (!f)
        return false;
    /* the game's name on the first line, nothing in it that could start another */
    str_copy(name, sizeof(name), title);
    for (char *p = name; *p; ++p)
        if ((unsigned char)*p < 0x20)
            *p = ' ';
    fprintf(f, "# Dulcamara %s: the keys of %s (Dulcamara's menu > Keys for this game)\n", NP_VERSION, name);
    fprintf(f, "# a RetroPad button = a key, click, rightclick, cursor or none; the others do the core's own\n");
    for (int pad = 0; pad < MAP_BUTTONS; ++pad)
        if (KEY_OPTIONS[pad] && keys[pad] > 0 && keys[pad] < KEY_ACTION_COUNT)
            fprintf(f, "%s = %s\n", KEY_OPTIONS[pad] + strlen(KEY_PREFIX), KEY_ACTIONS[keys[pad]].value);
    const bool ok = fclose(f) == 0;
    if (ok)
    {
        remove(path);
        rename(temp, path);
    }
    return ok;
}
