/*
 * Dulcamara - the on-screen keyboard over the game (app.h), while the game wants text typed: a Ren'Py game's name
 * entry (renpy.input). The console's own keyboard cannot be used (a title that imports it does not start), so this one
 * is Dulcamara's: its keys are keyboard.c's, what it types goes to the core as a keyboard's would (core/host.c), and
 * the game's own input shows the text, with the keyboard at the other end of the screen from it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../core/cores.h"
#include "../core/host.h"
#include "../i18n.h"
#include "../keyboard.h"
#include "../platform/platform.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <stdlib.h>

#define KEY_W 92.0f /* a key, in screen pixels: 11 across fill 1092, crisp at 1080p */
#define KEY_H 60.0f
#define GAP 8.0f
#define INSET 28.0f  /* the panel around the keys */
#define HINTS_H 48.0f
#define DEFLECTED 8000 /* the stick, off its middle */

static Keyboard kb;
static bool up;       /* shown, with the controller */
static bool put_away; /* hidden with Circle while the game still wants text; R3 shows it again */
static bool at_top;   /* the game's input is in the lower half: the keyboard at the top */
static float shown;   /* 0..1, as it comes in */
static uint32_t last_buttons;
/* what the keyboard was given back with, still held: kept from the game until let go (a Cross held on Done would
 * otherwise be the game's Return too, on its next line) */
static uint32_t withheld;
static bool stick_withheld, touch_withheld;
/* the touchpad: the finger's touch, for a tap (as core/host.c has it for the pointer) */
static bool finger_was_down, finger_on_key, clicked_this_touch;
static uint64_t finger_down_us;
static int finger_down_x, finger_down_y, finger_x, finger_y;

typedef struct
{
    float x, y, w, h;           /* the panel */
    float keys_x, keys_y, keys_w, keys_h;
} Layout;

static Layout layout(void)
{
    Layout l;
    l.keys_w = KB_COLUMNS * KEY_W + (KB_COLUMNS - 1) * GAP;
    l.keys_h = KB_ROWS * KEY_H + (KB_ROWS - 1) * GAP;
    l.w = l.keys_w + 2 * INSET;
    l.h = l.keys_h + 2 * INSET + HINTS_H;
    l.x = ((float)plat_width() - l.w) * 0.5f;
    l.y = at_top ? 40.0f : (float)plat_height() - l.h - 40.0f;
    l.keys_x = l.x + INSET;
    l.keys_y = l.y + INSET;
    return l;
}

static bool deflected(const PadState *p)
{
    return abs(p->lx) > DEFLECTED || abs(p->ly) > DEFLECTED;
}

/* The controller back to the game, but for what is still held. */
static void give_back(const PadState *pad, const char *why)
{
    if (up)
        np_log("keyboard: %s", why);
    up = false;
    withheld = pad->buttons;
    stick_withheld = deflected(pad);
    touch_withheld = pad->touching;
}

/* What a press does to the game. */
static void act(KbAction action, uint32_t character)
{
    switch (action)
    {
    case KB_TYPE:
        host_type(RETROK_UNKNOWN, character);
        sfx_play(SFX_CLICK);
        break;
    case KB_BACKSPACE:
        host_type(RETROK_BACKSPACE, 0);
        sfx_play(SFX_CLICK);
        break;
    case KB_RETURN:
        /* the game takes the text and ends its input, which closes the keyboard; an input that stays (a game's
         * own screen, with its own button to go on) keeps it until Circle */
        host_type(RETROK_RETURN, 0);
        sfx_play(SFX_SELECT);
        break;
    case KB_HIDE:
        put_away = true;
        sfx_play(SFX_BACK);
        break;
    case KB_NOTHING:
        break;
    }
}

/* The touchpad: the finger highlights the key under it, and a tap or the touchpad's click presses that key. */
static void touch(const PadState *pad, uint32_t presses)
{
    if (pad->touching)
    {
        const Layout l = layout();
        finger_x = pad->touch_x;
        finger_y = pad->touch_y;
        const float x = (float)finger_x * (float)plat_width() / 1920.0f;
        const float y = (float)finger_y * (float)plat_height() / 1080.0f;
        finger_on_key = kb_point(&kb, (x - l.keys_x) / l.keys_w, (y - l.keys_y) / l.keys_h);
        if (!finger_was_down)
        {
            finger_down_us = plat_ticks_us();
            finger_down_x = finger_x;
            finger_down_y = finger_y;
            clicked_this_touch = false;
        }
    }
    uint32_t character;
    if ((presses & BIT(BTN_MENU)) && finger_on_key)
    {
        clicked_this_touch = true;
        const KbAction a = kb_press(&kb, &character);
        act(a, character);
    }
    else if (!pad->touching && finger_was_down && finger_on_key && !clicked_this_touch &&
             plat_ticks_us() - finger_down_us < 250000 && abs(finger_down_x - finger_x) < 60 &&
             abs(finger_down_y - finger_y) < 60)
    {
        const KbAction a = kb_press(&kb, &character);
        act(a, character);
    }
    finger_was_down = pad->touching;
}

/* The controller's presses on the keyboard: the D-pad and the stick with the menus' repeats (pressed), the other
 * buttons once a press. */
static void typing(const PadState *pad, uint32_t pressed, uint32_t presses, bool options_tapped)
{
    static const int BUTTONS[] = {BTN_UP,     BTN_DOWN,     BTN_LEFT, BTN_RIGHT, BTN_CROSS,
                                  BTN_SQUARE, BTN_TRIANGLE, BTN_L1,   BTN_L2,    BTN_CIRCLE};
    const uint32_t moves = BIT(BTN_UP) | BIT(BTN_DOWN) | BIT(BTN_LEFT) | BIT(BTN_RIGHT);
    uint32_t character;
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); ++i)
    {
        const uint32_t bit = BIT(BUTTONS[i]);
        if (!(((bit & moves) ? pressed : presses) & bit))
            continue;
        const Keyboard before = kb;
        const KbAction a = kb_button(&kb, BUTTONS[i], &character);
        if (a == KB_NOTHING && (before.row != kb.row || before.column != kb.column || before.shift != kb.shift))
            sfx_play(SFX_CLICK);
        act(a, character);
        if (a == KB_HIDE)
        {
            give_back(pad, "put away (Circle): the controller is the game's until R3");
            return;
        }
    }
    if (options_tapped)
    {
        const KbAction a = kb_button(&kb, BTN_START, &character);
        act(a, character);
    }
    touch(pad, presses);
}

bool keyboard_frame(const PadState *pad, uint32_t pressed, bool options_tapped, PadState *game_pad)
{
    /* each press once: the menus' presses (main.c's, made every frame, whichever screen is up, so that a Circle that
     * closed Dulcamara's menu is not one here), less their repeats of L1 (Shift), held since the last frame */
    const uint32_t presses = pressed & ~(last_buttons & BIT(BTN_L1));
    last_buttons = pad->buttons;
    float top, bottom;
    const bool wanted = host_text_wanted(&top, &bottom);
    if (!wanted)
    {
        put_away = false;
        if (up)
            give_back(pad, "closed: the game has its text");
    }
    else if (!up && !put_away)
    {
        kb_reset(&kb);
        up = true;
        shown = 0.0f;
        finger_was_down = finger_on_key = false;
        np_log("keyboard: open for the game's text (its input %.0f-%.0f %% down the picture)", top * 100.0f,
               bottom * 100.0f);
        sfx_play(SFX_SELECT);
    }
    else if (put_away && (presses & BIT(BTN_R3)))
    {
        put_away = false;
        up = true;
        shown = 0.0f;
        finger_was_down = finger_on_key = false;
        np_log("keyboard: shown again (R3)");
        sfx_play(SFX_SELECT);
        return true;
    }
    if (up)
    {
        /* away from the game's input, so that what is typed shows; at the foot when the core does not say where */
        at_top = (top + bottom) * 0.5f > 0.5f;
        typing(pad, pressed, presses, options_tapped);
        return true; /* the Circle that put it away is the keyboard's too */
    }
    withheld &= pad->buttons;
    stick_withheld = stick_withheld && deflected(pad);
    touch_withheld = touch_withheld && pad->touching;
    game_pad->buttons &= ~withheld;
    if (stick_withheld)
        game_pad->lx = game_pad->ly = 0;
    if (touch_withheld)
        game_pad->touching = false;
    return false;
}

/* A key's face: its character, or the special key's name. */
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

void keyboard_draw(void)
{
    if (put_away)
    {
        /* hidden while the game still wants text: how to get it back */
        const float size = 24, h = 56, gw = pad_glyph_width(GLYPH_R3, size);
        const char *label = tr("Keyboard");
        const float w = 64 + gw + size * 0.35f + text_width(size, FONT_REGULAR, label) + 28;
        const float x = ((float)plat_width() - w) * 0.5f, y = 36;
        draw_rrect(x, y, w, h, h * 0.5f, TH_PILL_A(0xe0));
        icon_draw(ICON_KEYBOARD, x + 22, y + 13, 30, TH_FOCUS);
        draw_hint(x + 64, y + (h - size) * 0.5f - 2, GLYPH_R3, label, size, TH_TEXT);
        return;
    }
    if (!up)
        return;
    anim_approach(&shown, 1.0f, app.dt, 12.0f);
    const float a = shown, rise = (1.0f - shown) * (at_top ? -24.0f : 24.0f);
    Layout l = layout();
    l.y += rise;
    l.keys_y += rise;
    draw_rrect(l.x, l.y, l.w, l.h, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), a));
    draw_rect(l.x + TH_RADIUS, at_top ? l.y + l.h - 4 : l.y, l.w - 2 * TH_RADIUS, 4, argb_alpha(TH_FOCUS, a));

    const KbKey lit = kb_key(&kb, kb.row, kb.column);
    for (int row = 0; row < KB_ROWS; ++row)
        for (int column = 0; column < KB_COLUMNS;)
        {
            const KbKey key = kb_key(&kb, row, column);
            column = key.column + key.span;
            const float kx = l.keys_x + (float)key.column * (KEY_W + GAP), ky = l.keys_y + (float)row * (KEY_H + GAP);
            const float kw = (float)key.span * KEY_W + (float)(key.span - 1) * GAP;
            const bool on = row == kb.row && key.column == lit.column;
            const bool shifted = key.kind == KB_SHIFT && kb.shift != KB_SHIFT_OFF;
            draw_rrect(kx, ky, kw, KEY_H, TH_RADIUS_SMALL,
                       argb_alpha(on ? TH_FOCUS : shifted ? TH_ROW_SELECTED : TH_PILL_A(0xff), a));
            if (shifted && kb.shift == KB_SHIFT_LOCKED && !on)
                draw_rrect_outline(kx, ky, kw, KEY_H, TH_RADIUS_SMALL, 2, argb_alpha(TH_FOCUS, a));
            char buffer[2];
            const char *label = key_label(&key, buffer);
            const float size = key.kind == KB_CHARACTER ? 32 : 24;
            const uint32_t color = on ? 0xffffffffu : shifted ? TH_FOCUS : TH_TEXT;
            text_draw(kx + kw * 0.5f, ky + KEY_H * 0.5f - size * 0.6f, size, FONT_BOLD, argb_alpha(color, a),
                      ALIGN_CENTER, label);
        }

    const float size = 24, hy = l.keys_y + l.keys_h + (INSET + HINTS_H - size) * 0.5f - 6;
    const uint32_t hint = argb_alpha(TH_HINT, a);
    static const int SHIFTS[] = {GLYPH_L1, GLYPH_L2};
    draw_set_glyph_alpha(a);
    float hx = l.keys_x;
    hx += draw_hint(hx, hy, GLYPH_SQUARE, "Delete", size, hint);
    hx += draw_hint(hx, hy, GLYPH_TRIANGLE, "Space", size, hint);
    hx += draw_hint_combo(hx, hy, SHIFTS, 2, '/', "Shift", size, hint) + size * 1.4f;
    hx += draw_hint(hx, hy, GLYPH_START, "Done", size, hint);
    draw_hint(hx, hy, GLYPH_CIRCLE, "Hide", size, hint);
    draw_set_glyph_alpha(1.0f);
}
