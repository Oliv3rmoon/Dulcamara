/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "profile.h"
#include "storage.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

typedef struct
{
    const char *name;
    size_t offset;
    bool boolean;
    int min, max;
} Field;
#define BOOL(name) {#name, offsetof(Settings, name), true, 0, 1}
#define INT(name, lo, hi) {#name, offsetof(Settings, name), false, lo, hi}
static const Field fields[PROFILE_FIELD_COUNT] = {
    BOOL(integer_scale),     BOOL(smooth),        INT(shader, 0, 99),
    INT(aspect, 0, 1),       INT(upscale, 1, 3),  INT(upscale_filter, 0, UPSCALE_FILTER_COUNT - 1),
    INT(game_volume, 0, 10), INT(ff_speed, 0, 4), INT(ff_toggle, 0, 1),
    BOOL(stick_dpad),        BOOL(show_fps),      BOOL(renpy_skip_unseen),
    BOOL(automatic_backups),
};

bool profile_set(GameProfile *profile, ProfileField field, int value)
{
    if (field < 0 || field >= PROFILE_FIELD_COUNT || value < fields[field].min ||
        value > fields[field].max)
        return false;
    profile->values[field] = value;
    profile->present |= BIT(field);
    return true;
}

void profile_reset(GameProfile *profile, ProfileField field)
{
    if (field >= 0 && field < PROFILE_FIELD_COUNT)
    {
        profile->present &= ~BIT(field);
        profile->values[field] = 0;
    }
}

static bool valid_button(int core, int button)
{
    return core >= 0 && core < NP_CORES && button >= 0 && button < MAP_BUTTONS;
}

bool profile_set_button(GameProfile *profile, int core, int button, int mapping)
{
    if (!valid_button(core, button) || mapping < -1 || mapping >= MAP_BUTTONS)
        return false;
    profile->button_present[core] |= BIT(button);
    profile->button_map[core][button] = (int8_t)mapping;
    return true;
}

void profile_reset_button(GameProfile *profile, int core, int button)
{
    if (valid_button(core, button))
    {
        profile->button_present[core] &= ~BIT(button);
        profile->button_map[core][button] = 0;
    }
}

void profile_resolve(const Settings *global, const GameProfile *profile, Settings *effective)
{
    if (effective == global)
        return;
    *effective = *global;
    for (int i = 0; i < PROFILE_FIELD_COUNT; ++i)
        if ((profile->present & BIT(i)) && profile->values[i] >= fields[i].min &&
            profile->values[i] <= fields[i].max)
        {
            void *out = (char *)effective + fields[i].offset;
            if (fields[i].boolean)
                *(bool *)out = profile->values[i] != 0;
            else
                *(int *)out = profile->values[i];
        }
    for (int core = 0; core < NP_CORES; ++core)
        for (int button = 0; button < MAP_BUTTONS; ++button)
            if ((profile->button_present[core] & BIT(button)) &&
                profile->button_map[core][button] >= -1 &&
                profile->button_map[core][button] < MAP_BUTTONS)
                effective->button_map[core][button] = profile->button_map[core][button];
}

bool profile_load(GameProfile *profile, const char *path)
{
    memset(profile, 0, sizeof(*profile));
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    GameProfile loaded = {0};
    char line[128];
    bool ok = fgets(line, sizeof(line), f) &&
              (!strcmp(line, "profile_version = 1\n") || !strcmp(line, "profile_version = 1\r\n"));
    bool complete = false;
    while (ok && fgets(line, sizeof(line), f))
    {
        if (!strcmp(line, "profile_end\n") || !strcmp(line, "profile_end\r\n"))
        {
            complete = true;
            if (fgetc(f) != EOF)
                ok = false;
            break;
        }
        char key[64], extra, *end;
        int value_start = 0;
        if (!strchr(line, '\n') || sscanf(line, " %63[^= ] = %n", key, &value_start) != 1 ||
            !value_start)
        {
            ok = false;
            break;
        }
        /* Parse through the real line ending; a bounded copy could hide an invalid suffix. */
        char *value_text = line + value_start;
        errno = 0;
        long value = strtol(value_text, &end, 10);
        if (end == value_text || errno || value < INT_MIN || value > INT_MAX)
        {
            ok = false;
            break;
        }
        while (isspace((unsigned char)*end))
            ++end;
        if (*end)
        {
            ok = false;
            break;
        }
        bool found = false;
        for (int i = 0; i < PROFILE_FIELD_COUNT; ++i)
            if (!strcmp(key, fields[i].name))
            {
                found = true;
                ok = profile_set(&loaded, (ProfileField)i, (int)value);
                break;
            }
        if (!found)
        {
            int core, button;
            /* Tiny bounded indices: avoid scanf integer overflow on hostile input. */
            ok = strlen(key) <= 12 &&
                 sscanf(key, "button_%1d_%2d%c", &core, &button, &extra) == 2 &&
                 profile_set_button(&loaded, core, button, (int)value);
        }
    }
    if (!complete || (ferror)(f))
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (ok)
        *profile = loaded;
    return ok;
}

bool profile_save(const GameProfile *profile, const char *path)
{
    StorageFile write;
    if (!storage_begin(&write, path))
        return false;
    fputs("profile_version = 1\n", write.file);
    bool ok = !(profile->present & ~((1u << PROFILE_FIELD_COUNT) - 1));
    for (int i = 0; i < PROFILE_FIELD_COUNT; ++i)
        if (profile->present & BIT(i))
        {
            if (profile->values[i] < fields[i].min || profile->values[i] > fields[i].max)
                ok = false;
            fprintf(write.file, "%s = %d\n", fields[i].name, profile->values[i]);
        }
    for (int core = 0; core < NP_CORES; ++core)
        for (int button = 0; button < MAP_BUTTONS; ++button)
            if (profile->button_present[core] & BIT(button))
            {
                int mapping = profile->button_map[core][button];
                if (mapping < -1 || mapping >= MAP_BUTTONS)
                    ok = false;
                fprintf(write.file, "button_%d_%d = %d\n", core, button, mapping);
            }
    if (!ok)
    {
        storage_abort(&write);
        return false;
    }
    fputs("profile_end\n", write.file);
    return storage_commit(&write);
}
