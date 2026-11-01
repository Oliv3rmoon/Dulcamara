/*
 * Dulcamara - the in-game menu (the touchpad): over the game, darkened.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../core/cores.h"
#include "../core/host.h"
#include "../covers.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum
{
    ITEM_RESUME,
    ITEM_FAST,
    ITEM_RESET,
    ITEM_KEYS, /* Ren'Py's games: what each button presses (ui/settings.c) */
    ITEM_SETTINGS,
    ITEM_QUIT,
    ITEM_COUNT
};

static int selected; /* a place in the game's items */
static float slide;  /* 0..1, the panel coming in */
static float bar_y;  /* the highlight, animated */
static bool confirm_reset;

static bool playing_renpy(void)
{
    return app.game >= 0 && core_is_renpy(app.library.games[app.game].core);
}

/* The game's items, in order: all of them, Keys for this game in Ren'Py's games only. */
static int menu_items(int *items)
{
    int n = 0;
    for (int i = 0; i < ITEM_COUNT; ++i)
        if (i != ITEM_KEYS || playing_renpy())
            items[n++] = i;
    return n;
}

void menu_open(void)
{
    app.screen = SCREEN_MENU;
    selected = 0; /* Resume */
    slide = 0.0f;
    bar_y = -1.0f;
    confirm_reset = false;
    sfx_play(SFX_SELECT);
}

static void resume(void)
{
    app.screen = SCREEN_GAME;
    sfx_play(SFX_BACK);
}

void menu_screen(uint32_t pressed)
{
    int items[ITEM_COUNT];
    const int count = menu_items(items);
    selected = selected < count ? selected : count - 1;
    if (pressed & (BIT(BTN_CIRCLE) | BIT(BTN_MENU)))
    {
        resume();
        app_draw_game(255);
        return;
    }
    if (pressed & BIT(BTN_UP))
    {
        selected = (selected + count - 1) % count;
        confirm_reset = false;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_DOWN))
    {
        selected = (selected + 1) % count;
        confirm_reset = false;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_CROSS))
    {
        switch (items[selected])
        {
        case ITEM_RESUME: resume(); break;
        case ITEM_FAST:
            /* from the menu it stays on until it is turned off here, R2 or not */
            app.fast_latched = !app.fast_forward;
            app.fast_forward = app.fast_latched;
            sfx_play(SFX_CLICK);
            resume();
            break;
        case ITEM_RESET:
            if (playing_renpy())
            {
                /* Ren'Py's core does not reset: its own menu goes back to the main menu */
                app_toast("In Ren'Py, the game's menu (Circle) goes back to its main menu.");
                resume();
            }
            else if (!confirm_reset)
            {
                confirm_reset = true;
                sfx_play(SFX_CLICK);
            }
            else
            {
                host_reset();
                app_toast("Back to the game's title screen.");
                resume();
            }
            break;
        case ITEM_KEYS:
            sfx_play(SFX_SELECT);
            keys_open();
            keys_screen(0); /* drawn now, over the game */
            return;
        case ITEM_SETTINGS:
            sfx_play(SFX_SELECT);
            app_open_settings(SCREEN_MENU);
            return;
        case ITEM_QUIT:
            sfx_play(SFX_BACK);
            app_stop_game();
            return;
        }
    }
    if (app.screen != SCREEN_MENU)
    {
        app_draw_game(255);
        return;
    }

    anim_approach(&slide, 1.0f, app.dt, 12.0f);
    app_draw_game((uint8_t)(255 - 150 * slide));
    const float w = 640, x = -w * (1.0f - slide) * 0.4f, h = (float)plat_height();
    draw_rect(x, 0, w, h, argb_alpha(TH_CARD_A(0xf0), slide));
    draw_rect(x + w - 3, 0, 3, h, argb_alpha(TH_FOCUS, slide * 0.6f));

    const Game *g = app.game >= 0 ? &app.library.games[app.game] : NULL;
    float y = 92;
    PlatTexture *cover = app.game >= 0 ? covers_get(app.game) : NULL;
    if (cover)
    {
        plat_draw_texture(cover, x + 64, y, 240, 180, argb_alpha(0xffffffffu, slide), true);
        draw_rrect_outline(x + 60, y - 4, 248, 188, 8, 2, argb_alpha(TH_DIVIDER, slide));
        y += 212;
    }
    text_draw_fit(x + 64, y, 36, FONT_BOLD, argb_alpha(TH_TEXT, slide), ALIGN_LEFT, w - 128, g ? g->title : "");
    text_draw(x + 64, y + 50, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, slide), ALIGN_LEFT, host_core_name());
    y += 120;

    static const int ICONS[ITEM_COUNT] = {ICON_PLAYER_PLAY, ICON_PLAYER_TRACK_NEXT, ICON_ROTATE, ICON_KEYBOARD,
                                          ICON_SETTINGS, ICON_DOOR_EXIT};
    /* six items (Ren'Py's) a little closer, so that the last stays clear of the lines at the foot */
    const float row = count > 5 ? 78 : 84, inset = (row - 48) * 0.5f;
    const float target = y + selected * row;
    if (bar_y < 0)
        bar_y = target;
    anim_approach(&bar_y, target, app.dt, TH_SNAP);
    draw_rrect(x + 40, bar_y, w - 80, row - 10, TH_RADIUS_SMALL, argb_alpha(TH_ROW_SELECTED, slide));
    draw_rect(x + 40, bar_y + 14, 5, row - 38, argb_alpha(TH_FOCUS, slide));
    for (int i = 0; i < count; ++i)
    {
        const int item = items[i];
        const bool on = i == selected;
        char label[96];
        switch (item)
        {
        case ITEM_RESUME: str_copy(label, sizeof(label), tr("Resume")); break;
        case ITEM_FAST:
            str_copy(label, sizeof(label), app.fast_forward ? tr("Fast-forward: on") : tr("Fast-forward: off"));
            break;
        case ITEM_RESET:
            str_copy(label, sizeof(label),
                     playing_renpy()       ? tr("Back to the title screen: the game's menu")
                     : confirm_reset && on ? tr("Press again: unsaved progress is lost")
                                           : tr("Back to the title screen"));
            break;
        case ITEM_KEYS: str_copy(label, sizeof(label), tr("Keys for this game")); break;
        case ITEM_SETTINGS: str_copy(label, sizeof(label), tr("Settings")); break;
        case ITEM_QUIT: str_copy(label, sizeof(label), tr("Quit to the library")); break;
        }
        const float ry = y + i * row;
        const uint32_t color = on ? TH_TEXT : TH_TEXT_SOFT;
        icon_draw(ICONS[item], x + 72, ry + inset, 36, argb_alpha(on ? TH_FOCUS : TH_TEXT_DIM, slide));
        text_draw_fit(x + 130, ry + inset + 2, 30, on ? FONT_BOLD : FONT_REGULAR,
                      argb_alpha(item == ITEM_RESET && confirm_reset && on ? TH_DANGER : color, slide), ALIGN_LEFT,
                      w - 200, label);
    }
    text_draw_fit(x + 64, h - 176, 22, FONT_REGULAR, argb_alpha(TH_HINT, slide), ALIGN_LEFT, w - 128,
                  tr("Save in the game's own menu."));
    text_draw_fit(x + 64, h - 146, 22, FONT_REGULAR, argb_alpha(TH_HINT, slide), ALIGN_LEFT, w - 128,
                  tr("Its saves are kept in the game's folder."));
    const float hy = h - 82;
    float hx = x + 64;
    hx += draw_hint(hx, hy, GLYPH_CROSS, tr("Select"), 26, argb_alpha(TH_HINT, slide));
    draw_hint(hx, hy, GLYPH_CIRCLE, tr("Resume"), 26, argb_alpha(TH_HINT, slide));
}
