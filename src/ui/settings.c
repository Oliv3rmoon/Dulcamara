/*
 * Dulcamara - Settings: tabs (L1 / R1) of rows (up / down), each changed with left / right or Cross.
 * Opened from a game, the game shows behind it, so a shader or a scale is seen as it is chosen. A Ren'Py game's
 * Keys for this game (from its menu, or any one's from the Ren'Py tab) is a page of the same rows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../community.h"
#include "../config.h"
#include "../core/cores.h"
#include "../core/host.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "../update.h"
#include "draw.h"
#include "icons.h"
#include "qr.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Kind
{
    K_CHOICE, /* an int field, its values named */
    K_TOGGLE, /* a bool field */
    K_ACTION, /* Cross does something */
    K_INFO,   /* a line of text */
    K_KEY,    /* Keys for this game: what a controller button presses in a Ren'Py game */
    K_CORE,   /* Controls: whose buttons the rows under it show; in a game, its own, not to be changed */
    K_GAME,   /* Ren'Py: one of the library's Ren'Py games, by title, whose Keys for this game Cross opens */
};

enum Action
{
    A_NONE,
    A_DEFAULT_BUTTONS,
    A_RESCAN,
    A_CHECK_UPDATE,
    A_CLOSE_APP,
    A_COMMUNITY,    /* nightly's card: its QR code (community.h) */
    A_DEFAULT_KEYS, /* Keys for this game: every button back to the core's own */
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
    int button;                /* the controls' and the keys' rows: the controller button (BTN_*), else -1 */
} Row;

#define FIELD(f) offsetof(Settings, f)
#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

/* vk_present.c's shader numbers (Mandrake's list, without its 256-pixel NTSC ones) */
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
                                           "R (debug)",        "L2",                "Nothing"};
static const int GAME_BUTTON_VALUES[] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_START, PAD_SELECT,
                                         PAD_L3, PAD_R3, PAD_L, PAD_R, PAD_L2, PAD_NONE};
/* the same buttons in Ren'Py's games: what each presses there (src/core/renpy/input.c) unless the game's own keys say
 * otherwise (Keys for this game) */
static const char *const RENPY_GAME_BUTTONS[] = {"Game menu and back (A)",  "Pick and go on (B)",
                                                 "Skip on and off (X)",     "Hide the text (Y)",
                                                 "Game menu (Start)",       "Free for a key (Select)",
                                                 "Cursor on and off (L3)",  "Free for a key (R3)",
                                                 "Roll back (L)",           "Roll forward (R)",
                                                 "Free for a key (L2)",     "Nothing"};
_Static_assert(N(RENPY_GAME_BUTTONS) == N(GAME_BUTTONS), "a Ren'Py name for each of the game's buttons");
/* whose buttons Controls shows (CORE_* order): Ren'Py 8's games and Ren'Py 7's and 6's each have their own */
static const char *const CORE_GAMES[] = {"RPG Maker", "Ren'Py 8", "Ren'Py 7 / 6"};
_Static_assert(N(CORE_GAMES) == CORE_COUNT, "a name for each core");

#define BUTTON_ROW(name, b)                                                                                        \
    {name, "Which of the game's buttons this one presses.", K_CHOICE, 0, GAME_BUTTONS, N(GAME_BUTTONS), GAME_BUTTON_VALUES, \
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
    {"Buttons of", "Whose buttons the rows below show and change: RPG Maker's games, Ren'Py 8's, or Ren'Py 7's and "
                   "6's. Each keeps its own.",
     K_CORE, 0, CORE_GAMES, N(CORE_GAMES), NULL, A_NONE, -1},
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
    {"Default buttons", "The buttons above back to their own: for RPG Maker, Cross Decision, Circle Cancel, "
                        "Triangle the menu; for Ren'Py, as its tab lists them.",
     K_ACTION, 0, NULL, 0, NULL, A_DEFAULT_BUTTONS, -1},
    {"The D-pad and the left stick move; R2 fast-forwards (in Ren'Py, it skips read text too); pressing the "
     "touchpad opens Dulcamara's menu. In Ren'Py the touchpad is the cursor, and Options held opens the menu.", NULL,
     K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
};

static const Row RENPY_ROWS[] = {
    {"Fast-forward skips unread text too", "R2 skips text not read yet as well as what was read, whatever the game's "
                                            "own Skip setting says. Off: the game's setting decides. From the next "
                                            "Ren'Py game started.",
     K_TOGGLE, FIELD(renpy_skip_unseen), NULL, 0, NULL, A_NONE, -1},
    {"Keys for a game", "Keys a game needs of its own (its inventory's, say), as its menu's Keys for this game has "
                        "them: Left and Right choose the game, Cross opens them.",
     K_GAME, 0, NULL, 0, NULL, A_NONE, -1},
    /* the buttons as a Ren'Py game has them unless Controls or its keys change them (core/cores.c, core/renpy/input.c),
     * all on one page: nothing under the last row that can be chosen is scrolled to */
    {"{Cross} picks and moves the story on; {Circle} and {Options} open the game's menu (save, load, preferences) and "
     "go back.",
     NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
    {"The D-pad and the left stick move between choices; {L1} and {R1} roll back and forward; {Square} hides the text "
     "window.",
     NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
    {"{Triangle} turns skipping on and off; {R2} fast-forwards (Ren'Py's own skip); {L2}, {R3} and Create are free for "
     "a game's keys.",
     NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
    {"The cursor: {L3} turns it on and off, the left stick moves it, {Cross} clicks where it points; a touch on the "
     "touchpad moves it, pressing it down clicks.",
     NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
    {"Dulcamara's menu: {Options} held for 0.6 s. A short press of {Options} is the game's menu.", NULL, K_INFO, 0,
     NULL, 0, NULL, A_NONE, -1},
    {"Saves: " NP_DATA "/saves/renpy. Ren'Py 8 games run on Ren'Py 8.5, Ren'Py 7 and 6 games on 7.8; a second one "
     "starts the app again.",
     NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
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
    {"Check for updates at start", "Asks github.com/Oliv3rmoon/Dulcamara for a newer release when the app "
                                   "opens, and installs it (checked against its SHA256SUMS) for the next start.",
     K_TOGGLE, FIELD(update_check), NULL, 0, NULL, A_NONE, -1},
    {"Check for updates now", "Asks GitHub now.", K_ACTION, 0, NULL, 0, NULL, A_CHECK_UPDATE, -1},
    {"Close Dulcamara", "Closes the app. Save in the game first.", K_ACTION, 0, NULL, 0, NULL, A_CLOSE_APP, -1},
};

static const Row ABOUT[] = {
    {"Join the community", "nightly, the Discord of Dulcamara and the other Nightshade apps: help, bug reports, new "
                           "versions. Cross shows its QR code for your phone.",
     K_ACTION, 0, NULL, 0, NULL, A_COMMUNITY, -1},
    {NP_TITLE " " NP_VERSION ", a Nightshade app by Oliv3rmoon. Free software: GPL-3.0-or-later.", NULL, K_INFO, 0, NULL, 0, NULL,
     A_NONE, -1},
    {"Games run on EasyRPG Player (GPL-3.0-or-later) and liblcf (MIT), by the EasyRPG team.", NULL, K_INFO, 0, NULL,
     0, NULL, A_NONE, -1},
    {"Ren'Py games run on Ren'Py 8.5 (MIT) by Tom Rothamel and its contributors, in Python 3.12 (PSF), with FFmpeg "
     "(LGPL-2.1); Ren'Py 7 and 6 games on Ren'Py 7.8, in Python 2.7 (PSF), with pygame_sdl2 (zlib).", NULL, K_INFO, 0,
     NULL, 0, NULL, A_NONE, -1},
    {"Built on Mandrake (formerly Nightshade Station): darkxex's SwanStationPS5 on BlackBearReloaded's boilerplate.", NULL, K_INFO,
     0, NULL, 0, NULL, A_NONE, -1},
    {"Games: " NP_DATA "/games, one folder (or .zip, .easyrpg) each. A Ren'Py game is its folder with game/ in it.",
     NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
    {"RTP: " NP_DATA "/rtp/2000 and rtp/2003. RPG Maker saves stay in each game's folder; Ren'Py's are in "
     NP_DATA "/saves/renpy.", NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
    {"Licences and credits: the licenses folder of the app, and THIRD_PARTY_NOTICES.md on GitHub.", NULL, K_INFO, 0,
     NULL, 0, NULL, A_NONE, -1},
    {"No games come with Dulcamara: copy your own.", NULL, K_INFO, 0, NULL, 0, NULL, A_NONE, -1},
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
    {"Ren'Py", ICON_FILE_TEXT, RENPY_ROWS, N(RENPY_ROWS)},
    {"Library", ICON_BOOKS, LIBRARY_ROWS, N(LIBRARY_ROWS)},
    {"System", ICON_SETTINGS, SYSTEM, N(SYSTEM)},
    {"About", ICON_INFO_CIRCLE, ABOUT, N(ABOUT)},
};
#define TAB_COUNT N(TABS)
#define ROWS_MAX 16 /* the most a tab has */

static int tab, row, top;
static float bar_y = -1.0f;
static bool community_open;  /* nightly's card is up, over the page */
static int controls_pick;    /* outside a game, whose buttons Controls shows (CORE_*) */
static char keys_pick[24];   /* the Ren'Py tab's game for Keys for a game: its id (the library's order changes) */

/* ---------------------------------------------------------------- the library's Ren'Py games */

/* by title, as the library sorts them */
static int by_title(const Game *a, const Game *b)
{
    const int c = str_icmp(a->title, b->title);
    return c ? c : strcmp(a->location, b->location);
}

/* The library's Ren'Py game after (step 1) or before (-1) the game from by title, round from the last to the first;
 * from -1, the first. from itself when it is the only one, -1 when there are none. */
static int renpy_game_step(int from, int step)
{
    int next = -1, wrap = -1;
    for (int i = 0; i < app.library.count; ++i)
    {
        const Game *g = &app.library.games[i];
        if (i == from || !core_is_renpy(g->core))
            continue;
        if (from >= 0 && by_title(g, &app.library.games[from]) * step > 0 &&
            (next < 0 || by_title(g, &app.library.games[next]) * step < 0))
            next = i;
        if (wrap < 0 || by_title(g, &app.library.games[wrap]) * step < 0)
            wrap = i;
    }
    return next >= 0 ? next : wrap >= 0 ? wrap : from;
}

/* The game Keys for a game shows: the one chosen, else the first by title; -1 when the library has no Ren'Py game. */
static int keys_pick_game(void)
{
    for (int i = 0; i < app.library.count; ++i)
        if (core_is_renpy(app.library.games[i].core) && !strcmp(app.library.games[i].id, keys_pick))
            return i;
    const int first = renpy_game_step(-1, 1);
    if (first >= 0)
        str_copy(keys_pick, sizeof(keys_pick), app.library.games[first].id);
    return first;
}

void settings_opened(void)
{
    row = 0;
    top = 0;
    bar_y = -1.0f;
    community_open = false;
    /* Controls' buttons and Ren'Py's keys start at the game the app is at: the running one, else the shelf's */
    const int at = app.game >= 0 ? app.game : app.settings.last_game;
    if (at >= 0 && at < app.library.count)
    {
        controls_pick = app.library.games[at].core;
        if (core_is_renpy(app.library.games[at].core))
            str_copy(keys_pick, sizeof(keys_pick), app.library.games[at].id);
    }
}

/* ---------------------------------------------------------------- values */

/* The core whose buttons the Controls page shows: the running game's, else the one chosen at its top. */
static int controls_core(void)
{
    return app.game >= 0 ? app.library.games[app.game].core : controls_pick;
}

static int *int_field(const Row *r)
{
    static int value;
    if (r->button >= 0)
    {
        value = app.settings.button_map[controls_core()][r->button];
        return &value;
    }
    if (r->kind == K_CORE)
    {
        value = controls_core();
        return &value;
    }
    return (int *)((char *)&app.settings + r->field);
}

/* What a choice's values are called: the game's buttons by the core whose they are. */
static const char *const *choice_names(const Row *r)
{
    return r->button >= 0 && core_is_renpy(controls_core()) ? RENPY_GAME_BUTTONS : r->names;
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
        app.settings.button_map[controls_core()][r->button] = (int8_t)v;
    else if (r->kind == K_CORE)
        controls_pick = v;
    else
        *int_field(r) = v;
}

static bool selectable(const Row *r)
{
    return r->kind != K_INFO && !(r->kind == K_CORE && app.game >= 0);
}

/* The rows a tab shows now: Keys for a game only when the library has a Ren'Py game. */
static int tab_rows(const Tab *t, const Row **rows)
{
    int n = 0;
    for (int i = 0; i < t->count && n < ROWS_MAX; ++i)
        if (t->rows[i].kind != K_GAME || keys_pick_game() >= 0)
            rows[n++] = &t->rows[i];
    return n;
}

/* s cut to max_width with "..." as text_draw_fit cuts it, for a value drawn in a choice's pill */
static const char *fit_text(const char *s, float size, float max_width, char *out, size_t out_size)
{
    str_copy(out, out_size, s);
    for (size_t len = strlen(out); len > 0 && text_width(size, FONT_REGULAR, out) > max_width;)
    {
        do
            --len;
        while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80); /* stay on a UTF-8 boundary */
        snprintf(out, out_size, "%.*s...", (int)len, s);
    }
    return out;
}

/* A controller button's (BTN_*) picture at the left of a row at y, x; returns its width. */
static float draw_button(int button, float x, float y)
{
    static const int GLYPHS[MAP_BUTTONS] = {
        [BTN_CROSS] = GLYPH_CROSS,       [BTN_SQUARE] = GLYPH_SQUARE, [BTN_SELECT] = GLYPH_SELECT,
        [BTN_START] = GLYPH_START,       [BTN_UP] = GLYPH_UP,         [BTN_DOWN] = GLYPH_DOWN,
        [BTN_LEFT] = GLYPH_LEFT,         [BTN_RIGHT] = GLYPH_RIGHT,   [BTN_CIRCLE] = GLYPH_CIRCLE,
        [BTN_TRIANGLE] = GLYPH_TRIANGLE, [BTN_L1] = GLYPH_L1,         [BTN_R1] = GLYPH_R1,
        [BTN_L2] = GLYPH_L2,             [BTN_R2] = GLYPH_R2,         [BTN_L3] = GLYPH_L3,
        [BTN_R3] = GLYPH_R3};
    const float gw = pad_glyph_width((enum PadGlyph)GLYPHS[button], 30);
    draw_pad_glyph((enum PadGlyph)GLYPHS[button], x + gw * 0.5f, y + 38, 30);
    return gw;
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
        config_default_buttons(&app.settings, controls_core());
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
    case A_COMMUNITY: community_open = true; break;
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

/* ---------------------------------------------------------------- nightly's card */

/* Any text as a QR code on a white card (Mandrake's draw_url_qr), made on the console (qr.h): dark modules in the
 * near-black of the theme, a 2-module white margin, scaled up by whole pixels so that a phone reads it, and centred in
 * the size x size square at x, y. */
static void draw_url_qr(const char *text, float x, float y, float size)
{
    static char made_for[128];
    static PlatTexture *tex;
    static int modules;
    if (strcmp(made_for, text) != 0)
    {
        str_copy(made_for, sizeof(made_for), text);
        plat_texture_free(tex);
        tex = NULL;
        static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], temp[qrcodegen_BUFFER_LEN_MAX];
        if (qr_encode(text, qr, temp))
        {
            const int n = qrcodegen_getSize(qr), side = n + 4;
            uint8_t *px = malloc((size_t)side * side * 4);
            if (px)
            {
                for (int j = 0; j < side; ++j)
                    for (int i = 0; i < side; ++i)
                    {
                        const bool dark = qrcodegen_getModule(qr, i - 2, j - 2);
                        uint8_t *p = &px[(j * side + i) * 4];
                        p[0] = dark ? 15 : 255, p[1] = dark ? 19 : 255, p[2] = dark ? 48 : 255, p[3] = 255;
                    }
                tex = plat_texture_create(px, side, side, false);
                modules = side;
                free(px);
            }
        }
    }
    if (!tex)
        return;
    const int scale = (int)(size / modules);
    const float side = (float)(scale * modules), pad = 14;
    x += (size - side) * 0.5f;
    y += (size - side) * 0.5f;
    draw_rrect(x - pad, y - pad, side + 2 * pad, side + 2 * pad, TH_RADIUS_SMALL, 0xffffffffu);
    plat_draw_texture(tex, x, y, side, side, 0xffffffffu, false);
}

/* About > Join the community, Cross: the invitation's QR code for a phone, with the server's name and its short link.
 * Cross or Circle puts it away. */
static void draw_community(void)
{
    const float w = 660, h = 630, x = (plat_width() - w) * 0.5f, y = (plat_height() - h) * 0.5f - 24;
    draw_rect(0, 0, (float)plat_width(), (float)plat_height(), 0xa0000000u);
    draw_rrect(x, y, w, h, TH_RADIUS, TH_CARD);
    draw_rect(x, y, w, 4, TH_FOCUS);
    text_draw(x + w * 0.5f, y + 40, 36, FONT_BOLD, TH_TEXT, ALIGN_CENTER, tr("Join the community"));
    const float qs = 360, qy = y + 122;
    draw_url_qr(COMMUNITY_URL, x + (w - qs) * 0.5f, qy, qs);
    char line[160];
    snprintf(line, sizeof(line), tr("Scan to join %s on Discord"), COMMUNITY_NAME);
    text_draw_fit(x + w * 0.5f, qy + qs + 34, 28, FONT_BOLD, TH_TEXT, ALIGN_CENTER, w - 80, line);
    text_draw(x + w * 0.5f, qy + qs + 82, 26, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER, COMMUNITY_LINK);
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
    if (community_open)
    {
        /* nightly's card takes the buttons: Cross or Circle puts it away, and the page stays */
        if (pressed & (BIT(BTN_CROSS) | BIT(BTN_CIRCLE)))
        {
            community_open = false;
            sfx_play(SFX_BACK);
        }
        pressed = 0;
    }
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
    const Row *rows[ROWS_MAX];
    const int count = tab_rows(t, rows);
    if (row >= count)
        row = count - 1;
    /* the first row that can be chosen */
    while (row < count - 1 && !selectable(rows[row]))
        ++row;
    if (pressed & (BIT(BTN_UP) | BIT(BTN_DOWN)))
    {
        const int step = pressed & BIT(BTN_UP) ? -1 : 1;
        int r = row;
        do
            r += step;
        while (r >= 0 && r < count && !selectable(rows[r]));
        if (r >= 0 && r < count)
        {
            row = r;
            sfx_play(SFX_CLICK);
        }
    }
    const Row *r = rows[row];
    if (selectable(r))
    {
        if ((r->kind == K_CHOICE || r->kind == K_CORE) && (pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_CROSS))))
        {
            const int step = pressed & BIT(BTN_LEFT) ? -1 : 1;
            choice_set(r, (choice_index(r) + step + r->count) % r->count);
            if (r->kind == K_CHOICE)
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
        else if (r->kind == K_GAME && (pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT))))
        {
            const int g = renpy_game_step(keys_pick_game(), pressed & BIT(BTN_LEFT) ? -1 : 1);
            if (g >= 0)
                str_copy(keys_pick, sizeof(keys_pick), app.library.games[g].id);
            sfx_play(SFX_CLICK);
        }
        else if (r->kind == K_GAME && (pressed & BIT(BTN_CROSS)))
        {
            /* the game's keys, as its menu has them, and Circle back here */
            sfx_play(SFX_SELECT);
            keys_open(keys_pick_game(), SCREEN_SETTINGS);
            keys_screen(0); /* drawn now */
            return;
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

    /* the rows: eight a page; About, which has nothing to scroll to, all of its own on one */
    const int visible = t->rows == ABOUT ? t->count : 8;
    const float list_y = 236, row_h = 8 * 78.0f / visible;
    top = list_top_row(top, row, visible, count);
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
    for (int i = top; i < count && i < top + visible; ++i)
    {
        const Row *it = rows[i];
        const float ry = list_y + (i - top) * row_h;
        const bool on = i == row && selectable(it);
        if (it->kind == K_INFO)
        {
            text_draw_fit(x + 28, ry + 22, 26, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w - 56, tr(it->label));
            continue;
        }
        float lx = x + 28;
        if (it->button >= 0)
            lx += draw_button(it->button, lx, ry) + 20; /* the controller button's own picture before its name */
        text_draw_fit(lx, ry + 21, 28, on ? FONT_BOLD : FONT_REGULAR, on ? TH_TEXT : TH_TEXT_SOFT, ALIGN_LEFT,
                      w * 0.5f, tr(it->label));
        const float right = x + w - 24;
        if (it->kind == K_CORE && !selectable(it))
            /* in a game: its own core's buttons, which the page does not change */
            text_draw_fit(right, ry + 23, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, w * 0.45f,
                          tr(it->names[choice_index(it)]));
        else if (it->kind == K_CHOICE || it->kind == K_CORE)
            draw_choice(right, ry + 12, 48, 24, on ? TH_PILL : TH_PILL_A(0x90), on ? TH_TEXT : TH_TEXT_SOFT,
                        tr(choice_names(it)[choice_index(it)]));
        else if (it->kind == K_GAME)
        {
            /* the game's title, cut to leave its name room: the pill is the title and 98 pixels */
            char title[sizeof(app.library.games[0].title) + 4];
            const int g = keys_pick_game();
            draw_choice(right, ry + 12, 48, 24, on ? TH_PILL : TH_PILL_A(0x90), on ? TH_TEXT : TH_TEXT_SOFT,
                        fit_text(g >= 0 ? app.library.games[g].title : "", 24, w * 0.45f - 98, title, sizeof(title)));
        }
        else if (it->kind == K_TOGGLE)
        {
            const bool v = *(const bool *)((const char *)&app.settings + it->field);
            draw_switch(right - 92, ry + 17, 44, v ? 1.0f : 0.0f);
        }
        else if (it->kind == K_ACTION)
        {
            char community[64];
            snprintf(community, sizeof(community), "%s  \xC2\xB7  %s", COMMUNITY_NAME, COMMUNITY_LINK);
            const char *value = it->action == A_CHECK_UPDATE ? update_text()
                                : it->action == A_COMMUNITY  ? community
                                                             : "";
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
    if (community_open)
        draw_community();
}

/* ---------------------------------------------------------------- Keys for this game */

/* Keys for this game, from a Ren'Py game's menu (Dulcamara's) or for any of the library's Ren'Py games from Settings >
 * Ren'Py: what each of the controller's buttons presses in it. A button reaches the core as the RetroPad button
 * Settings > Controls gives the game's core, and that button's renpy_key_* option (its action in the game's keys,
 * config.h) is the core's own, nothing, a key or a click. The running game's keys are app.keys, and a change goes to
 * its core at once: it reads its options at its next frame. Another game's come from its file, and its core has them
 * when it starts. Leaving the page keeps the game's keys in their file (app_save_keys). */
#define KEY_ROW(name, b) {name, NULL, K_KEY, 0, NULL, 0, NULL, A_NONE, b}

static const Row KEY_ROWS[] = {
    KEY_ROW("Cross", BTN_CROSS),       KEY_ROW("Circle", BTN_CIRCLE), KEY_ROW("Square", BTN_SQUARE),
    KEY_ROW("Triangle", BTN_TRIANGLE), KEY_ROW("L1", BTN_L1),         KEY_ROW("R1", BTN_R1),
    KEY_ROW("L2", BTN_L2),             KEY_ROW("L3", BTN_L3),         KEY_ROW("R3", BTN_R3),
    KEY_ROW("Options", BTN_START),     KEY_ROW("Create", BTN_SELECT), KEY_ROW("Up", BTN_UP),
    KEY_ROW("Down", BTN_DOWN),         KEY_ROW("Left", BTN_LEFT),     KEY_ROW("Right", BTN_RIGHT),
    {"Default keys", "Every button back to Ren'Py's own, the one each shows as (default).", K_ACTION, 0, NULL, 0, NULL,
     A_DEFAULT_KEYS, -1},
};

static int key_row, key_top;
static float key_bar_y = -1.0f;
static bool keys_changed;             /* since the page opened: the game's file is written as it closes */
static int key_game = -1;             /* the library's game the page is for */
static int8_t key_copy[MAP_BUTTONS];  /* the keys of a game that is not running, from its file */
static int8_t *page_keys = key_copy;  /* the page's: app.keys for the running game, else key_copy */
static enum Screen key_back;          /* where Circle goes: the game's menu, or Settings */

void keys_open(int index, enum Screen back_to)
{
    key_game = index;
    key_back = back_to;
    if (index == app.game)
        page_keys = app.keys; /* the ones its core has */
    else
    {
        app_load_keys(index, key_copy);
        page_keys = key_copy;
    }
    app.screen = SCREEN_KEYS;
    key_row = key_top = 0;
    key_bar_y = -1.0f;
    keys_changed = false;
}

/* The page's game is the running one: its keys go to its core as they change. */
static bool key_game_runs(void)
{
    return key_game >= 0 && key_game == app.game;
}

/* The RetroPad button a row's controller button gives the page's game (Settings > Controls, its core's), or -1 when
 * it gives it none that a key can go on. */
static int key_pad(const Row *r)
{
    const int pad = key_game >= 0 && key_game < app.library.count
                        ? app.settings.button_map[app.library.games[key_game].core][r->button]
                        : PAD_NONE;
    return config_key_option(pad) ? pad : -1;
}

/* The action after (step 1) or before (-1) this one, past the one the core's own already is ("default"). */
static int next_key(int pad, int action, int step)
{
    const int own = config_key_default(pad);
    do
        action = (action + step + KEY_ACTION_COUNT) % KEY_ACTION_COUNT;
    while (action == own);
    return action;
}

/* L1 and R1: the first action of the kind before or after this one (the core's own and the mouse, the keys, the
 * letters, the digits, the F keys), or of its own kind when it is not that kind's first. */
static int jump_key(int pad, int action, int step)
{
    const int from = action;
    for (;;)
    {
        do
            action = (action + step + KEY_ACTION_COUNT) % KEY_ACTION_COUNT;
        while (action && KEY_ACTIONS[action].kind == KEY_ACTIONS[action - 1].kind);
        const int to = action == config_key_default(pad) ? next_key(pad, action, 1) : action;
        if (to != from)
            return to;
    }
}

/* What a row shows: its button's action, or the core's own, marked (default). */
static const char *key_text(const Row *r, char *out, size_t size)
{
    const int pad = key_pad(r);
    if (pad < 0)
        return tr("Nothing (Settings > Controls)");
    if (page_keys[pad])
        return tr(KEY_ACTIONS[page_keys[pad]].name);
    snprintf(out, size, tr("%s (default)"), tr(KEY_ACTIONS[config_key_default(pad)].name));
    return out;
}

static const char *key_help(const Row *r)
{
    const int pad = key_pad(r);
    if (pad < 0)
        return tr("Settings > Controls gives this button nothing in Ren'Py's games, so it presses no key.");
    const int action = page_keys[pad] ? page_keys[pad] : config_key_default(pad);
    if (!strcmp(KEY_ACTIONS[action].value, "cursor"))
        return tr("Turns the cursor on and off: the left stick moves it, and {Cross} clicks whatever it presses.");
    return tr("What it presses in this game: Ren'Py's own, one of the game's keys, a click, or nothing.");
}

void keys_screen(uint32_t pressed)
{
    const int count = N(KEY_ROWS);
    if (pressed & BIT(BTN_CIRCLE))
    {
        if (keys_changed)
            app_save_keys(key_game, page_keys);
        sfx_play(SFX_BACK);
        app.screen = key_back;
        if (key_back == SCREEN_SETTINGS)
            settings_screen(0); /* drawn now, as it was */
        else
            app_draw_game(105);
        return;
    }
    if (pressed & (BIT(BTN_UP) | BIT(BTN_DOWN)))
    {
        const int to = key_row + (pressed & BIT(BTN_UP) ? -1 : 1);
        if (to >= 0 && to < count)
        {
            key_row = to;
            sfx_play(SFX_CLICK);
        }
    }
    const Row *r = &KEY_ROWS[key_row];
    const int pad = r->kind == K_KEY ? key_pad(r) : -1;
    const uint32_t change = BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_CROSS), jump = BIT(BTN_L1) | BIT(BTN_R1);
    if (pad >= 0 && (pressed & (change | jump)))
    {
        const int step = pressed & (BIT(BTN_LEFT) | BIT(BTN_L1)) ? -1 : 1;
        const int action =
            pressed & change ? next_key(pad, page_keys[pad], step) : jump_key(pad, page_keys[pad], step);
        page_keys[pad] = (int8_t)action;
        if (key_game_runs())
            app_apply_keys();
        keys_changed = true;
        sfx_play(SFX_CLICK);
    }
    else if (r->kind == K_ACTION && r->action == A_DEFAULT_KEYS && (pressed & BIT(BTN_CROSS)))
    {
        memset(page_keys, 0, MAP_BUTTONS);
        if (key_game_runs())
            app_apply_keys();
        keys_changed = true;
        app_toast("Every button presses Ren'Py's own again.");
        sfx_play(SFX_SELECT);
    }

    /* behind it: the running game, darkened, as under Settings, or the library's backdrop */
    if (app.game >= 0)
    {
        app_draw_game(110);
        draw_rect(0, 0, (float)plat_width(), (float)plat_height(), 0x60000000u);
    }
    else
        shelf_backdrop();
    const float x = TH_MARGIN + 40, w = plat_width() - 2 * (TH_MARGIN + 40);
    draw_rrect(TH_MARGIN + 6, 60, 10, 34, 5, TH_FOCUS);
    text_draw(TH_MARGIN + 32, 54, 40, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Keys for this game"));
    /* the game, where Settings has its tabs */
    const Game *g = key_game >= 0 && key_game < app.library.count ? &app.library.games[key_game] : NULL;
    icon_draw(ICON_KEYBOARD, x, 148, 36, TH_FOCUS);
    text_draw_fit(x + 52, 150, 30, FONT_BOLD, TH_TEXT_SOFT, ALIGN_LEFT, w - 52, g ? g->title : "");

    /* the rows, eight a page, and how far down the page is on the card's right */
    const int visible = 8;
    const float list_y = 236, row_h = 78;
    key_top = list_top_row(key_top, key_row, visible, count);
    draw_rrect(x - 20, list_y - 16, w + 40, visible * row_h + 32, TH_RADIUS, TH_CARD_A(0xe6));
    if (count > visible)
    {
        const float track = visible * row_h - 8, thumb = track * visible / count;
        draw_rrect(x + w + 8, list_y, 4, track, 2, TH_DIVIDER);
        draw_rrect(x + w + 8, list_y + (track - thumb) * key_top / (count - visible), 4, thumb, 2, TH_TEXT_DIM);
    }
    const float target = list_y + (key_row - key_top) * row_h;
    if (key_bar_y < 0)
        key_bar_y = target;
    anim_approach(&key_bar_y, target, app.dt, TH_SNAP);
    draw_rrect(x, key_bar_y, w, row_h - 8, TH_RADIUS_SMALL, TH_ROW_SELECTED);
    draw_rect(x, key_bar_y + 16, 5, row_h - 40, TH_FOCUS);
    for (int i = key_top; i < count && i < key_top + visible; ++i)
    {
        const Row *it = &KEY_ROWS[i];
        const float ry = list_y + (i - key_top) * row_h, right = x + w - 24;
        const bool on = i == key_row;
        float lx = x + 28;
        if (it->button >= 0)
            lx += draw_button(it->button, lx, ry) + 20;
        text_draw_fit(lx, ry + 21, 28, on ? FONT_BOLD : FONT_REGULAR, on ? TH_TEXT : TH_TEXT_SOFT, ALIGN_LEFT,
                      w * 0.5f, tr(it->label));
        if (it->kind == K_ACTION)
        {
            icon_draw(ICON_CHEVRON_RIGHT, right - 34, ry + 20, 32, on ? TH_FOCUS : TH_TEXT_DIM);
            continue;
        }
        char text[96];
        const int it_pad = key_pad(it);
        const char *value = key_text(it, text, sizeof(text));
        if (it_pad < 0)
            text_draw_fit(right, ry + 23, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, w * 0.45f, value);
        else
            draw_choice(right, ry + 12, 48, 24, on ? TH_PILL : TH_PILL_A(0x90),
                        on || page_keys[it_pad] ? TH_TEXT : TH_TEXT_SOFT, value);
    }
    /* the chosen row's explanation, and the page's */
    const float hy = list_y + visible * row_h + 40;
    text_draw_fit(x, hy, 26, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w, r->kind == K_KEY ? key_help(r) : tr(r->help));
    const char *page = key_game_runs() ? tr("A change applies at once, and Dulcamara keeps this game's keys for it.")
                                       : tr("Dulcamara keeps this game's keys for it, and the game has them when it "
                                            "starts.");
    text_draw_fit(x, hy + 40, 24, FONT_REGULAR, TH_HINT, ALIGN_LEFT, w, page);
    static const int glyphs[] = {GLYPH_CROSS, GLYPH_CIRCLE}, jumps[] = {GLYPH_L1, GLYPH_R1};
    static const char *const labels[] = {"Change", "Back"};
    app_draw_hints(glyphs, labels, 2, NULL);
    const char *jump_label = "Keys, letters, digits, F keys";
    draw_hint_combo(plat_width() - TH_MARGIN - hint_combo_width(jumps, 2, jump_label, 26), TH_HINT_Y, jumps, 2, '/',
                    jump_label, 26, TH_HINT);
}
