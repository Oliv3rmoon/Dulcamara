/*
 * Dulcamara - the library: a row of title screens on Nightshade's black-and-red backdrop, the chosen one
 * large in the middle with its title under it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../core/cores.h"
#include "../covers.h"
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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CARD_W 720.0f /* the chosen game's title screen, 4:3 */
#define SIDE_W 432.0f /* its neighbours' */
#define CARD_Y 190.0f
#define GAP 48.0f

static float pos;       /* the row's position, animated towards the cursor */
static float pulse;     /* the chosen card's glow */
static bool positioned;

/* ---------------------------------------------------------------- the backdrop */

/* Nightshade's: near black, a low red glow rising from below the cards, right of the middle, and a fainter one
 * behind them (Mandrake's BACKDROP_GLOW, ui/coverflow.c there). */
void shelf_backdrop(void)
{
    enum { W = 960, H = 540 };
    static uint8_t *grey;
    if (!grey)
    {
        grey = malloc((size_t)W * H);
        if (!grey)
        {
            draw_rect(0, 0, (float)plat_width(), (float)plat_height(), TH_BG);
            return;
        }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float t = (float)y / (H - 1), u = (float)x / (W - 1);
                const float gx = (u - 0.60f) / 0.55f, gy = (t - 1.04f) / 0.32f;
                const float bx = (u - 0.5f) / 0.75f, by = (t - 0.46f) / 0.34f;
                const float v = 0.035f + 0.34f * expf(-(gx * gx + gy * gy)) + 0.05f * expf(-(bx * bx + by * by));
                grey[(size_t)y * W + x] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f);
            }
        plat_backdrop_changed();
    }
    plat_draw_backdrop(grey, W, H, theme.backdrop_tint);
}

/* ---------------------------------------------------------------- the cards */

static int cursor(void)
{
    const int n = app.library.count;
    if (n <= 0)
        return 0;
    int c = app.settings.last_game;
    return c < 0 ? 0 : c >= n ? n - 1 : c;
}

void shelf_open(void)
{
    app.screen = SCREEN_LIBRARY;
    positioned = false;
}

void shelf_library_changed(void)
{
    if (app.settings.last_game >= app.library.count)
        app.settings.last_game = app.library.count ? app.library.count - 1 : 0;
    positioned = false;
}

/* Where card `d` places from the middle sits, and how wide it is. */
static void card_place(float d, float *cx, float *w)
{
    const float a = fabsf(d);
    const float step1 = CARD_W * 0.5f + GAP + SIDE_W * 0.5f, step = SIDE_W + GAP;
    const float off = a <= 1.0f ? a * step1 : step1 + (a - 1.0f) * step;
    *cx = plat_width() * 0.5f + (d < 0 ? -off : off);
    *w = a <= 1.0f ? CARD_W + (SIDE_W - CARD_W) * a : SIDE_W * powf(0.92f, a - 1.0f);
}

/* The picture upside down under the card, fading out: the floor's reflection. */
static void draw_reflection(PlatTexture *tex, float x, float y, float w, float h, float alpha)
{
    const float rh = h * 0.28f;
    const uint32_t top = argb_alpha(0x55ffffffu, alpha), bottom = 0x00ffffffu;
    const PlatVertex v[6] = {{x, y, 0, 1, top},           {x + w, y, 1, 1, top},
                             {x + w, y + rh, 1, 1 - 0.28f, bottom}, {x, y, 0, 1, top},
                             {x + w, y + rh, 1, 1 - 0.28f, bottom}, {x, y + rh, 0, 1 - 0.28f, bottom}};
    plat_draw_mesh(tex, v, 6, NULL, 0);
}

static void draw_placeholder(const Game *g, float x, float y, float w, float h, float alpha, bool chosen)
{
    const uint32_t colors[2] = {argb_alpha(0xff1a1a1fu, alpha), argb_alpha(0xff0d0d10u, alpha)};
    const float stops[2] = {0.0f, 1.0f};
    draw_vgradient(x, y, w, h, colors, stops, 2);
    const float icon = w * 0.16f;
    icon_draw(ICON_PHOTO, x + (w - icon) * 0.5f, y + h * 0.30f, icon, argb_alpha(TH_TEXT_DIM, alpha * 0.6f));
    const float size = chosen ? 34 : 24;
    text_draw_fit(x + w * 0.5f, y + h * 0.30f + icon + 24, size, FONT_BOLD, argb_alpha(TH_TEXT_SOFT, alpha),
                  ALIGN_CENTER, w - 48, g->title);
}

static void draw_card(int index, float d)
{
    float cx, w;
    card_place(d, &cx, &w);
    const float h = w * 0.75f, a = fabsf(d);
    const float x = cx - w * 0.5f, y = CARD_Y + (CARD_W * 0.75f - h) * 0.5f;
    if (x > plat_width() || x + w < 0)
        return;
    const float alpha = a <= 1.0f ? 1.0f - 0.25f * a : fmaxf(0.0f, 0.75f - (a - 1.0f) * 0.45f);
    const bool chosen = a < 0.5f;
    const Game *g = &app.library.games[index];
    PlatTexture *tex = covers_get(index);
    if (chosen)
        draw_glow(x, y, w, h, 46 + 10 * pulse, argb_alpha(TH_FOCUS, 0.22f + 0.10f * pulse));
    if (tex)
    {
        const uint8_t shade = (uint8_t)(255 * (a <= 1.0f ? 1.0f - 0.45f * a : 0.55f));
        plat_draw_texture(tex, x, y, w, h, argb_alpha(0xff000000u | shade << 16 | shade << 8 | shade, alpha), true);
        draw_reflection(tex, x, y + h + 6, w, h, alpha * (a <= 1.0f ? 1.0f - 0.5f * a : 0.4f));
    }
    else
        draw_placeholder(g, x, y, w, h, alpha, chosen);
    if (chosen)
        draw_rrect_outline(x - 6, y - 6, w + 12, h + 12, 10, 4, argb_alpha(TH_FOCUS, 1.0f - 2.0f * a));
}

static const char *played_text(long long when, char *out, size_t size)
{
    if (!when)
        return tr("Not played yet");
    const long long ago = (long long)time(NULL) - when;
    if (ago < 120)
        snprintf(out, size, "%s", tr("Played just now"));
    else if (ago < 7200)
        snprintf(out, size, tr("Played %lld minutes ago"), ago / 60);
    else if (ago < 172800)
        snprintf(out, size, tr("Played %lld hours ago"), ago / 3600);
    else
        snprintf(out, size, tr("Played %lld days ago"), ago / 86400);
    return out;
}

static void draw_header(void)
{
    const float y = 54;
    draw_rrect(TH_MARGIN, y + 6, 10, 34, 5, TH_FOCUS);
    text_draw(TH_MARGIN + 26, y, 40, FONT_BOLD, TH_TEXT, ALIGN_LEFT, NP_TITLE);
    text_draw(TH_MARGIN + 26 + text_width(40, FONT_BOLD, NP_TITLE) + 18, y + 13, 24, FONT_REGULAR, TH_TEXT_DIM,
              ALIGN_LEFT, tr("a Nightshade app"));
    char right[96], clock[32];
    plat_clock(clock, sizeof(clock));
    if (app.library.count == 1)
        snprintf(right, sizeof(right), "%s   %s", tr("1 game"), clock);
    else
        snprintf(right, sizeof(right), "%d %s   %s", app.library.count, tr("games"), clock);
    text_draw(plat_width() - TH_MARGIN, y + 8, 28, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, right);
    if (update_state() == UPDATE_INSTALLED)
        text_draw(plat_width() - TH_MARGIN, y + 50, 22, FONT_REGULAR, TH_GOOD, ALIGN_RIGHT,
                  tr("Update installed: restart Dulcamara to use it"));
}

static void draw_empty(void)
{
    const float w = 1160, h = 470, x = (plat_width() - w) * 0.5f, y = 250;
    draw_rrect(x, y, w, h, TH_RADIUS, TH_CARD_A(0xe8));
    draw_rect(x, y, w, 4, TH_FOCUS);
    icon_draw(ICON_FOLDER, x + 56, y + 56, 64, TH_FOCUS);
    text_draw(x + 150, y + 62, 40, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("No games yet"));
    const char *lines[] = {
        "Copy each game's folder, the one that holds RPG_RT.ldb and RPG_RT.lmt, into",
        NP_DATA "/games  (over FTP, for example).",
        ".zip and .easyrpg archives of a game work as they are.",
        "Games that need the RPG Maker run-time package (RTP): put it in",
        NP_DATA "/rtp/2000 or rtp/2003.",
        "No games come with Dulcamara: copy your own.",
    };
    float ly = y + 160;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i, ly += 44)
        text_draw(x + 56, ly, 28, i == 1 || i == 4 ? FONT_BOLD : FONT_REGULAR, i == 1 || i == 4 ? TH_TEXT : TH_TEXT_DIM,
                  ALIGN_LEFT, tr(lines[i]));
}

void shelf_screen(uint32_t pressed)
{
    const int n = app.library.count;
    int c = cursor();
    if (n > 0)
    {
        int move = 0;
        if (pressed & BIT(BTN_LEFT))
            move = -1;
        if (pressed & BIT(BTN_RIGHT))
            move = 1;
        if (pressed & BIT(BTN_L1))
            move = -5;
        if (pressed & BIT(BTN_R1))
            move = 5;
        if (move)
        {
            int to = c + move;
            to = to < 0 ? 0 : to >= n ? n - 1 : to;
            if (to != c)
            {
                c = to;
                app.settings.last_game = c;
                sfx_play(SFX_CLICK);
            }
        }
        if (pressed & BIT(BTN_CROSS))
        {
            sfx_play(SFX_SELECT);
            app_start_game(c);
            return;
        }
        if (pressed & BIT(BTN_TRIANGLE))
        {
            /* the order: by title, or the last played first */
            const char *keep = app.library.games[c].id;
            char id[24];
            str_copy(id, sizeof(id), keep);
            app.settings.sort_mode = !app.settings.sort_mode;
            library_sort(&app.library, app.settings.sort_mode);
            for (int i = 0; i < n; ++i)
                if (!strcmp(app.library.games[i].id, id))
                    app.settings.last_game = c = i;
            covers_start(&app.library);
            positioned = false;
            app_toast(app.settings.sort_mode ? "Last played first" : "By title");
            app_save_settings();
            sfx_play(SFX_CLICK);
        }
    }
    if (pressed & BIT(BTN_SQUARE))
    {
        sfx_play(SFX_SELECT);
        app_rescan();
        char msg[64];
        snprintf(msg, sizeof(msg), app.library.count == 1 ? "%d game found" : "%d games found", app.library.count);
        app_toast(msg);
        c = cursor();
    }
    if (pressed & BIT(BTN_START))
    {
        sfx_play(SFX_SELECT);
        app_open_settings(SCREEN_LIBRARY);
        return;
    }

    if (!positioned)
    {
        pos = (float)c;
        positioned = true;
    }
    anim_approach(&pos, (float)c, app.dt, 14.0f);
    pulse = 0.5f + 0.5f * sinf((float)plat_ticks_us() / 1e6f * 2.4f);

    shelf_backdrop();
    draw_header();
    if (n == 0)
        draw_empty();
    else
    {
        /* far cards first, the chosen one last, over its neighbours */
        for (int ring = 4; ring >= 0; --ring)
            for (int side = -1; side <= 1; side += 2)
            {
                const int i = c + side * ring;
                if (i < 0 || i >= n || (ring == 0 && side > 0))
                    continue;
                draw_card(i, (float)i - pos);
            }
        const Game *g = &app.library.games[c];
        const float ty = CARD_Y + CARD_W * 0.75f + 72;
        text_draw_fit(plat_width() * 0.5f, ty, 46, FONT_BOLD, TH_TEXT, ALIGN_CENTER, 1500, g->title);
        char meta[200], played[64];
        char renpy[16];
        snprintf(renpy, sizeof(renpy), g->engine ? "Ren'Py %d" : "Ren'Py", g->engine);
        const char *engine = core_is_renpy(g->core) ? renpy
                             : g->engine == 2003    ? "RPG Maker 2003"
                             : g->engine == 2000    ? "RPG Maker 2000"
                                                    : "RPG Maker";
        snprintf(meta, sizeof(meta), "%s   \xC2\xB7   %s   \xC2\xB7   %s", engine,
                 g->archive ? tr("Archive") : tr("Folder"), played_text(g->played, played, sizeof(played)));
        text_draw_fit(plat_width() * 0.5f, ty + 66, 26, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER, 1500, meta);
        char count[32];
        snprintf(count, sizeof(count), "%d / %d", c + 1, n);
        text_draw(plat_width() * 0.5f, ty + 112, 22, FONT_REGULAR, TH_HINT, ALIGN_CENTER, count);
    }
    static const int glyphs[] = {GLYPH_CROSS, GLYPH_TRIANGLE, GLYPH_SQUARE, GLYPH_START};
    static const char *const labels[] = {"Play", "Sort", "Look again", "Settings"};
    const int first = n > 0 ? 0 : 2;
    app_draw_hints(glyphs + first, labels + first, 4 - first,
                   n > 0 ? (app.settings.sort_mode ? "Last played first" : "By title") : NULL);
}
