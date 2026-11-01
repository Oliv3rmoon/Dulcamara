/*
 * Dulcamara - the app's state and what the screens share (main.c owns them).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_APP_H
#define NP_APP_H

#include "library.h"
#include "nightshade.h"

enum Screen
{
    SCREEN_LIBRARY,
    SCREEN_GAME,
    SCREEN_MENU,     /* the in-game menu (the touchpad) */
    SCREEN_SETTINGS,
    SCREEN_KEYS,     /* Keys for this game: a Ren'Py game's buttons, from its menu or Settings > Ren'Py */
    SCREEN_COUNT
};

typedef struct
{
    Paths paths;
    Settings settings;
    Library library;
    int game;                 /* the running game's library index, -1 on the shelf */
    bool renpy_ran;           /* a Ren'Py game (8 or 7) has run since the app started: the next needs a restart */
    enum Screen screen;
    enum Screen settings_return;
    char storage_error[256];  /* the data folder can't be written */
    bool sandboxed;           /* /data can't be listed */
    char sandbox_reason[200];
    float dt;                 /* seconds since the last frame */
    float fps;                /* the game's frames a second */
    bool fast_forward;        /* running faster now */
    bool fast_latched;        /* turned on from the menu: stays on until turned off there */
    int8_t keys[MAP_BUTTONS]; /* the running Ren'Py game's keys: each RetroPad button's KEY_ACTIONS index (config.h) */
    PadState pads[NP_MAX_PADS];
    bool quit_requested;
} App;

extern App app;

void app_toast(const char *message);                 /* a notice for 2.5 s */
void app_toast_for(const char *message, float seconds);
/* A message in a box in the middle of the screen, until Cross is pressed. */
void app_message_box(const char *message);
/* The running game behind a menu; dim 255 = full brightness. */
void app_draw_game(uint8_t dim);
/* The controller that moves through the app: player 1's, else the first one connected. */
int app_nav_pad(void);
void app_rescan(void);
void app_start_game(int index);
void app_stop_game(void);
void app_save_settings(void);
void app_open_settings(enum Screen back_to);
/* The settings changed what the platform layer or the sounds use: applied now. */
void app_apply_settings(void);
/* The running Ren'Py game's keys (app.keys) to its core, which reads them at its next frame. */
void app_apply_keys(void);
/* The keys of the library's game index (each RetroPad button's KEY_ACTIONS index, config.h) from its keys file, and
 * into it. */
void app_load_keys(int index, int8_t keys[MAP_BUTTONS]);
void app_save_keys(int index, const int8_t keys[MAP_BUTTONS]);

/* The bottom hint bar: pairs of (PadGlyph, label), then an optional right-hand text. */
void app_draw_hints(const int *glyphs, const char *const *labels, int count, const char *right);

/* Screens: each handles its buttons and draws one frame. */
void shelf_open(void);
void shelf_screen(uint32_t pressed);
void shelf_library_changed(void);
void shelf_backdrop(void);
void menu_open(void);
void menu_screen(uint32_t pressed);
void settings_screen(uint32_t pressed);
void settings_opened(void);
/* Keys for this game, for the library's Ren'Py game index (the running one's from its menu); Circle goes back to
 * back_to (SCREEN_MENU or SCREEN_SETTINGS). */
void keys_open(int index, enum Screen back_to);
void keys_screen(uint32_t pressed);
/* The on-screen keyboard (ui/keyboard.c), each frame of the game screen, while the game wants text typed (a Ren'Py
 * game's name entry): pad is the controller, pressed its presses with the menus' repeats, options_tapped a short
 * press of Options let go (Done; held, Options is still Dulcamara's menu). True while the keyboard has the
 * controller: the game gets none of it meanwhile. Otherwise it takes out of game_pad what is still held from the
 * keyboard (a button, the stick, a finger on the touchpad), which the game gets once that has been let go. */
bool keyboard_frame(const PadState *pad, uint32_t pressed, bool options_tapped, PadState *game_pad);
/* Over the game: the keyboard, at the other end of the screen from the game's input; or, put away with Circle while
 * the game still wants text, a reminder that R3 brings it back. */
void keyboard_draw(void);

#endif
