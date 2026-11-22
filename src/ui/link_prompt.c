/*
 * Dulcamara - a game's link typed on Dulcamara's own keyboard (keyboard.h, with its link keys), in a panel over
 * Settings as the insider code's is (insider_prompt.c): the console's own keyboard can't be used in Dulcamara. It
 * starts at "https://"; Done starts the download (links_ui.h) when what is typed is a link, Circle closes it.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "links_ui.h"

#include "../app.h"
#include "../i18n.h"
#include "../keyboard.h"
#include "../links.h"
#include "../platform/platform.h"
#include "draw.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define KEY_W 92.0f /* the keys as the insider code's keyboard has them */
#define KEY_H 60.0f
#define GAP 8.0f
#define INSET 28.0f
#define TYPED_MAX (LINKS_URL_MAX - 1)

static Keyboard kb;
static bool open;
static char typed[TYPED_MAX + 1];
static bool not_a_link; /* Done found no link in what is typed: it says so until the next key */
static uint32_t last_buttons;
static float shown;

void link_prompt_open(void)
{
    kb_reset_url(&kb);
    open = true;
    str_copy(typed, sizeof(typed), "https://");
    not_a_link = false;
    shown = 0.0f;
    last_buttons = app.pads[app_nav_pad()].buttons;
    sfx_play(SFX_SELECT);
}

static void act(KbAction action, uint32_t character)
{
    const size_t n = strlen(typed);
    switch (action)
    {
    case KB_TYPE:
        not_a_link = false;
        if (n < TYPED_MAX && character > 0x20 && character < 0x7f) /* a link has no spaces */
        {
            typed[n] = (char)character;
            typed[n + 1] = '\0';
        }
        sfx_play(SFX_CLICK);
        break;
    case KB_BACKSPACE:
        not_a_link = false;
        if (n)
            typed[n - 1] = '\0';
        sfx_play(SFX_CLICK);
        break;
    case KB_RETURN:
        if (!link_url_valid(typed))
        {
            not_a_link = true; /* nothing starts: it stays open to be put right */
            sfx_play(SFX_BACK);
            break;
        }
        open = false;
        sfx_play(SFX_SELECT);
        links_ui_from_url(typed);
        break;
    case KB_HIDE:
        open = false;
        sfx_play(SFX_BACK);
        break;
    case KB_NOTHING: break;
    }
}

bool link_prompt_input(uint32_t pressed)
{
    if (!open)
        return false;
    const uint32_t held = app.pads[app_nav_pad()].buttons;
    const uint32_t presses = pressed & ~(last_buttons & BIT(BTN_L1)); /* the menus repeat L1, which is Shift here */
    last_buttons = held;
    static const int BUTTONS[] = {BTN_UP,       BTN_DOWN, BTN_LEFT, BTN_RIGHT,  BTN_CROSS, BTN_SQUARE,
                                  BTN_TRIANGLE, BTN_L1,   BTN_L2,   BTN_CIRCLE, BTN_START};
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]) && open; ++i)
    {
        if (!(presses & BIT(BUTTONS[i])))
            continue;
        const Keyboard before = kb;
        uint32_t character;
        const KbAction a = kb_button(&kb, BUTTONS[i], &character);
        if (a == KB_NOTHING && (before.row != kb.row || before.column != kb.column || before.shift != kb.shift))
            sfx_play(SFX_CLICK);
        act(a, character);
    }
    return true;
}

static const char *key_label(const KbKey *key, char *buffer)
{
    switch (key->kind)
    {
    case KB_CHARACTER:
        buffer[0] = (char)key->character;
        buffer[1] = '\0';
        return buffer;
    case KB_SHIFT: return kb.shift == KB_SHIFT_LOCKED ? tr("Caps") : tr("Shift");
    case KB_SPACE: return tr("Space");
    case KB_DELETE: return tr("Delete");
    case KB_DONE: return tr("Done");
    }
    return "";
}

void link_prompt_draw(void)
{
    if (!open)
        return;
    anim_approach(&shown, 1.0f, app.dt, 12.0f);
    const float a = shown;
    const float keys_w = KB_COLUMNS * KEY_W + (KB_COLUMNS - 1) * GAP, keys_h = KB_ROWS * KEY_H + (KB_ROWS - 1) * GAP;
    const float head = 236, hints = 48;
    const float w = keys_w + 2 * INSET, h = head + keys_h + 2 * INSET + hints;
    const float x = ((float)plat_width() - w) * 0.5f;
    const float y = ((float)plat_height() - h) * 0.5f + (1.0f - a) * 24.0f;
    draw_rect(0, 0, (float)plat_width(), (float)plat_height(), argb_alpha(0xa0000000u, a));
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD, a));
    draw_rect(x + TH_RADIUS, y, w - 2 * TH_RADIUS, 4, argb_alpha(TH_FOCUS, a));

    const float kx0 = x + INSET;
    text_draw(kx0, y + 30, 34, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, tr("Add game from link"));
    text_draw_fit(kx0, y + 78, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_LEFT, keys_w,
                  tr("A direct link to a game's .zip (it starts the download itself). No logins or download pages. "
                     "Long links: put them in " NP_DATA "/links.txt instead."));
    /* what is typed: its end, when all of it does not fit */
    char field[TYPED_MAX + 8];
    snprintf(field, sizeof(field), "%s_", typed);
    const float field_w = keys_w - 48;
    const char *visible = field;
    char cut[TYPED_MAX + 8];
    if (text_width(30, FONT_BOLD, field) > field_w)
    {
        const size_t length = strlen(field);
        for (const char *p = field + (length > 120 ? length - 120 : 0); *p; ++p) /* a line holds about 60 */
        {
            snprintf(cut, sizeof(cut), "...%s", p);
            if (text_width(30, FONT_BOLD, cut) <= field_w)
            {
                visible = cut;
                break;
            }
        }
    }
    const float fy = y + 118;
    draw_rrect(kx0, fy, keys_w, 64, TH_RADIUS_SMALL, argb_alpha(TH_PILL_A(0xff), a));
    draw_rrect_outline(kx0, fy, keys_w, 64, TH_RADIUS_SMALL, 2, argb_alpha(not_a_link ? TH_DANGER : TH_FOCUS, a));
    text_draw(kx0 + 24, fy + 15, 30, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, visible);
    if (not_a_link)
        text_draw_fit(kx0, fy + 78, 24, FONT_REGULAR, argb_alpha(TH_DANGER, a), ALIGN_LEFT, keys_w,
                      tr("That isn't a link: it starts with https:// or http:// and a server's name."));

    const float keys_y = y + head + INSET;
    const KbKey lit = kb_key(&kb, kb.row, kb.column);
    for (int row = 0; row < KB_ROWS; ++row)
        for (int column = 0; column < KB_COLUMNS;)
        {
            const KbKey key = kb_key(&kb, row, column);
            column = key.column + key.span;
            const float kx = kx0 + (float)key.column * (KEY_W + GAP), ky = keys_y + (float)row * (KEY_H + GAP);
            const float kw = (float)key.span * KEY_W + (float)(key.span - 1) * GAP;
            const bool on = row == kb.row && key.column == lit.column;
            const bool shifted = key.kind == KB_SHIFT && kb.shift != KB_SHIFT_OFF;
            draw_rrect(kx, ky, kw, KEY_H, TH_RADIUS_SMALL,
                       argb_alpha(on ? TH_FOCUS : shifted ? TH_ROW_SELECTED : TH_PILL_A(0xff), a));
            char buffer[2];
            const char *label = key_label(&key, buffer);
            const float size = key.kind == KB_CHARACTER ? 32 : 24;
            const uint32_t color = on ? 0xffffffffu : shifted ? TH_FOCUS : TH_TEXT;
            text_draw(kx + kw * 0.5f, ky + KEY_H * 0.5f - size * 0.6f, size, FONT_BOLD, argb_alpha(color, a),
                      ALIGN_CENTER, label);
        }

    const float size = 24, hy = keys_y + keys_h + (INSET + hints - size) * 0.5f - 6;
    const uint32_t hint = argb_alpha(TH_HINT, a);
    draw_set_glyph_alpha(a);
    float hx = kx0;
    hx += draw_hint(hx, hy, GLYPH_SQUARE, tr("Delete"), size, hint);
    hx += draw_hint(hx, hy, GLYPH_START, tr("Done"), size, hint);
    draw_hint(hx, hy, GLYPH_CIRCLE, tr("Cancel"), size, hint);
    draw_set_glyph_alpha(1.0f);
}
