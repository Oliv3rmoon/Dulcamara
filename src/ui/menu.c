/*
 * Dulcamara - the in-game menu (the touchpad): over the game, darkened.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
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
    ITEM_SETTINGS,
    ITEM_QUIT,
    ITEM_COUNT
};

static int selected;
static float slide;  /* 0..1, the panel coming in */
static float bar_y;  /* the highlight, animated */
static bool confirm_reset;

void menu_open(void)
{
    app.screen = SCREEN_MENU;
    selected = ITEM_RESUME;
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
    if (pressed & (BIT(BTN_CIRCLE) | BIT(BTN_MENU)))
    {
        resume();
        app_draw_game(255);
        return;
    }
    if (pressed & BIT(BTN_UP))
    {
        selected = (selected + ITEM_COUNT - 1) % ITEM_COUNT;
        confirm_reset = false;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_DOWN))
    {
        selected = (selected + 1) % ITEM_COUNT;
        confirm_reset = false;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_CROSS))
    {
        switch (selected)
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
            if (!confirm_reset)
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

    static const int ICONS[ITEM_COUNT] = {ICON_PLAYER_PLAY, ICON_PLAYER_TRACK_NEXT, ICON_ROTATE, ICON_SETTINGS,
                                          ICON_DOOR_EXIT};
    const float row = 84;
    const float target = y + selected * row;
    if (bar_y < 0)
        bar_y = target;
    anim_approach(&bar_y, target, app.dt, TH_SNAP);
    draw_rrect(x + 40, bar_y, w - 80, row - 10, TH_RADIUS_SMALL, argb_alpha(TH_ROW_SELECTED, slide));
    draw_rect(x + 40, bar_y + 14, 5, row - 38, argb_alpha(TH_FOCUS, slide));
    for (int i = 0; i < ITEM_COUNT; ++i)
    {
        char label[96];
        switch (i)
        {
        case ITEM_RESUME: str_copy(label, sizeof(label), tr("Resume")); break;
        case ITEM_FAST:
            str_copy(label, sizeof(label), app.fast_forward ? tr("Fast-forward: on") : tr("Fast-forward: off"));
            break;
        case ITEM_RESET:
            str_copy(label, sizeof(label),
                     confirm_reset && selected == ITEM_RESET ? tr("Press again: unsaved progress is lost")
                                                             : tr("Back to the title screen"));
            break;
        case ITEM_SETTINGS: str_copy(label, sizeof(label), tr("Settings")); break;
        case ITEM_QUIT: str_copy(label, sizeof(label), tr("Quit to the library")); break;
        }
        const float ry = y + i * row;
        const uint32_t color = i == selected ? TH_TEXT : TH_TEXT_SOFT;
        icon_draw(ICONS[i], x + 72, ry + 18, 36, argb_alpha(i == selected ? TH_FOCUS : TH_TEXT_DIM, slide));
        text_draw_fit(x + 130, ry + 20, 30, i == selected ? FONT_BOLD : FONT_REGULAR,
                      argb_alpha(i == ITEM_RESET && confirm_reset && selected == i ? TH_DANGER : color, slide),
                      ALIGN_LEFT, w - 200, label);
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
