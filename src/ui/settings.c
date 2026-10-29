/*
 * Nightshade Player - Settings: tabs (L1 / R1) of rows (up / down), each changed with left / right or Cross.
 * Opened from a game, the game shows behind it, so a shader or a scale is seen as it is chosen.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../config.h"
#include "../core/cores.h"
#include "../core/host.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "../update.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum Kind
{
    K_CHOICE, /* an int field, its values named */
    K_TOGGLE, /* a bool field */
    K_ACTION, /* Cross does something */
    K_INFO,   /* a line of text */
};

enum Action
{
    A_NONE,
    A_DEFAULT_BUTTONS,
    A_RESCAN,
    A_CHECK_UPDATE,
    A_CLOSE_APP,
};

typedef struct
{
    const char *label;
    const char *help;
    int kind;
    size_t field;              /* offsetof(Settings, ...) for K_CHOICE / K_TOGGLE */
    const char *const *names;  /* K_CHOICE: what each value is called */
    int count;
    const int *values;         /* K_CHOICE: the stored values, NULL when they are 0..count-1 */
    int action;                /* K_ACTION */
    int button;                /* the controls' rows: the controller button (BTN_*), else -1 */
} Row;

#define FIELD(f) offsetof(Settings, f)
#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

/* vk_present.c's shader numbers (Nightshade Station's list, without its 256-pixel NTSC ones) */
static const char *const SHADERS[] = {"Off",           "CRT Royale",       "CRT Royale Fast", "CRT Lottes",
                                      "CRT Pi",        "CRT Newpixie",     "CRT Hyllian NTSC", "CRT Mattias",
                                      "NTSC S-Video",  "NTSC Composite",   "LCD3x",           "zfast LCD",
                                      "Game Boy LCD"};
static const int SHADER_VALUES[] = {0, 13, 2, 9, 10, 14, 15, 16, 5, 6, 1, 12, 11};
static const char *const SHAPES[] = {"The game's (4:3)", "Stretched to 16:9"};
static const char *const UPSCALES[] = {"Off", "2x", "3x"};
static const int UPSCALE_VALUES[] = {1, 2, 3};
static const char *const FILTERS[] = {"Sharp pixels", "Smooth pixels (Scale2x)", "xBR"};
static const char *const SPEEDS[] = {"x2", "x3", "x4", "x6", "x8"};
static const char *const FF_MODES[] = {"While R2 is held", "R2 turns it on and off"};
static const char *const VOLUMES[] = {"Off", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"};
static const char *const UI_SOUNDS[] = {"Soft", "Wood", "Pop", "Chime", "Classic", "Off"};
static const char *const UI_VOLUMES[] = {"25%", "50%", "75%", "100%"};
static const char *const ORDERS[] = {"By title", "Last played first"};
/* the game's buttons (EasyRPG Player's names for them) */
static const char *const GAME_BUTTONS[] = {"Decision (A)",     "Cancel (B)",        "Cancel and menu (X)",
                                           "Shift (Y)",        "EasyRPG settings (Start)", "Reset to title (Select)",
                                           "Key 0 (L3)",       "Key 5 (R3)",        "L (debug)",
                                           "R (debug)",        "Nothing"};
static const int GAME_BUTTON_VALUES[] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_START, PAD_SELECT,
                                         PAD_L3, PAD_R3, PAD_L, PAD_R, PAD_NONE};

#define BUTTON_ROW(name, b)                                                                                        \
    {name, "The game's button this one presses.", K_CHOICE, 0, GAME_BUTTONS, N(GAME_BUTTONS), GAME_BUTTON_VALUES, \
     A_NONE, b}

static const Row PICTURE[] = {
    {"Shader", "The look of a CRT TV, an LCD or an analog signal over the picture. The CRT ones are the heaviest.",
     K_CHOICE, FIELD(shader), SHADERS, N(SHADERS), SHADER_VALUES, A_NONE, -1},
    {"Smooth scaling", "On: the picture is smoothed as it is enlarged. Off: sharp square pixels, as RPG Maker drew them.",
     K_TOGGLE, FIELD(smooth), NULL, 0, NULL, A_NONE, -1},
    {"Whole-number scaling", "Every pixel of the game the same size on screen, with black around the picture.",
     K_TOGGLE, FIELD(integer_scale), NULL, 0, NULL, A_NONE, -1},
    {"Shape", "The game's own shape, or wider to fill the screen.", K_CHOICE, FIELD(aspect), SHAPES, N(SHAPES), NULL,
     A_NONE, -1},
    {"Upscaling", "Enlarges the picture before it is scaled to the screen (without a shader).", K_CHOICE,
     FIELD(upscale), UPSCALES, N(UPSCALES), UPSCALE_VALUES, A_NONE, -1},
    {"Upscaling filter", "Sharp pixels keeps them square; Scale2x and xBR round off their edges.", K_CHOICE,
     FIELD(upscale_filter), FILTERS, N(FILTERS), NULL, A_NONE, -1},
};

static const Row PLAYING[] = {
    {"Fast-forward speed", "How much faster the game runs while it fast-forwards (R2).", K_CHOICE, FIELD(ff_speed),
     SPEEDS, N(SPEEDS), NULL, A_NONE, -1},
    {"Fast-forward with R2", "Hold R2 to fast-forward, or press it once to turn fast-forward on and again to stop.",
     K_CHOICE, FIELD(ff_toggle), FF_MODES, N(FF_MODES), NULL, A_NONE, -1},
    {"Left stick moves", "The left stick walks like the D-pad.", K_TOGGLE, FIELD(stick_dpad), NULL, 0, NULL, A_NONE,
     -1},
    {"Game volume", "The game's music and sounds.", K_CHOICE, FIELD(game_volume), VOLUMES, N(VOLUMES), NULL, A_NONE,
     -1},
    {"Show FPS", "The game's frames a second, in the corner.", K_TOGGLE, FIELD(show_fps), NULL, 0, NULL, A_NONE, -1},
};

static const Row CONTROLS[] = {
    BUTTON_ROW("Cross", BTN_CROSS),
    BUTTON_ROW("Circle", BTN_CIRCLE),
    BUTTON_ROW("Square", BTN_SQUARE),
    BUTTON_ROW("Triangle", BTN_TRIANGLE),
    BUTTON_ROW("L1", BTN_L1),
    BUTTON_ROW("R1", BTN_R1),
    BUTTON_ROW("L2", BTN_L2),
    BUTTON_ROW("L3", BTN_L3),
    BUTTON_ROW("R3", BTN_R3),
    BUTTON_ROW("OPTIONS", BTN_START),
    {"Default buttons", "Cross Decision, Circle Cancel, Triangle the menu, Square Shift, OPTIONS EasyRPG's settings.",
     K_ACTION, 0, NULL, 0, NULL, A_DEFAULT_BUTTONS, -1},
    {"The D-pad and the left stick move; R2 fast-forwards; the touchpad opens Nightshade Player's menu.", NULL, K_INFO,
     0, NULL, 0, NULL, A_NONE, -1},
};

static const Row LIBRARY_ROWS[] = {
    {"Order", "The library by title, or the games last played first. Triangle on the library switches it too.",
     K_CHOICE, FIELD(sort_mode), ORDERS, N(ORDERS), NULL, A_NONE, -1},
    {"Interface sounds", "The clicks of the library and the menus.", K_CHOICE, FIELD(ui_sound), UI_SOUNDS,
     N(UI_SOUNDS), NULL, A_NONE, -1},
    {"Interface volume", "How loud those clicks are.", K_CHOICE, FIELD(ui_volume), UI_VOLUMES, N(UI_VOLUMES), NULL,
     A_NONE, -1},
    {"Look for games again", "Reads " NP_DATA "/games again, for games copied while the app was open.", K_ACTION, 0,
     NULL, 0, NULL, A_RESCAN, -1},
};

static const Row SYSTEM[] = {
    {"Check for updates at start", "Asks github.com/Oliv3rmoon/Nightshade-Player for a newer release when the app "
                                   "opens, and installs it (checked against its SHA256SUMS) for the next start.",
     K_TOGGLE, FIELD(update_check), NULL, 0, NULL, A_NONE, -1},
    {"Check for updates now", "Asks GitHub now.", K_ACTION, 0, NULL, 0, NULL, A_CHECK_UPDATE, -1},
    {"Close Nightshade Player", "Closes the app. Save in the game first.", K_ACTION, 0, NULL, 0, NULL, A_CLOSE_APP, -1},
};

static const Row ABOUT[] = {
    {NP_TITLE " " NP_VERSION ", by Oliv3rmoon. Free software: GPL-3.0-or-later.", NULL, K_INFO, 0, NULL, 0, NULL,
     A_NONE, -1},
    {"Games run on EasyRPG Player (GPL-3.0-or-later) and liblcf (MIT), by the EasyRPG team.", NULL, K_INFO, 0, NULL,
     0, NULL, A_NONE, -1},
    {"Built on Nightshade Station: darkxex's SwanStationPS5 and BlackBearReloaded's PS5 boilerplate.", NULL, K_INFO,
     0, NULL, 0, NULL, A_NONE, -1},
    {"Games: " NP_DATA "/games, one folder (or .zip, .easyrpg) each.", NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
    {"RTP: " NP_DATA "/rtp/2000 and rtp/2003. Saves stay in each game's folder.", NULL, K_INFO, 0, NULL, 0, NULL,
     A_NONE, -1},
    {"Licences and credits: the licenses folder of the app, and THIRD_PARTY_NOTICES.md on GitHub.", NULL, K_INFO, 0,
     NULL, 0, NULL, A_NONE, -1},
    {"No games come with Nightshade Player: copy your own.", NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
};

typedef struct
{
    const char *name;
    int icon;
    const Row *rows;
    int count;
} Tab;

static const Tab TABS[] = {
    {"Picture", ICON_DEVICE_TV, PICTURE, N(PICTURE)},
    {"Playing", ICON_PLAYER_PLAY, PLAYING, N(PLAYING)},
    {"Controls", ICON_DEVICE_GAMEPAD_2, CONTROLS, N(CONTROLS)},
    {"Library", ICON_BOOKS, LIBRARY_ROWS, N(LIBRARY_ROWS)},
    {"System", ICON_SETTINGS, SYSTEM, N(SYSTEM)},
    {"About", ICON_INFO_CIRCLE, ABOUT, N(ABOUT)},
};
#define TAB_COUNT N(TABS)

static int tab, row, top;
static float bar_y = -1.0f;

void settings_opened(void)
{
    row = 0;
    top = 0;
    bar_y = -1.0f;
}

/* ---------------------------------------------------------------- values */

static int *int_field(const Row *r)
{
    if (r->button >= 0)
    {
        static int value;
        value = app.settings.button_map[r->button];
        return &value;
    }
    return (int *)((char *)&app.settings + r->field);
}

static int choice_index(const Row *r)
{
    const int v = *int_field(r);
    for (int i = 0; i < r->count; ++i)
        if ((r->values ? r->values[i] : i) == v)
            return i;
    return 0;
}

static void choice_set(const Row *r, int index)
{
    const int v = r->values ? r->values[index] : index;
    if (r->button >= 0)
        app.settings.button_map[r->button] = (int8_t)v;
    else
        *int_field(r) = v;
}

static bool selectable(const Row *r)
{
    return r->kind != K_INFO;
}

static void changed(const Row *r)
{
    if (r->field == FIELD(sort_mode) && r->kind == K_CHOICE && r->button < 0)
        app_rescan(); /* sorted again, the covers with it */
    app_apply_settings();
}

static void run_action(int action)
{
    switch (action)
    {
    case A_DEFAULT_BUTTONS:
        config_default_buttons(&app.settings, app.game >= 0 ? app.library.games[app.game].core : CORE_EASYRPG);
        app_toast("The buttons are back to their defaults.");
        break;
    case A_RESCAN:
    {
        app_rescan();
        char msg[64];
        snprintf(msg, sizeof(msg), app.library.count == 1 ? "%d game found" : "%d games found", app.library.count);
        app_toast(msg);
        break;
    }
    case A_CHECK_UPDATE:
        if (update_state() != UPDATE_CHECKING && update_state() != UPDATE_INSTALLING)
            update_check();
        break;
    case A_CLOSE_APP:
        app_save_settings();
        app.quit_requested = true;
        break;
    default: break;
    }
}

static const char *update_text(void)
{
    static char text[160];
    switch (update_state())
    {
    case UPDATE_CHECKING: return tr("Checking...");
    case UPDATE_NONE: return tr("This is the newest version");
    case UPDATE_AVAILABLE:
        snprintf(text, sizeof(text), tr("%s is available: Cross installs it"), update_version());
        return text;
    case UPDATE_INSTALLING: return tr("Installing...");
    case UPDATE_INSTALLED: return tr("Installed: restart to use it");
    case UPDATE_FAILED: return update_message();
    default: return "";
    }
}

/* ---------------------------------------------------------------- the screen */

static void leave(void)
{
    app_save_settings();
    app_apply_settings();
    sfx_play(SFX_BACK);
    if (app.settings_return == SCREEN_MENU && app.game >= 0)
        app.screen = SCREEN_MENU;
    else
    {
        app.screen = SCREEN_LIBRARY;
        shelf_open();
    }
}

void settings_screen(uint32_t pressed)
{
    const Tab *t = &TABS[tab];
    if (pressed & BIT(BTN_CIRCLE))
    {
        leave();
        if (app.screen == SCREEN_MENU)
            app_draw_game(105);
        else
            shelf_backdrop();
        return;
    }
    if (pressed & (BIT(BTN_L1) | BIT(BTN_R1)))
    {
        tab = (tab + (pressed & BIT(BTN_R1) ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
        t = &TABS[tab];
        row = top = 0;
        bar_y = -1.0f;
        sfx_play(SFX_CLICK);
    }
    /* the first row that can be chosen */
    while (row < t->count - 1 && !selectable(&t->rows[row]))
        ++row;
    if (pressed & (BIT(BTN_UP) | BIT(BTN_DOWN)))
    {
        const int step = pressed & BIT(BTN_UP) ? -1 : 1;
        int r = row;
        do
            r += step;
        while (r >= 0 && r < t->count && !selectable(&t->rows[r]));
        if (r >= 0 && r < t->count)
        {
            row = r;
            sfx_play(SFX_CLICK);
        }
    }
    const Row *r = &t->rows[row];
    if (selectable(r))
    {
        if (r->kind == K_CHOICE && (pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_CROSS))))
        {
            const int step = pressed & BIT(BTN_LEFT) ? -1 : 1;
            choice_set(r, (choice_index(r) + step + r->count) % r->count);
            changed(r);
            sfx_play(SFX_CLICK);
        }
        else if (r->kind == K_TOGGLE && (pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_CROSS))))
        {
            bool *b = (bool *)((char *)&app.settings + r->field);
            *b = !*b;
            changed(r);
            sfx_play(SFX_CLICK);
        }
        else if (r->kind == K_ACTION && (pressed & BIT(BTN_CROSS)))
        {
            if (r->action == A_CHECK_UPDATE && update_state() == UPDATE_AVAILABLE)
                update_install();
            else
                run_action(r->action);
            sfx_play(SFX_SELECT);
        }
    }

    /* behind it: the running game, darkened, or the library's backdrop */
    if (app.game >= 0)
    {
        app_draw_game(110);
        draw_rect(0, 0, (float)plat_width(), (float)plat_height(), 0x60000000u);
    }
    else
        shelf_backdrop();

    const float x = TH_MARGIN + 40, w = plat_width() - 2 * (TH_MARGIN + 40);
    draw_rrect(TH_MARGIN + 6, 60, 10, 34, 5, TH_FOCUS);
    text_draw(TH_MARGIN + 32, 54, 40, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Settings"));
    /* the tabs */
    float tx = x;
    const float ty = 138;
    draw_pad_glyph(GLYPH_L1, tx + 22, ty + 28, 26);
    tx += 64;
    for (int i = 0; i < TAB_COUNT; ++i)
    {
        const char *name = tr(TABS[i].name);
        const float tw = text_width(26, FONT_BOLD, name) + 96;
        const bool on = i == tab;
        draw_rrect(tx, ty, tw, 56, 28, on ? TH_FOCUS : TH_PILL_A(0xd0));
        icon_draw(TABS[i].icon, tx + 20, ty + 13, 30, on ? 0xffffffffu : TH_TEXT_DIM);
        text_draw(tx + 60, ty + 14, 26, FONT_BOLD, on ? 0xffffffffu : TH_TEXT_SOFT, ALIGN_LEFT, name);
        tx += tw + 14;
    }
    draw_pad_glyph(GLYPH_R1, tx + 30, ty + 28, 26);

    /* the rows */
    const float list_y = 236, row_h = 78;
    const int visible = 8;
    top = list_top_row(top, row, visible, t->count);
    draw_rrect(x - 20, list_y - 16, w + 40, visible * row_h + 32, TH_RADIUS, TH_CARD_A(0xe6));
    const float target = list_y + (row - top) * row_h;
    if (bar_y < 0)
        bar_y = target;
    anim_approach(&bar_y, target, app.dt, TH_SNAP);
    if (selectable(r))
    {
        draw_rrect(x, bar_y, w, row_h - 8, TH_RADIUS_SMALL, TH_ROW_SELECTED);
        draw_rect(x, bar_y + 16, 5, row_h - 40, TH_FOCUS);
    }
    for (int i = top; i < t->count && i < top + visible; ++i)
    {
        const Row *it = &t->rows[i];
        const float ry = list_y + (i - top) * row_h;
        const bool on = i == row && selectable(it);
        if (it->kind == K_INFO)
        {
            text_draw_fit(x + 28, ry + 22, 26, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w - 56, tr(it->label));
            continue;
        }
        float lx = x + 28;
        if (it->button >= 0)
        {
            /* the controller button's own picture before its name */
            static const int GLYPHS[MAP_BUTTONS] = {
                [BTN_CROSS] = GLYPH_CROSS, [BTN_SQUARE] = GLYPH_SQUARE, [BTN_SELECT] = GLYPH_SELECT,
                [BTN_START] = GLYPH_START, [BTN_UP] = GLYPH_UP,         [BTN_DOWN] = GLYPH_DOWN,
                [BTN_LEFT] = GLYPH_LEFT,   [BTN_RIGHT] = GLYPH_RIGHT,   [BTN_CIRCLE] = GLYPH_CIRCLE,
                [BTN_TRIANGLE] = GLYPH_TRIANGLE, [BTN_L1] = GLYPH_L1,  [BTN_R1] = GLYPH_R1,
                [BTN_L2] = GLYPH_L2,       [BTN_R2] = GLYPH_R2,         [BTN_L3] = GLYPH_L3,
                [BTN_R3] = GLYPH_R3};
            const float gw = pad_glyph_width((enum PadGlyph)GLYPHS[it->button], 30);
            draw_pad_glyph((enum PadGlyph)GLYPHS[it->button], lx + gw * 0.5f, ry + 38, 30);
            lx += gw + 20;
        }
        text_draw_fit(lx, ry + 21, 28, on ? FONT_BOLD : FONT_REGULAR, on ? TH_TEXT : TH_TEXT_SOFT, ALIGN_LEFT,
                      w * 0.5f, tr(it->label));
        const float right = x + w - 24;
        if (it->kind == K_CHOICE)
            draw_choice(right, ry + 12, 48, 24, on ? TH_PILL : TH_PILL_A(0x90), on ? TH_TEXT : TH_TEXT_SOFT,
                        tr(it->names[choice_index(it)]));
        else if (it->kind == K_TOGGLE)
        {
            const bool v = *(const bool *)((const char *)&app.settings + it->field);
            draw_switch(right - 92, ry + 17, 44, v ? 1.0f : 0.0f);
        }
        else if (it->kind == K_ACTION)
        {
            const char *value = it->action == A_CHECK_UPDATE ? update_text() : "";
            if (value[0])
                text_draw_fit(right, ry + 23, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, w * 0.45f, value);
            else
                icon_draw(ICON_CHEVRON_RIGHT, right - 34, ry + 20, 32, on ? TH_FOCUS : TH_TEXT_DIM);
        }
    }
    /* the chosen row's explanation */
    if (selectable(r) && r->help)
        text_draw_fit(x, list_y + visible * row_h + 40, 26, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w, tr(r->help));
    static const int glyphs[] = {GLYPH_CROSS, GLYPH_CIRCLE};
    static const char *const labels[] = {"Change", "Back"};
    app_draw_hints(glyphs, labels, 2, app.game >= 0 ? host_core_name() : NP_TITLE " " NP_VERSION);
}
