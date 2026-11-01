/*
 * Dulcamara - the library: a row of title screens on Nightshade's black-and-red backdrop, the chosen one
 * large in the middle with its title under it. With RPG Maker's and Ren'Py's games both there, each kind is a
 * section of its own, one row at a time, its tab at the top.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../community.h"
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

/* The sections (library.h): with games of both kinds the row is the cursor's section, RPG Maker's games or Ren'Py's,
 * each in its own order, and Up and Down go to the other, at the game last chosen there. With one kind there are no
 * tabs, and the row is the whole library. */
static char section_game[SECTION_COUNT][24]; /* the id of the game last chosen in each section */
static float section_in = 1.0f;              /* 0..1: the row coming in after Up or Down */
static float section_from;                   /* where it comes from: -1 above, 1 below */
static bool vertical_held;                   /* Up or Down held: one section a press */

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

/* The row the cursor is in: its section, where its games start and how many there are. */
static int row_of(int c, int *first, int *count)
{
    const int section = app.library.count > 0 ? library_section(&app.library.games[c]) : SECTION_RPG_MAKER;
    *first = library_section_start(&app.library, section, count);
    return section;
}

static int sections_with_games(void)
{
    int n = 0;
    for (int s = 0; s < SECTION_COUNT; ++s)
    {
        int count;
        library_section_start(&app.library, s, &count);
        n += count > 0;
    }
    return n;
}

/* The section before (step -1) or after (1) this one that has games, and the game to choose there into *game. */
static int next_section(int section, int step, int *game)
{
    for (int i = 1; i < SECTION_COUNT; ++i)
    {
        const int s = ((section + step * i) % SECTION_COUNT + SECTION_COUNT) % SECTION_COUNT;
        int count;
        const int first = library_section_start(&app.library, s, &count);
        if (!count)
            continue;
        *game = first;
        for (int g = first; g < first + count; ++g)
            if (!strcmp(app.library.games[g].id, section_game[s]))
                *game = g;
        return s;
    }
    return section;
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
    const float h = w * 0.75f, a = fabsf(d), fade = section_in;
    const float x = cx - w * 0.5f, y = CARD_Y + (CARD_W * 0.75f - h) * 0.5f + (1.0f - fade) * 60.0f * section_from;
    if (x > plat_width() || x + w < 0)
        return;
    const float alpha = (a <= 1.0f ? 1.0f - 0.25f * a : fmaxf(0.0f, 0.75f - (a - 1.0f) * 0.45f)) * fade;
    const bool chosen = a < 0.5f;
    const Game *g = &app.library.games[index];
    PlatTexture *tex = covers_get(index);
    if (chosen)
        draw_glow(x, y, w, h, 46 + 10 * pulse, argb_alpha(TH_FOCUS, (0.22f + 0.10f * pulse) * fade));
    if (tex)
    {
        const uint8_t shade = (uint8_t)(255 * (a <= 1.0f ? 1.0f - 0.45f * a : 0.55f));
        plat_draw_texture(tex, x, y, w, h, argb_alpha(0xff000000u | shade << 16 | shade << 8 | shade, alpha), true);
        draw_reflection(tex, x, y + h + 6, w, h, alpha * (a <= 1.0f ? 1.0f - 0.5f * a : 0.4f));
    }
    else
        draw_placeholder(g, x, y, w, h, alpha, chosen);
    if (chosen)
        draw_rrect_outline(x - 6, y - 6, w + 12, h + 12, 10, 4, argb_alpha(TH_FOCUS, (1.0f - 2.0f * a) * fade));
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

/* The sections' tabs in the middle of the header, between Up and Down as Settings has its tabs between L1 and R1:
 * each section's name and how many games it has. */
static void draw_sections(int section)
{
    static const struct
    {
        const char *name;
        int icon;
    } SECTIONS[SECTION_COUNT] = {[SECTION_RPG_MAKER] = {"RPG Maker", ICON_DEVICE_GAMEPAD_2},
                                 [SECTION_RENPY] = {"Ren'Py", ICON_FILE_TEXT}};
    const float y = 50, h = 52, size = 24, gap = 12, glyph = 26, glyph_w = pad_glyph_width(GLYPH_UP, glyph);
    char counts[SECTION_COUNT][16];
    float widths[SECTION_COUNT], total = 2 * (glyph_w + 16) - gap;
    for (int s = 0; s < SECTION_COUNT; ++s)
    {
        int count;
        library_section_start(&app.library, s, &count);
        snprintf(counts[s], sizeof(counts[s]), "%d", count);
        widths[s] = count ? 58 + text_width(size, FONT_BOLD, tr(SECTIONS[s].name)) + 10 +
                                text_width(size, FONT_REGULAR, counts[s]) + 22
                          : 0;
        total += widths[s] ? widths[s] + gap : 0;
    }
    float x = (plat_width() - total) * 0.5f;
    draw_pad_glyph(GLYPH_UP, x + glyph_w * 0.5f, y + h * 0.5f, glyph);
    x += glyph_w + 16;
    for (int s = 0; s < SECTION_COUNT; ++s)
    {
        if (!widths[s])
            continue;
        const bool on = s == section;
        const char *name = tr(SECTIONS[s].name);
        draw_rrect(x, y, widths[s], h, h * 0.5f, on ? TH_FOCUS : TH_PILL_A(0xd0));
        icon_draw(SECTIONS[s].icon, x + 20, y + 12, 28, on ? 0xffffffffu : TH_TEXT_DIM);
        text_draw(x + 58, y + 12, size, FONT_BOLD, on ? 0xffffffffu : TH_TEXT_SOFT, ALIGN_LEFT, name);
        text_draw(x + 58 + text_width(size, FONT_BOLD, name) + 10, y + 12, size, FONT_REGULAR,
                  on ? 0xd8ffffffu : TH_TEXT_DIM, ALIGN_LEFT, counts[s]);
        x += widths[s] + gap;
    }
    draw_pad_glyph(GLYPH_DOWN, x - gap + 16 + glyph_w * 0.5f, y + h * 0.5f, glyph);
}

/* nightly, the community on Discord, at the foot of the empty library: the first time the empty library is shown, as
 * long as the app stays open, and never again (Settings.community_seen, saved as soon as it is shown) */
static bool invite;

static void invite_once(void)
{
    static bool decided;
    if (decided)
        return;
    decided = true;
    invite = !app.settings.community_seen;
    if (invite)
    {
        app.settings.community_seen = true;
        app_save_settings();
    }
}

static void draw_empty(void)
{
    const float w = 1160, h = invite ? 570 : 470, x = (plat_width() - w) * 0.5f, y = 250;
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
    if (invite)
    {
        /* Join nightly, Dulcamara's community on Discord: discord.gg/...   Its QR code is in Settings > About. */
        char line[200];
        snprintf(line, sizeof(line), tr("Join %s, Dulcamara's community on Discord: %s"), COMMUNITY_NAME,
                 COMMUNITY_LINK);
        draw_rect(x + 56, y + 440, w - 112, 2, TH_DIVIDER);
        text_draw_fit(x + 56, y + 462, 28, FONT_REGULAR, TH_TEXT, ALIGN_LEFT, w - 112, line);
        text_draw_fit(x + 56, y + 506, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w - 112,
                      tr("Its QR code is in Settings > About."));
    }
}

void shelf_screen(uint32_t pressed)
{
    int c = cursor(), first, count;
    int section = row_of(c, &first, &count);
    /* Up and Down: once a press, not at nav_pressed's repeats, and not for the stick pushed along the row askew */
    const PadState *p = &app.pads[app_nav_pad()];
    const bool vertical = (p->buttons & (BIT(BTN_UP) | BIT(BTN_DOWN))) || p->ly < -20000 || p->ly > 20000;
    const bool switches = vertical && !vertical_held && p->lx >= -20000 && p->lx <= 20000;
    vertical_held = vertical;
    if (count > 0)
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
            to = to < first ? first : to >= first + count ? first + count - 1 : to;
            if (to != c)
            {
                c = to;
                app.settings.last_game = c;
                sfx_play(SFX_CLICK);
            }
        }
        if (switches && (pressed & (BIT(BTN_UP) | BIT(BTN_DOWN))) && sections_with_games() > 1)
        {
            /* the other section, at the game last chosen there */
            const int step = pressed & BIT(BTN_UP) ? -1 : 1;
            str_copy(section_game[section], sizeof(section_game[0]), app.library.games[c].id);
            section = next_section(section, step, &c);
            app.settings.last_game = c;
            positioned = false;
            section_in = 0.0f;
            section_from = (float)step;
            sfx_play(SFX_CLICK);
        }
        if (pressed & BIT(BTN_CROSS))
        {
            sfx_play(SFX_SELECT);
            app_start_game(c);
            return;
        }
        if (pressed & BIT(BTN_TRIANGLE))
        {
            /* the order, in each section: by title, or the last played first */
            const char *keep = app.library.games[c].id;
            char id[24];
            str_copy(id, sizeof(id), keep);
            app.settings.sort_mode = !app.settings.sort_mode;
            library_sort(&app.library, app.settings.sort_mode);
            for (int i = 0; i < app.library.count; ++i)
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
    }
    if (pressed & BIT(BTN_START))
    {
        sfx_play(SFX_SELECT);
        app_open_settings(SCREEN_LIBRARY);
        return;
    }

    /* the library as it is now (a look again may have changed it) */
    const int n = app.library.count;
    c = cursor();
    section = row_of(c, &first, &count);
    if (!positioned)
    {
        pos = (float)c;
        positioned = true;
    }
    anim_approach(&pos, (float)c, app.dt, 14.0f);
    anim_approach(&section_in, 1.0f, app.dt, 10.0f);
    pulse = 0.5f + 0.5f * sinf((float)plat_ticks_us() / 1e6f * 2.4f);

    shelf_backdrop();
    draw_header();
    if (n == 0)
    {
        invite_once();
        draw_empty();
    }
    else
    {
        if (sections_with_games() > 1)
            draw_sections(section);
        /* the section's cards, far ones first, the chosen one last, over its neighbours */
        for (int ring = 4; ring >= 0; --ring)
            for (int side = -1; side <= 1; side += 2)
            {
                const int i = c + side * ring;
                if (i < first || i >= first + count || (ring == 0 && side > 0))
                    continue;
                draw_card(i, (float)i - pos);
            }
        const Game *g = &app.library.games[c];
        const float ty = CARD_Y + CARD_W * 0.75f + 72;
        text_draw_fit(plat_width() * 0.5f, ty, 46, FONT_BOLD, argb_alpha(TH_TEXT, section_in), ALIGN_CENTER, 1500,
                      g->title);
        char meta[200], played[64];
        char renpy[16];
        snprintf(renpy, sizeof(renpy), g->engine ? "Ren'Py %d" : "Ren'Py", g->engine);
        const char *engine = core_is_renpy(g->core) ? renpy
                             : g->engine == 2003    ? "RPG Maker 2003"
                             : g->engine == 2000    ? "RPG Maker 2000"
                                                    : "RPG Maker";
        snprintf(meta, sizeof(meta), "%s   \xC2\xB7   %s   \xC2\xB7   %s", engine,
                 g->archive ? tr("Archive") : tr("Folder"), played_text(g->played, played, sizeof(played)));
        text_draw_fit(plat_width() * 0.5f, ty + 66, 26, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, section_in),
                      ALIGN_CENTER, 1500, meta);
        char place[32];
        snprintf(place, sizeof(place), "%d / %d", c - first + 1, count);
        text_draw(plat_width() * 0.5f, ty + 112, 22, FONT_REGULAR, argb_alpha(TH_HINT, section_in), ALIGN_CENTER,
                  place);
    }
    static const int glyphs[] = {GLYPH_CROSS, GLYPH_TRIANGLE, GLYPH_SQUARE, GLYPH_START};
    static const char *const labels[] = {"Play", "Sort", "Look again", "Settings"};
    const int skip = n > 0 ? 0 : 2; /* Play and Sort, without games */
    app_draw_hints(glyphs + skip, labels + skip, 4 - skip,
                   n > 0 ? (app.settings.sort_mode ? "Last played first" : "By title") : NULL);
}
