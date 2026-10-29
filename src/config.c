/*
 * Nightshade Player - the settings file (config.h): one "key = value" a line.
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
    const CoreInfo *c = &CORES[core >= 0 && core < CORE_COUNT ? core : CORE_EASYRPG];
    memcpy(s->button_map, c->buttons, sizeof(s->button_map));
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
    config_default_buttons(s, CORE_EASYRPG);
    s->ui_sound = SFX_STYLE_SOFT;
    s->ui_volume = 2;
    s->game_volume = 10;
    s->update_check = true;
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
        else if (!strcmp(key, "ui_sound"))
            s->ui_sound = clamp(v, 0, SFX_STYLE_OFF);
        else if (!strcmp(key, "ui_volume"))
            s->ui_volume = clamp(v, 0, 3);
        else if (!strcmp(key, "game_volume"))
            s->game_volume = clamp(v, 0, 10);
        else if (!strcmp(key, "update_check"))
            s->update_check = v != 0;
        else if (!strcmp(key, "sort_mode"))
            s->sort_mode = clamp(v, 0, 1);
        else if (!strcmp(key, "last_game"))
            s->last_game = v < 0 ? 0 : v;
        else if (!strncmp(key, "button_", 7))
        {
            const int b = atoi(key + 7);
            if (b >= 0 && b < MAP_BUTTONS)
                s->button_map[b] = (int8_t)clamp(v, -1, 15);
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
    fprintf(f, "# Nightshade Player %s settings\n", NP_VERSION);
    fprintf(f, "version = %s\n", NP_VERSION);
    fprintf(f, "integer_scale = %d\nsmooth = %d\nshader = %d\naspect = %d\nupscale = %d\nupscale_filter = %d\n",
            s->integer_scale, s->smooth, s->shader, s->aspect, s->upscale, s->upscale_filter);
    fprintf(f, "ff_speed = %d\nff_toggle = %d\nstick_dpad = %d\nshow_fps = %d\n", s->ff_speed, s->ff_toggle,
            s->stick_dpad, s->show_fps);
    for (int b = 0; b < MAP_BUTTONS; ++b)
        fprintf(f, "button_%d = %d\n", b, s->button_map[b]);
    fprintf(f, "ui_sound = %d\nui_volume = %d\ngame_volume = %d\nupdate_check = %d\nsort_mode = %d\nlast_game = %d\n",
            s->ui_sound, s->ui_volume, s->game_volume, s->update_check, s->sort_mode, s->last_game);
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
}
