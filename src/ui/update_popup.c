/*
 * Dulcamara - the update's popup (update_popup.h). What it shows follows update_state(): the offer while the
 * update is available, the download's progress while it installs, then installed or failed. Circle always closes
 * it; a download already started goes on without it, and Settings > System shows how far it has got.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "update_popup.h"

#include "../app.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "../update.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>

static bool open;
static float shown;  /* 0..1 as it rises */
static int choice;   /* the highlighted button: 0 the left one */
static float spin;   /* seconds, for the bar while the size isn't known */

void update_popup_open(void)
{
    if (update_state() != UPDATE_AVAILABLE)
        return;
    open = true;
    shown = 0.0f;
    choice = 0;
    np_log("update: offered %s%s in the popup", update_version(), update_test_build() ? " (a test build)" : "");
}

bool update_popup_is_open(void)
{
    return open;
}

/* The buttons the popup has now, left to right; how many. */
static int buttons(const char *labels[2])
{
    switch (update_state())
    {
    case UPDATE_AVAILABLE:
        labels[0] = "Update now";
        labels[1] = "Later";
        return 2;
    case UPDATE_INSTALLED:
        labels[0] = "Close Dulcamara";
        labels[1] = "Later";
        return 2;
    case UPDATE_FAILED:
        if (update_can_retry())
        {
            labels[0] = "Try again";
            labels[1] = "Later";
            return 2;
        }
        labels[0] = "Close";
        return 1;
    case UPDATE_INSTALLING:
        labels[0] = "Hide";
        return 1;
    default: return 0;
    }
}

static void close_popup(void)
{
    open = false;
    if (update_state() == UPDATE_INSTALLING)
        app_toast_for("The update goes on downloading: Settings > System shows how far it has got.", 4.0f);
}

bool update_popup_input(uint32_t *pressed)
{
    if (!open)
        return false;
    const uint32_t p = *pressed;
    *pressed = 0;
    const char *labels[2];
    const int count = buttons(labels);
    if (count == 0)
    {
        open = false; /* nothing to offer any more (a test build, and Test builds turned off) */
        return true;
    }
    if (choice >= count)
        choice = count - 1;
    if ((p & (BIT(BTN_LEFT) | BIT(BTN_RIGHT))) && count > 1)
    {
        choice = p & BIT(BTN_LEFT) ? 0 : 1;
        sfx_play(SFX_CLICK);
    }
    if (p & BIT(BTN_CIRCLE))
    {
        sfx_play(SFX_BACK);
        close_popup();
        return true;
    }
    if (!(p & BIT(BTN_CROSS)))
        return true;
    switch (update_state())
    {
    case UPDATE_AVAILABLE:
    case UPDATE_FAILED:
        if (choice == 0 && update_can_retry())
        {
            np_log("update: trying the download again");
            update_install();
            sfx_play(SFX_SELECT);
        }
        else if (choice == 0 && update_state() == UPDATE_AVAILABLE)
        {
            update_install();
            sfx_play(SFX_SELECT);
        }
        else
        {
            sfx_play(SFX_BACK);
            close_popup();
        }
        break;
    case UPDATE_INSTALLED:
        if (choice == 0)
        {
            np_log("update: closing to use %s", update_version());
            app_save_settings();
            app.quit_requested = true;
        }
        sfx_play(SFX_BACK);
        open = false;
        break;
    default:
        sfx_play(SFX_BACK);
        close_popup();
        break;
    }
    return true;
}

static void megabytes(char *text, size_t size, uint64_t bytes)
{
    snprintf(text, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

void update_popup_draw(void)
{
    if (!open)
        return;
    const int state = update_state();
    const char *labels[2];
    const int count = buttons(labels);
    if (count == 0)
    {
        open = false;
        return;
    }
    shown = fminf(shown + app.dt * 7.0f, 1.0f);
    spin += app.dt;
    const float a = shown;
    const float w = 900, h = 560, pad = 56;
    const float x = (plat_width() - w) * 0.5f, y = (plat_height() - h) * 0.5f + (1.0f - a) * 30.0f;
    const float cx = x + w * 0.5f, inner = w - 2 * pad;
    draw_rect(0, 0, (float)plat_width(), (float)plat_height(), argb_alpha(0xb0000000u, a));
    draw_glow(x, y, w, h, 40, argb_alpha(TH_FOCUS, 0.16f * a));
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(0xfa), a));
    draw_rect(x, y, w, 4, argb_alpha(TH_FOCUS, a));

    const bool failed = state == UPDATE_FAILED, done = state == UPDATE_INSTALLED;
    const uint32_t accent = failed ? TH_DANGER : done ? TH_GOOD : TH_FOCUS;
    const int icon = failed ? ICON_ALERT_TRIANGLE : done ? ICON_CIRCLE_CHECK : ICON_DOWNLOAD;
    /* the arrow settles as the popup rises, and bobs while it downloads */
    const float bob = state == UPDATE_INSTALLING ? sinf(spin * 3.0f) * 4.0f : (1.0f - a) * -16.0f;
    icon_draw(icon, cx - 36, y + pad - 8 + bob, 72, argb_alpha(accent, a));

    char line[200], size_text[32], total_text[32];
    float ty = y + pad + 92;
    switch (state)
    {
    case UPDATE_AVAILABLE:
        text_draw(cx, ty, 42, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER,
                  tr(update_test_build() ? "Test build available" : "Update available"));
        snprintf(line, sizeof(line), tr("Dulcamara %s is ready to install."), update_version());
        text_draw_fit(cx, ty + 66, 28, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_CENTER, inner, line);
        if (update_size() > 0)
        {
            megabytes(size_text, sizeof(size_text), update_size());
            snprintf(line, sizeof(line), tr("Download size: %s"), size_text);
            text_draw(cx, ty + 110, 24, FONT_REGULAR, argb_alpha(TH_FOCUS, a), ALIGN_CENTER, line);
        }
        text_draw_fit(cx, ty + 156, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, inner,
                      tr("Your games, saves and settings are kept."));
        text_draw_fit(cx, ty + 190, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, inner,
                      tr("It is used from the next time Dulcamara opens."));
        break;
    case UPDATE_INSTALLING:
    {
        uint64_t got, total;
        update_progress(&got, &total);
        text_draw(cx, ty, 42, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER, tr("Downloading"));
        snprintf(line, sizeof(line), tr("Dulcamara %s"), update_version());
        text_draw(cx, ty + 62, 26, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, line);
        const float bx = x + pad + 40, bw = w - 2 * (pad + 40), by = ty + 120, bh = 14;
        draw_rrect(bx, by, bw, bh, bh * 0.5f, argb_alpha(TH_PILL_A(0xff), a));
        if (total > 0)
        {
            const float share = fminf((float)got / (float)total, 1.0f);
            draw_rrect(bx, by, fmaxf(bh, bw * share), bh, bh * 0.5f, argb_alpha(TH_FOCUS, a));
            megabytes(size_text, sizeof(size_text), got);
            megabytes(total_text, sizeof(total_text), total);
            snprintf(line, sizeof(line), "%d%%   %s / %s", (int)(share * 100.0f), size_text, total_text);
        }
        else
        {
            /* the size isn't known yet: a light running along the bar */
            const float run = fmodf(spin * 0.7f, 1.3f) - 0.15f;
            draw_rrect(bx + fmaxf(0.0f, bw * run - 90), by, fminf(180, bw - fmaxf(0.0f, bw * run - 90)), bh,
                       bh * 0.5f, argb_alpha(TH_FOCUS, a));
            megabytes(size_text, sizeof(size_text), got);
            snprintf(line, sizeof(line), "%s", size_text);
        }
        text_draw(cx, by + 34, 24, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_CENTER, line);
        text_draw_fit(cx, by + 82, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, inner,
                      tr("You can hide this: the download goes on."));
        break;
    }
    case UPDATE_INSTALLED:
        text_draw(cx, ty, 42, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER, tr("Update installed"));
        snprintf(line, sizeof(line), tr("Dulcamara %s is ready."), update_version());
        text_draw_fit(cx, ty + 66, 28, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_CENTER, inner, line);
        text_draw_fit(cx, ty + 120, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, inner,
                      tr("Close Dulcamara and open it again to use it."));
        break;
    case UPDATE_FAILED:
    default:
        text_draw(cx, ty, 42, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER, tr("The update didn't install"));
        text_draw_fit(cx, ty + 66, 26, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_CENTER, inner, update_message());
        text_draw_fit(cx, ty + 120, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, inner,
                      tr(update_can_retry() ? "Nothing was changed. Try again goes on from where it stopped."
                                            : "Nothing was changed. Settings > System can check again."));
        break;
    }

    /* the buttons, side by side along the bottom */
    const float gap = 20, bh = 72, by = y + h - pad - bh - 40;
    const float bw = count == 2 ? (inner - gap) * 0.5f : inner * 0.5f;
    float bx = count == 2 ? x + pad : cx - bw * 0.5f;
    for (int i = 0; i < count; ++i, bx += bw + gap)
    {
        const bool on = i == choice;
        draw_rrect(bx, by, bw, bh, TH_RADIUS_SMALL, argb_alpha(on ? TH_FOCUS : TH_PILL_A(0xff), a));
        text_draw_fit(bx + bw * 0.5f, by + bh * 0.5f - 17, 28, on ? FONT_BOLD : FONT_REGULAR,
                      argb_alpha(on ? 0xffffffffu : TH_TEXT, a), ALIGN_CENTER, bw - 24, tr(labels[i]));
    }
    /* and what the pad does */
    const float hs = 24;
    float hx = x + pad, hy = y + h - pad - hs + 6;
    draw_set_glyph_alpha(a);
    if (count > 1)
    {
        static const int arrows[] = {GLYPH_LEFT, GLYPH_RIGHT};
        hx += draw_hint_combo(hx, hy, arrows, 2, '/', tr("Choose"), hs, argb_alpha(TH_HINT, a)) + 24;
    }
    hx += draw_hint(hx, hy, GLYPH_CROSS, tr("Select"), hs, argb_alpha(TH_HINT, a)) + 24;
    draw_hint(hx, hy, GLYPH_CIRCLE, tr(state == UPDATE_INSTALLING ? "Hide" : "Later"), hs, argb_alpha(TH_HINT, a));
    draw_set_glyph_alpha(1.0f);
}
