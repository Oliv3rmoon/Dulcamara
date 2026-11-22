/* Sparse settings editor. Never swaps global Settings into gameplay. GPL-3.0-or-later */
#include "panels.h"
#include "controller.h"
#include "../app.h"
#include "../profile.h"
#include "../config.h"
#include "draw.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static int game_index;
static GameProfile profile;
static UiList list;
static char path[NP_PATH_MAX];
static const char *names[PROFILE_FIELD_COUNT] = {"Whole-number scaling",
                                                 "Smooth scaling",
                                                 "Shader",
                                                 "Shape",
                                                 "Upscaling",
                                                 "Upscaling filter",
                                                 "Game volume",
                                                 "Fast-forward speed",
                                                 "Fast-forward toggle",
                                                 "Left stick moves",
                                                 "Show FPS",
                                                 "Ren'Py skip unread text",
                                                 "Automatic local backups"};
static const int minimum[PROFILE_FIELD_COUNT] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
static const int maximum[PROFILE_FIELD_COUNT] = {1, 1, 16, 1, 3, 2, 10, 4, 1, 1, 1, 1, 1};
static const char *buttons[MAP_BUTTONS] = {"Cross", "Square", "Create", "Options",  "Up", "Down",
                                           "Left",  "Right",  "Circle", "Triangle", "L1", "R1",
                                           "L2",    "R2",     "L3",     "R3"};
static const int shaders[] = {0, 13, 2, 9, 10, 14, 15, 16, 5, 6, 1, 12, 11};
static const char *shader_names[] = {
    "Off",          "CRT Royale",       "CRT Royale Fast", "CRT Lottes",   "CRT Pi",
    "CRT Newpixie", "CRT Hyllian NTSC", "CRT Mattias",     "NTSC S-Video", "NTSC Composite",
    "LCD3x",        "zfast LCD",        "Game Boy LCD"};
static const char *pad_names[] = {
    "B / Cancel",   "Y / Shift", "Select", "Start", "Up", "Down", "Left", "Right",
    "A / Decision", "X / Menu",  "L",      "R",     "L2", "R2",   "L3",   "R3"};
static void display_value(char *out, size_t n, int field, int v)
{
    if (field == PROFILE_SHADER)
    {
        for (int i = 0; i < 13; i++)
            if (shaders[i] == v)
            {
                snprintf(out, n, "%s", shader_names[i]);
                return;
            }
        snprintf(out, n, "Unknown (%d)", v);
        return;
    }
    if (field == PROFILE_GAME_VOLUME)
    {
        snprintf(out, n, "%d%%", v * 10);
        return;
    }
    if (field == PROFILE_FF_SPEED)
    {
        const int speed[] = {2, 3, 4, 6, 8};
        snprintf(out, n, "%dx", speed[v % 5]);
        return;
    }
    if (field == PROFILE_UPSCALE)
    {
        snprintf(out, n, "%dx", v);
        return;
    }
    if (field == PROFILE_UPSCALE_FILTER)
    {
        const char *filters[] = {"Sharp pixels", "Scale2x", "xBR"};
        snprintf(out, n, "%s", filters[v % 3]);
        return;
    }
    if (field == PROFILE_ASPECT)
    {
        snprintf(out, n, "%s", v ? "16:9" : "Game aspect");
        return;
    }
    snprintf(out, n, "%s", v ? "On" : "Off");
}
static int value(const Settings *s, int f)
{
    switch (f)
    {
    case PROFILE_INTEGER_SCALE:
        return s->integer_scale;
    case PROFILE_SMOOTH:
        return s->smooth;
    case PROFILE_SHADER:
        return s->shader;
    case PROFILE_ASPECT:
        return s->aspect;
    case PROFILE_UPSCALE:
        return s->upscale;
    case PROFILE_UPSCALE_FILTER:
        return s->upscale_filter;
    case PROFILE_GAME_VOLUME:
        return s->game_volume;
    case PROFILE_FF_SPEED:
        return s->ff_speed;
    case PROFILE_FF_TOGGLE:
        return s->ff_toggle;
    case PROFILE_STICK_DPAD:
        return s->stick_dpad;
    case PROFILE_SHOW_FPS:
        return s->show_fps;
    case PROFILE_RENPY_SKIP_UNSEEN:
        return s->renpy_skip_unseen;
    default:
        return s->automatic_backups;
    }
}
void preferences_open(int i)
{
    game_index = i;
    char name[40];
    snprintf(name, sizeof(name), "%s.ini", app.library.games[i].id);
    path_join(path, sizeof(path), app.paths.profiles, name);
    profile_load(&profile, path);
    list = (UiList){0};
    app.screen = SCREEN_PROFILE;
}
void preferences_screen(uint32_t p)
{
    if (p & BIT(BTN_CIRCLE))
    {
        app.screen = SCREEN_DETAILS;
        return;
    }
    int count = PROFILE_FIELD_COUNT + MAP_BUTTONS, core = app.library.games[game_index].core;
    ui_list_move(&list, (p & BIT(BTN_DOWN)) ? 1 : (p & BIT(BTN_UP)) ? -1 : 0, count, 8);
    Settings resolved;
    profile_resolve(&app.settings, &profile, &resolved);
    if (p & (BIT(BTN_CROSS) | BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_SQUARE)))
    {
        GameProfile edited = profile;
        int f = list.row, step = (p & BIT(BTN_LEFT)) ? -1 : 1;
        if (f < PROFILE_FIELD_COUNT)
        {
            if (p & BIT(BTN_SQUARE))
                profile_reset(&edited, f);
            else
            {
                int v = value(&resolved, f) + step;
                if (v < minimum[f])
                    v = maximum[f];
                if (v > maximum[f])
                    v = minimum[f];
                if (f == PROFILE_SHADER)
                {
                    int at = 0;
                    for (int i = 0; i < 13; i++)
                        if (shaders[i] == value(&resolved, f))
                            at = i;
                    v = shaders[(at + step + 13) % 13];
                }
                profile_set(&edited, f, v);
            }
        }
        else
        {
            int b = f - PROFILE_FIELD_COUNT;
            if (p & BIT(BTN_SQUARE))
                profile_reset_button(&edited, core, b);
            else
            {
                int v = resolved.button_map[core][b] + step;
                if (v >= MAP_BUTTONS)
                    v = -1;
                if (v < -1)
                    v = MAP_BUTTONS - 1;
                profile_set_button(&edited, core, b, v);
            }
        }
        make_dirs(app.paths.profiles);
        if (profile_save(&edited, path))
        {
            profile = edited;
            app_apply_settings();
        }
        else
            app_message_box("Game preference could not be saved. The previous value remains "
                            "effective. Check storage space and permissions.");
        profile_resolve(&app.settings, &profile, &resolved);
    }
    panel_begin(
        app.library.games[game_index].title,
        "Game preferences  |  Left/Right override  |  Square resets this field to inheritance");
    for (int r = list.top; r < count && r < list.top + 8; r++)
    {
        char v[128], label[80];
        bool own;
        if (r < PROFILE_FIELD_COUNT)
        {
            own = (profile.present & BIT(r)) != 0;
            snprintf(label, sizeof(label), "%s", names[r]);
            char shown[64];
            display_value(shown, sizeof(shown), r, value(&resolved, r));
            snprintf(v, sizeof(v), "%s: %s", own ? "Override" : "Inherited", shown);
        }
        else
        {
            int b = r - PROFILE_FIELD_COUNT;
            own = (profile.button_present[core] & BIT(b)) != 0;
            snprintf(label, sizeof(label), "Controller: %s", buttons[b]);
            int mapped = resolved.button_map[core][b];
            snprintf(v, sizeof(v), "%s: %s", own ? "Override" : "Inherited",
                     mapped < 0 ? "Nothing" : pad_names[mapped]);
        }
        panel_row(r - list.top, label, v, r == list.row);
    }
    char note[128];
    snprintf(
        note, sizeof(note),
        "%d / %d  |  Global changes affect inherited fields. Each game keeps its own overrides.",
        list.row + 1, count);
    panel_note(850, note);
    int g[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_CIRCLE};
    const char *l[] = {"Override", "Inherit", "Back"};
    app_draw_hints(g, l, 3, NULL);
}
