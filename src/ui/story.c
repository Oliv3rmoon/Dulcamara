/*
 * Dulcamara - Story mode: the library as an old RPG menu window, the chosen game's picture large beside it over the
 * ASCII eclipse, and a visual-novel text box across the bottom that says something true about the game and offers
 * what can be done as choices. It owns no library, save or launch state: selection, sort, search, filters,
 * favourites, details, backups and the launch transition are the shelf's and the panels' (shelf.c, panels.c,
 * backup_panel.c, main.c), called the same way; only the drawing and the pad's mapping differ.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "story.h"
#include "../app.h"
#include "../community.h"
#include "../update.h"
#include "../core/cores.h"
#include "../covers.h"
#include "../platform/platform.h"
#include "draw.h"
#include "eclipse.h"
#include "panels.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The layout, in 1920x1080 screen pixels: the prototype's 1280x720 stage at 1.5, kept above the hint line. */
#define WIN_X 66.0f
#define WIN_Y 160.0f /* below the private-test mark's line (watermark.c, y 116..146) */
#define WIN_W 645.0f
#define WIN_H 540.0f
#define FULL_W 1788.0f /* the status and backups windows, the whole width */
#define ROW_Y0 (WIN_Y + 70.0f)
#define ROW_H 54.0f
#define ROWS 7
#define CG_X 768.0f
#define CG_Y 118.0f
#define CG_W 1086.0f
#define CG_H 582.0f
#define BOX_X 66.0f
#define BOX_Y 744.0f
#define BOX_W 1788.0f
#define BOX_H 222.0f

#define BONE 0xffece7e1u
#define SOFT 0xffbdb8c0u
#define DIM 0xff8c8790u
#define WHITE 0xffffffffu

static StoryLine line;
static float clock_s;         /* visual time for the blinks */
static uint32_t accent = STORY_RED;
static float accent_rgb[3] = {0xe5, 0x48, 0x4d};
static int top;               /* the list's first row */
static int choice;            /* Play, Details, Backups, Favorite */
static bool on_extra;         /* the "N not shown" row */
static char feedback[160], feedback_for[24];
static char counted_id[24];   /* the last backup list the panel read: for the status screen */
static int counted_total = -1, counted_automatic;
static uint64_t counted_serial; /* that list's save job: any later job (play, backup, restore, delete) may change it */

bool story_view(void)
{
    return app.settings.library_view == LIBRARY_VIEW_STORY;
}

/* ---------------------------------------------------------------- small shapes and text */

static void triangle(float x, float y, float size, bool down, uint32_t argb)
{
    PlatVertex v[3];
    if (down)
    {
        v[0] = (PlatVertex){x, y, 0, 0, argb};
        v[1] = (PlatVertex){x + size, y, 0, 0, argb};
        v[2] = (PlatVertex){x + size * 0.5f, y + size * 0.62f, 0, 0, argb};
    }
    else
    {
        v[0] = (PlatVertex){x, y, 0, 0, argb};
        v[1] = (PlatVertex){x + size * 0.62f, y + size * 0.5f, 0, 0, argb};
        v[2] = (PlatVertex){x, y + size, 0, 0, argb};
    }
    plat_draw_mesh(NULL, v, 3, NULL, 0);
}

static void star(float cx, float cy, float r, uint32_t argb)
{
    PlatVertex v[30];
    for (int i = 0; i < 10; ++i)
    {
        const float a0 = -1.5707963f + i * 0.6283185f, a1 = a0 + 0.6283185f;
        const float r0 = i % 2 ? r * 0.45f : r, r1 = i % 2 ? r : r * 0.45f;
        v[i * 3] = (PlatVertex){cx, cy, 0, 0, argb};
        v[i * 3 + 1] = (PlatVertex){cx + cosf(a0) * r0, cy + sinf(a0) * r0, 0, 0, argb};
        v[i * 3 + 2] = (PlatVertex){cx + cosf(a1) * r1, cy + sinf(a1) * r1, 0, 0, argb};
    }
    plat_draw_mesh(NULL, v, 30, NULL, 0);
}

/* Letter-spaced text, one character at a time; returns the width used. */
static float spaced(float x, float y, float size, int font, uint32_t argb, float spacing, const char *s)
{
    float start = x;
    while (*s)
    {
        char one[8];
        int n = 1;
        while (n < 7 && ((unsigned char)s[n] & 0xc0) == 0x80)
            ++n;
        memcpy(one, s, (size_t)n);
        one[n] = 0;
        text_draw(x, y, size, font, argb, ALIGN_LEFT, one);
        x += text_width(size, font, one) + spacing;
        s += n;
    }
    return x - start - spacing;
}

static float spaced_width(float size, int font, float spacing, const char *s)
{
    float w = 0;
    int chars = 0;
    while (*s)
    {
        char one[8];
        int n = 1;
        while (n < 7 && ((unsigned char)s[n] & 0xc0) == 0x80)
            ++n;
        memcpy(one, s, (size_t)n);
        one[n] = 0;
        w += text_width(size, font, one);
        ++chars;
        s += n;
    }
    return w + (chars > 1 ? (chars - 1) * spacing : 0);
}

static void hgradient(float x, float y, float w, float h, uint32_t argb)
{
    const uint32_t clear = argb & 0x00ffffffu;
    const float m = x + w * 0.5f;
    const PlatVertex v[12] = {{x, y, 0, 0, clear},     {m, y, 0, 0, argb},         {m, y + h, 0, 0, argb},
                              {x, y, 0, 0, clear},     {m, y + h, 0, 0, argb},     {x, y + h, 0, 0, clear},
                              {m, y, 0, 0, argb},      {x + w, y, 0, 0, clear},    {x + w, y + h, 0, 0, clear},
                              {m, y, 0, 0, argb},      {x + w, y + h, 0, 0, clear}, {m, y + h, 0, 0, argb}};
    plat_draw_mesh(NULL, v, 12, NULL, 0);
}

/* A soft halo around a box, as rings fading outwards (the CSS box-shadow of the prototype). */
static void halo(float x, float y, float w, float h, float spread, uint32_t argb)
{
    const int rings = 8;
    const float step = spread / rings;
    for (int i = 1; i <= rings; ++i)
    {
        const float o = i * step - step * 0.5f;
        draw_rrect_outline(x - o, y - o, w + 2 * o, h + 2 * o, 6 + o, step,
                           argb_alpha(argb, (1.0f - (float)(i - 1) / rings) * 0.55f));
    }
}

/* Pixel-face measure for the wrap (story_model.c). */
static float pixel_measure(const char *s, size_t bytes, void *context)
{
    char buf[700];
    if (bytes >= sizeof(buf))
        bytes = sizeof(buf) - 1;
    memcpy(buf, s, bytes);
    buf[bytes] = 0;
    return text_width(*(float *)context, FONT_PIXEL, buf);
}

/* "RPG Maker 2003", "Ren'Py 8": as the shelf names a game's engine. */
static void engine_name(const Game *g, int index, char *out, size_t size)
{
    const int detected = g->engine ? g->engine : (index >= 0 ? covers_engine(index) : 0);
    if (core_is_renpy(g->core))
        snprintf(out, size, detected ? "Ren'Py %d" : "Ren'Py", detected);
    else
        snprintf(out, size, "%s", detected == 2003   ? "RPG Maker 2003"
                                  : detected == 2000 ? "RPG Maker 2000"
                                                     : "RPG Maker");
}

/* Data as plain text (story_plain): titles, folder names, paths and errors are never markup. */
static const char *plain(char *buf, size_t size, const char *data)
{
    story_plain(buf, size, data);
    return buf;
}
#define PLAIN(data) plain((char[NP_PATH_MAX * 2 + 8]){0}, NP_PATH_MAX * 2 + 8, (data))

static const GameUserData *user_data(const Game *g)
{
    return user_metadata_find(&app.metadata, g->id);
}

/* ---------------------------------------------------------------- accent (the optional cover tint) */

static void accent_toward(uint32_t target)
{
    const float t = app.settings.still_background ? 1.0f : fminf(1.0f, app.dt * 6.0f);
    const float want[3] = {(float)(target >> 16 & 0xff), (float)(target >> 8 & 0xff), (float)(target & 0xff)};
    for (int k = 0; k < 3; ++k)
        accent_rgb[k] += (want[k] - accent_rgb[k]) * t;
    accent = 0xff000000u | (uint32_t)(accent_rgb[0] + 0.5f) << 16 | (uint32_t)(accent_rgb[1] + 0.5f) << 8 |
             (uint32_t)(accent_rgb[2] + 0.5f);
}

/* A decoded cover's average colour (computed once at decode time, covers.c), else red. */
static uint32_t cover_accent(int index)
{
    return index >= 0 && covers_get(index) ? story_tint(covers_color(index)) : STORY_RED;
}

/* ---------------------------------------------------------------- the frame: header, windows, picture */

static void draw_header(void)
{
    const float cx = 91, cy = 64;
    for (int i = 8; i >= 1; --i)
        draw_circle(cx - 4 + i * 0.6f, cy - 2, 25 + i * 2.2f, argb_alpha(accent, 0.05f));
    draw_circle(cx, cy, 25, 0xff000000);
    draw_ring(cx, cy, 25, 2.2f, argb_alpha(accent, 0.9f));
    spaced(137, 40, 28, FONT_REGULAR, 0xfff2f0ee, 9.5f, "DULCAMARA");
    spaced(139, 78, 16, FONT_MONO, accent, 6.5f, "ECLIPSE \xC2\xB7 " NP_VERSION);

    /* right: games, not shown, the clock */
    char clock[32], text[48];
    plat_clock(clock, sizeof(clock));
    float x = plat_width() - 66;
    const float y = 46, size = 24;
    text_draw(x, y, size, FONT_PIXEL, BONE, ALIGN_RIGHT, clock);
    x -= text_width(size, FONT_PIXEL, clock) + 32;
    if (app.library.skipped_count > 0)
    {
        text_draw(x, y, size, FONT_PIXEL, 0xffb9b5bd, ALIGN_RIGHT, "not shown");
        x -= text_width(size, FONT_PIXEL, "not shown") + 10;
        snprintf(text, sizeof(text), "%d", app.library.skipped_count);
        text_draw(x, y, size, FONT_PIXEL, BONE, ALIGN_RIGHT, text);
        x -= text_width(size, FONT_PIXEL, text) + 32;
    }
    const char *games = app.library.count == 1 ? "game" : "games";
    text_draw(x, y, size, FONT_PIXEL, 0xffb9b5bd, ALIGN_RIGHT, games);
    x -= text_width(size, FONT_PIXEL, games) + 10;
    snprintf(text, sizeof(text), "%d", app.library.count);
    text_draw(x, y, size, FONT_PIXEL, BONE, ALIGN_RIGHT, text);

    /* the scan's progress or error, else an installed update: one line under it */
    char status[NP_PATH_MAX + 128];
    status[0] = 0;
    uint32_t colour = DIM;
    if (app.scan_status.state == LIBRARY_SCAN_RUNNING)
        snprintf(status, sizeof(status), "Scanning: %u checked, %u games", app.scan_status.visited,
                 app.scan_status.found);
    else if (app.scan_status.state == LIBRARY_SCAN_ERROR)
    {
        story_plain(status, sizeof(status), app.scan_status.error);
        colour = TH_DANGER;
    }
    else if (update_state() == UPDATE_INSTALLED)
    {
        snprintf(status, sizeof(status), "Update installed: restart Dulcamara to use it");
        colour = TH_GOOD;
    }
    if (status[0])
        text_draw_fit(plat_width() - 66, 84, 20, FONT_PIXEL, colour, ALIGN_RIGHT, 1000, status);
}

/* The RPG menu window: a double bone border on a dark red ground, its name in the accent. */
static void draw_window(float x, float y, float w, float h, const char *title)
{
    halo(x, y, w, h, 36, 0x80000000);
    draw_rrect_flat(x, y, w, h, 9, 0xf2160509);
    const uint32_t stops[2] = {0xf026080e, 0xf50a0306};
    const float at[2] = {0, 1};
    draw_vgradient(x + 4, y + 4, w - 8, h - 8, stops, at, 2);
    draw_rrect_outline(x - 1.5f, y - 1.5f, w + 3, h + 3, 10, 1.5f, 0xff000000);
    draw_rrect_outline(x, y, w, h, 9, 2.0f, BONE);
    draw_rrect_outline(x + 5, y + 5, w - 10, h - 10, 6, 2.0f, BONE);
    spaced(x + 27, y + 24, 24, FONT_PIXEL, accent, 5.0f, title);
}

/* A picture filling (x, y, w, h) without stretching: dim and cropped behind, whole on top. */
static void draw_picture(PlatTexture *tex, float x, float y, float w, float h, uint32_t tint)
{
    int tw = 0, th = 0;
    plat_texture_size(tex, &tw, &th);
    if (tw <= 0 || th <= 0)
        return;
    int sx = 0, sy = 0, sw = tw, sh = th;
    if ((float)tw / th > w / h)
    {
        sw = (int)(th * w / h);
        sx = (tw - sw) / 2;
    }
    else
    {
        sh = (int)(tw * h / w);
        sy = (th - sh) / 2;
    }
    plat_draw_texture_region(tex, sx, sy, sw, sh, x, y, w, h, 0xff2a2a2a);
    const float s = fminf(w / tw, h / th), dw = tw * s, dh = th * s;
    plat_draw_texture(tex, x + (w - dw) * 0.5f, y + (h - dh) * 0.5f, dw, dh, tint, true);
}

static void draw_tag(float x, float y, const char *text)
{
    char upper[96];
    snprintf(upper, sizeof(upper), "%s", text);
    for (char *c = upper; *c; ++c)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 'a' + 'A');
    const float w = spaced_width(18, FONT_MONO, 2.5f, upper) + 36;
    draw_rrect_flat(x, y, w, 40, 20, 0xc0050506);
    draw_rrect_outline(x, y, w, 40, 20, 1.5f, argb_alpha(accent, 0.7f));
    spaced(x + 18, y + 10, 18, FONT_MONO, WHITE, 2.5f, upper);
}

/* The chosen game's picture, framed and lit in the accent, its engine on a tag. index -1: no game (title only). */
static void draw_cover(int index, const char *title, const char *tag, float x, float y, float w, float h)
{
    halo(x, y, w, h, 64, argb_alpha(accent, 0.30f));
    draw_rrect_outline(x - 8, y - 8, w + 16, h + 16, 8, 7, 0x99000000);
    draw_rect(x, y, w, h, 0xff000000);
    PlatTexture *tex = index >= 0 ? covers_get(index) : NULL;
    if (tex)
        draw_picture(tex, x, y, w, h, WHITE);
    else
    {
        eclipse_cover(x, y, w, h, 1, true);
        if (title && title[0])
            text_draw_fit(x + w * 0.5f, y + h - 120, w > 700 ? 40 : 28, FONT_PIXEL, 0xffeeeeee, ALIGN_CENTER,
                          w - 96, PLAIN(title));
    }
    draw_rrect_outline(x - 1, y - 1, w + 2, h + 2, 5, 2, argb_alpha(accent, 0.7f));
    if (tag && tag[0])
        draw_tag(x + 21, y + h - 58, tag);
}

/* ---------------------------------------------------------------- the text box */

typedef struct
{
    char text[640];
    float size;
    int count;
    size_t starts[3], lengths[3];
    float widths[3]; /* each line's room: the third stops short of the choices */
    bool overflow;
} Wrapped;

/* The line wrapped to the box at the largest size that fits: two lines at 40, else three at 32, the third one
 * beside the choices (cut with an ellipsis if even that is not enough). Cached: the line changes rarely. */
static const Wrapped *wrap_line(const char *text, float width, float last_width)
{
    static Wrapped w;
    static float for_width, for_last;
    if (!strcmp(w.text, text) && for_width == width && for_last == last_width && w.size > 0)
        return &w;
    snprintf(w.text, sizeof(w.text), "%s", text);
    for_width = width;
    for_last = last_width;
    w.size = 40;
    w.widths[0] = w.widths[1] = w.widths[2] = width;
    w.count = story_wrap(w.text, width, pixel_measure, &w.size, w.starts, w.lengths, 2, &w.overflow);
    if (w.overflow)
    {
        w.size = 32;
        w.count = story_wrap(w.text, width, pixel_measure, &w.size, w.starts, w.lengths, 2, &w.overflow);
        if (w.overflow)
        {
            const size_t rest = w.starts[1] + w.lengths[1];
            size_t start, length;
            w.count += story_wrap(w.text + rest, last_width, pixel_measure, &w.size, &start, &length, 1,
                                  &w.overflow);
            w.starts[2] = rest + start;
            w.lengths[2] = length;
            w.widths[2] = last_width;
        }
    }
    return &w;
}

typedef struct
{
    const char *const *labels;
    int count, selected;
    const char *prompt; /* instead of choices: pad glyphs and words ("{Cross} Yes, restore") */
} Choices;

static float choices_width(const Choices *c)
{
    if (c->prompt)
        return text_width(28, FONT_PIXEL, c->prompt);
    float w = 0;
    for (int i = 0; i < c->count; ++i)
        w += text_width(28, FONT_PIXEL, c->labels[i]) + 42 + (i == c->selected ? 22 : 0) + (i ? 12 : 0);
    return w;
}

static void draw_choices(const Choices *c)
{
    float x = BOX_X + BOX_W - 39 - choices_width(c);
    const float y = BOX_Y + BOX_H - 64;
    if (c->prompt)
    {
        text_draw(x, y + 8, 28, FONT_PIXEL, SOFT, ALIGN_LEFT, c->prompt);
        return;
    }
    for (int i = 0; i < c->count; ++i)
    {
        const bool on = i == c->selected;
        const float w = text_width(28, FONT_PIXEL, c->labels[i]) + 42 + (on ? 22 : 0);
        if (on)
        {
            draw_rrect_flat(x, y, w, 46, 10, argb_alpha(accent, 0.2f));
            draw_rrect_outline(x, y, w, 46, 10, 1.5f, accent);
            triangle(x + 19, y + 16, 14, false, accent);
        }
        text_draw(x + 21 + (on ? 22 : 0), y + 8, 28, FONT_PIXEL, on ? WHITE : SOFT, ALIGN_LEFT, c->labels[i]);
        x += w + 12;
    }
}

/* The line typed so far, cut on a character and never inside a {glyph} tag; whole once typed. */
static size_t visible_cut(const char *text, size_t cut)
{
    return story_line_done(&line) ? strlen(text) : story_visible_cut(text, cut);
}

void story_visible_text(char *out, size_t size)
{
    const size_t shown = visible_cut(line.text, story_line_visible_bytes(&line));
    size_t n = 0;
    for (size_t i = 0; i < shown && n + 1 < size; ++i)
        if (line.text[i] != '\x1b')
            out[n++] = line.text[i];
    if (size)
        out[n] = 0;
}

static void draw_textbox(const char *who, const Choices *choices)
{
    draw_rrect_flat(BOX_X, BOX_Y, BOX_W, BOX_H, 21, 0xe6080610);
    draw_rrect_outline(BOX_X, BOX_Y, BOX_W, BOX_H, 21, 1.5f, argb_alpha(accent, 0.75f));
    /* the name tag: who speaks, the game's engine or Dulcamara */
    const float tag_w = text_width(28, FONT_PIXEL, who) + 48;
    draw_rrect_flat(BOX_X + 36, BOX_Y - 28, tag_w, 44, 12, accent);
    text_draw(BOX_X + 60, BOX_Y - 22, 28, FONT_PIXEL, WHITE, ALIGN_LEFT, who);

    const float tx = BOX_X + 45, ty = BOX_Y + 34, width = BOX_W - 90;
    const float beside = choices ? width - choices_width(choices) - 40 : width;
    const Wrapped *w = wrap_line(line.text, width, beside);
    const size_t shown = visible_cut(line.text, story_line_visible_bytes(&line));
    const float lh = w->size == 40 ? 56 : 44;
    float end_x = tx, end_y = ty;
    for (int i = 0; i < w->count; ++i)
    {
        if (w->starts[i] >= shown)
            break;
        char part[640];
        size_t n = w->lengths[i];
        if (w->starts[i] + n > shown)
            n = shown - w->starts[i];
        memcpy(part, w->text + w->starts[i], n);
        part[n] = 0;
        const float y = ty + i * lh;
        if (i == w->count - 1 && w->overflow && story_line_done(&line))
        {
            /* what did not fit: cut with an ellipsis rather than leave the box */
            char rest[640];
            snprintf(rest, sizeof(rest), "%s", w->text + w->starts[i]);
            text_draw_fit(tx, y, w->size, FONT_PIXEL, 0xfff2f0ee, ALIGN_LEFT, w->widths[i], rest);
            end_x = tx + fminf(w->widths[i], text_width(w->size, FONT_PIXEL, rest));
        }
        else
        {
            text_draw(tx, y, w->size, FONT_PIXEL, 0xfff2f0ee, ALIGN_LEFT, part);
            end_x = tx + text_width(w->size, FONT_PIXEL, part);
        }
        end_y = y;
    }
    /* the advance mark, blinking, once the line is out */
    const bool blink = app.settings.still_background || fmodf(clock_s, 0.9f) < 0.45f;
    if (story_line_done(&line) && line.text[0] && blink && end_x + 30 < BOX_X + BOX_W - 20)
        triangle(end_x + 12, end_y + w->size * 0.4f, w->size * 0.5f, true, accent);
    if (choices)
        draw_choices(choices);
}

static void say(const char *text)
{
    story_line_say(&line, text, app.settings.still_background);
}

/* ---------------------------------------------------------------- the library (home) */

static int cursor(void)
{
    const int row = library_view_selection(&app.library_view, &app.library, &app.settings);
    return row < 0 ? 0 : row;
}

static bool filtered(void)
{
    return app.library_filter.search[0] || app.library_filter.favorites_only || app.library_filter.recent_only ||
           app.library_filter.section >= 0;
}

static void draw_list(int c, int n, bool extra)
{
    draw_window(WIN_X, WIN_Y, WIN_W, WIN_H, "LIBRARY");
    if (filtered())
    {
        char active[220];
        snprintf(active, sizeof(active), "%s%s%s%s%s", app.library_filter.search[0] ? "\"" : "",
                 PLAIN(app.library_filter.search), app.library_filter.search[0] ? "\" " : "",
                 app.library_filter.favorites_only ? "Favorites " : "",
                 app.library_filter.recent_only ? "Recent " : "");
        if (app.library_filter.section >= 0)
            snprintf(active + strlen(active), sizeof(active) - strlen(active), "%s",
                     app.library_filter.section == SECTION_RPG_MAKER ? "RPG Maker" : "Ren'Py");
        text_draw_fit(WIN_X + WIN_W - 27, WIN_Y + 26, 20, FONT_PIXEL, SOFT, ALIGN_RIGHT, 330, active);
    }
    const int total = n + (extra ? 1 : 0), at = on_extra ? n : n ? c : -1;
    top = list_top_row(top, at, ROWS, total);
    const bool blink = app.settings.still_background || fmodf(clock_s, 0.8f) < 0.4f;
    for (int r = top; r < total && r < top + ROWS; ++r)
    {
        const float y = ROW_Y0 + (r - top) * ROW_H;
        const bool on = r == at;
        if (on)
            triangle(WIN_X + 30, y + 9, 18, false, blink ? accent : argb_alpha(accent, 0.35f));
        if (r == n)
        {
            char label[48];
            snprintf(label, sizeof(label), "%d not shown", app.library.skipped_count);
            text_draw(WIN_X + 66, y, 32, FONT_PIXEL, on ? WHITE : DIM, ALIGN_LEFT, label);
            text_draw(WIN_X + WIN_W - 30, y + 6, 24, FONT_PIXEL, DIM, ALIGN_RIGHT, "log");
            continue;
        }
        const Game *g = &app.library.games[app.library_view.indices[r]];
        const GameUserData *d = user_data(g);
        char level[24];
        story_level(level, sizeof(level), d ? d->playtime_seconds : 0, g->played);
        const float lw = text_width(24, FONT_PIXEL, level);
        text_draw(WIN_X + WIN_W - 30, y + 6, 24, FONT_PIXEL, DIM, ALIGN_RIGHT, level);
        const bool fav = d && d->favorite;
        const float room = WIN_W - 66 - 30 - lw - 18 - (fav ? 34 : 0);
        const char *title = PLAIN(g->title);
        text_draw_fit(WIN_X + 66, y, 32, FONT_PIXEL, on ? WHITE : SOFT, ALIGN_LEFT, room, title);
        if (fav)
            star(WIN_X + 66 + fminf(room, text_width(32, FONT_PIXEL, title)) + 20, y + 18, 12, accent);
    }
    if (total > ROWS)
    {
        /* where the window is in a long list */
        const float bar_h = ROWS * ROW_H, knob = fmaxf(24, bar_h * ROWS / total);
        const float k = (bar_h - knob) * (float)top / (float)(total - ROWS);
        draw_rect(WIN_X + WIN_W - 14, ROW_Y0, 3, bar_h, 0x30ece7e1);
        draw_rect(WIN_X + WIN_W - 14, ROW_Y0 + k, 3, knob, argb_alpha(accent, 0.9f));
    }
    if (!n && !extra)
        text_draw(WIN_X + 66, ROW_Y0, 32, FONT_PIXEL, DIM, ALIGN_LEFT, filtered() ? "No matches" : "No games");
    draw_rect(WIN_X + 27, WIN_Y + WIN_H - 70, WIN_W - 54, 1.5f, 0x40ece7e1);
    text_draw_fit(WIN_X + 27, WIN_Y + WIN_H - 52, 24, FONT_PIXEL, SOFT, ALIGN_LEFT, WIN_W - 54,
                  "{Up}{Down} choose a game   {Left}{Right} choose what to do");
}

/* The empty library's card in the picture's place, with the shelf's own words. */
static void draw_empty_card(void)
{
    const float x = CG_X, y = CG_Y, w = CG_W, h = CG_H;
    draw_window(x, y, w, h, app.sandboxed ? "CAN'T SEE YOUR GAMES" : app.library.count ? "NO MATCHES" : "NO GAMES YET");
    const char *empty[] = {"Copy each game's folder, the one that holds RPG_RT.ldb and RPG_RT.lmt",
                           "(or a Ren'Py game's folder), into " NP_DATA "/games, over FTP for example.",
                           ".zip and .easyrpg archives of an RPG Maker game work as they are.",
                           "Games that need the RPG Maker run-time package (RTP): put it in",
                           NP_DATA "/rtp/2000 or rtp/2003.",
                           "No games come with Dulcamara: copy your own."};
    char reason[260];
    snprintf(reason, sizeof(reason), "Why: %s.", app.sandbox_reason[0] ? app.sandbox_reason : "not known");
    const char *sandboxed[] = {"Dulcamara could not open its data folder at startup. Check",
                               NP_DATA "/games. Your games and saves are still there.", reason,
                               "Check that the app has access to its data folder.",
                               "Press {Square} to restart and check again."};
    const char *matches[] = {"No game matches the search and filters.", "{L2} edits the search.",
                             "{R2} changes or clears the filters."};
    const char *const *lines = app.sandboxed ? sandboxed : app.library.count ? matches : empty;
    const int count = app.sandboxed ? 5 : app.library.count ? 3 : 6;
    float ly = y + 90;
    for (int i = 0; i < count; ++i, ly += 46)
        text_draw_fit(x + 36, ly, 26, FONT_PIXEL, i == 2 && app.sandboxed ? DIM : SOFT, ALIGN_LEFT, w - 72,
                      lines[i]);
    if (!app.sandboxed && !app.library.count && shelf_invite())
    {
        char invite[200];
        snprintf(invite, sizeof(invite), "Join %s, Dulcamara's community on Discord: %s", COMMUNITY_NAME,
                 COMMUNITY_LINK);
        draw_rect(x + 36, ly + 8, w - 72, 1.5f, 0x40ece7e1);
        text_draw_fit(x + 36, ly + 26, 26, FONT_PIXEL, BONE, ALIGN_LEFT, w - 72, invite);
        text_draw_fit(x + 36, ly + 70, 22, FONT_PIXEL, DIM, ALIGN_LEFT, w - 72, "Its QR code is in Settings > About.");
    }
}

static void draw_home_hints(void)
{
    const int glyphs[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_TRIANGLE, GLYPH_START, GLYPH_L2, GLYPH_R2, GLYPH_TOUCHPAD};
    const char *labels[] = {"Confirm", shelf_square_label(), "Sort", "Settings", "Search", "Filters", "Details"};
    app_draw_hints(glyphs, labels, 7, NULL);
}

static void act(int index, const Game *g)
{
    switch (choice)
    {
    case 0:
        sfx_play(SFX_SELECT);
        app_start_game(index); /* the shelf's Cross: the same transition and checks */
        break;
    case 1:
        sfx_play(SFX_SELECT);
        panels_details(index);
        break;
    case 2:
        sfx_play(SFX_SELECT);
        backup_panel_open_from(index, SCREEN_LIBRARY);
        break;
    default:
    {
        char id[24];
        str_copy(id, sizeof(id), g->id); /* the view is rebuilt under it */
        if (panels_toggle_favorite(id))
        {
            const GameUserData *d = user_metadata_find(&app.metadata, id);
            snprintf(feedback, sizeof(feedback), "%s",
                     d && d->favorite ? "Marked as a favourite. It has a star in the library."
                                      : "Removed from your favourites.");
            str_copy(feedback_for, sizeof(feedback_for), id);
        }
        sfx_play(SFX_CLICK);
        break;
    }
    }
}

void story_screen(uint32_t pressed)
{
    clock_s += app.dt;
    story_line_tick(&line, app.dt, app.settings.still_background);
    if (pressed)
        story_line_finish(&line); /* any button shows the whole line at once; the press still does its job */
    const int n = app.library_view.count;
    const bool extra = app.library.skipped_count > 0;
    on_extra = on_extra && extra; /* only while there is such a row; a search with no match does not move to it */
    if (pressed & BIT(BTN_L2))
    {
        panels_search();
        return;
    }
    if (pressed & BIT(BTN_R2))
    {
        panels_filters();
        return;
    }
    if ((pressed & (BIT(BTN_UP) | BIT(BTN_DOWN))) && (pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT))))
    {
        /* a diagonal in one frame (the stick pushed askew): its stronger axis only, the game or the choice, as the
         * shelf guards its row; the d-pad's (no stick) moves the game */
        const PadState *p = &app.pads[app_nav_pad()];
        pressed &= abs(p->lx) > abs(p->ly) ? ~(BIT(BTN_UP) | BIT(BTN_DOWN)) : ~(BIT(BTN_LEFT) | BIT(BTN_RIGHT));
    }
    int c = cursor();
    if ((pressed & BIT(BTN_MENU)) && n && !on_extra)
    {
        panels_details(app.library_view.indices[c]);
        return;
    }
    /* Up/Down (with the menus' repeat and acceleration), L1/R1 a window at a time */
    int delta = 0;
    if (pressed & BIT(BTN_UP))
        delta -= 1;
    if (pressed & BIT(BTN_DOWN))
        delta += 1;
    if (pressed & BIT(BTN_L1))
        delta -= ROWS;
    if (pressed & BIT(BTN_R1))
        delta += ROWS;
    const int total = n + (extra ? 1 : 0);
    if (delta && total)
    {
        const int from = on_extra ? n : n ? c : -1;
        int to = from + delta;
        to = to < 0 ? 0 : to >= total ? total - 1 : to;
        if (to != from)
        {
            on_extra = to == n;
            if (!on_extra)
            {
                c = to;
                library_view_select(&app.library_view, &app.library, &app.settings, c);
            }
            choice = 0;
            feedback[0] = 0;
            sfx_play(SFX_CLICK);
        }
    }
    if ((pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT))) && n && !on_extra)
    {
        choice = story_choice_move(choice, (pressed & BIT(BTN_LEFT)) ? -1 : 1, 4);
        feedback[0] = 0;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_CIRCLE))
    {
        if (on_extra && n)
            on_extra = false;
        else
            choice = 0;
        feedback[0] = 0;
    }
    if ((pressed & BIT(BTN_CROSS)) && on_extra && n)
    {
        on_extra = false; /* "Back to the list" */
        sfx_play(SFX_CLICK);
    }
    else if ((pressed & BIT(BTN_CROSS)) && n && !on_extra)
    {
        const int index = app.library_view.indices[c];
        const Game *g = &app.library.games[index];
        act(index, g);
        if (app.screen != SCREEN_LIBRARY || choice == 0)
            return; /* another screen, or the launch transition, takes the next frame */
    }
    if ((pressed & BIT(BTN_TRIANGLE)) && n)
        shelf_toggle_sort();
    if (pressed & BIT(BTN_SQUARE))
        shelf_square();
    if (pressed & BIT(BTN_START))
    {
        sfx_play(SFX_SELECT);
        app_open_settings(SCREEN_LIBRARY);
        return;
    }

    /* the library as it is now (a rescan or the sort may have changed it) */
    const int count = app.library_view.count;
    c = cursor();
    if (count == 0 && !extra)
        on_extra = false;
    const int index = count && !on_extra ? app.library_view.indices[c] : -1;
    const Game *g = index >= 0 ? &app.library.games[index] : NULL;
    accent_toward(cover_accent(index));

    eclipse_background(false);
    draw_header();

    char who[48] = "Dulcamara", text[640], log_path[NP_PATH_MAX];
    static const char *const actions[4] = {"Play", "Details", "Backups", "Favorite"};
    const char *labels[4] = {actions[0], actions[1], actions[2], actions[3]};
    static const char *const back[1] = {"Back to the list"};
    Choices choices = {labels, 4, choice, NULL};
    if (g)
    {
        const GameUserData *d = user_data(g);
        engine_name(g, index, who, sizeof(who));
        if (d && d->favorite)
            labels[3] = "Unfavorite";
        draw_cover(index, g->title, who, CG_X, CG_Y, CG_W, CG_H);
        if (feedback[0] && !strcmp(feedback_for, g->id))
        {
            snprintf(who, sizeof(who), "Dulcamara");
            snprintf(text, sizeof(text), "%s", feedback);
        }
        else
            story_home_line(text, sizeof(text), PLAIN(g->title), d ? d->playtime_seconds : 0, g->played,
                            (long long)time(NULL));
    }
    else if (on_extra)
    {
        char label[48];
        snprintf(label, sizeof(label), "%d not shown", app.library.skipped_count);
        draw_cover(-1, label, "not shown", CG_X, CG_Y, CG_W, CG_H);
        path_join(log_path, sizeof(log_path), app.paths.logs, "dulcamara.log");
        story_not_shown_line(text, sizeof(text), app.library.skipped, app.library.skipped_count, log_path);
        choices = (Choices){back, count ? 1 : 0, 0, NULL};
    }
    else
    {
        draw_empty_card();
        choices = (Choices){NULL, 0, 0, NULL};
        if (app.sandboxed)
            snprintf(text, sizeof(text), "Dulcamara couldn't open the games folder at startup. "
                                         "Press {Square} to restart and check again.");
        else if (app.scan_status.state == LIBRARY_SCAN_RUNNING)
            snprintf(text, sizeof(text), "Looking for games...");
        else if (app.scan_status.state == LIBRARY_SCAN_ERROR)
            story_plain(text, sizeof(text), app.scan_status.error);
        else if (app.library.count)
            snprintf(text, sizeof(text), "No game matches. {L2} edits the search; {R2} changes or clears the "
                                         "filters.");
        else
            snprintf(text, sizeof(text), "No games yet. Copy your games into %s/games, then press {Square} to "
                                         "look again.",
                     NP_DATA);
    }
    draw_list(c, count, extra); /* over the picture's halo */
    say(text);
    draw_textbox(who, choices.count || choices.prompt ? &choices : NULL);
    draw_home_hints();
}

/* ---------------------------------------------------------------- details: the status screen */

static void bar(float x, float y, float w, float fraction)
{
    draw_rrect_outline(x, y, w, 21, 4, 3, BONE);
    if (fraction > 0)
        draw_rect(x + 5, y + 5, (w - 10) * fminf(1.0f, fraction), 11, accent);
}

void story_note_backups(const char *game_id, const SavesStatus *status)
{
    if (status->offset != 0)
        return;
    str_copy(counted_id, sizeof(counted_id), game_id);
    counted_serial = status->serial;
    counted_total = (int)status->count;
    counted_automatic = 0;
    const size_t listed = status->count < SAVE_LIST_MAX ? status->count : SAVE_LIST_MAX;
    for (size_t i = 0; i < listed; ++i)
        counted_automatic += status->list[i].kind == SAVE_AUTOMATIC;
}

static bool counted(const char *game_id)
{
    static SavesStatus now; /* large: not on the stack */
    if (counted_total < 0 || strcmp(counted_id, game_id))
        return false;
    saves_poll(&now);
    return now.serial == counted_serial;
}

void story_backup_count_text(const char *game_id, char *out, size_t size)
{
    if (counted(game_id))
        snprintf(out, size, "%d automatic of 5, %d in all", counted_automatic, counted_total);
    else
        snprintf(out, size, "not counted now: open Backups to count them");
}

static void frame_backdrop(void)
{
    if (app.game >= 0)
        app_draw_game(90);
    else
        eclipse_background(false);
}

/* A press on the panels' screens completes the line at once too (they own the presses; a held button shows it). */
static void panel_tick(void)
{
    clock_s += app.dt;
    story_line_tick(&line, app.dt, app.settings.still_background);
    if (app.pads[app_nav_pad()].buttons)
        story_line_finish(&line);
}

void story_draw_status(const StoryStatus *s)
{
    panel_tick();
    accent_toward(cover_accent(s->index));
    frame_backdrop();
    draw_header();
    draw_window(WIN_X, WIN_Y, FULL_W, WIN_H, "STATUS");
    char engine[48];
    engine_name(s->game, s->index, engine, sizeof(engine));
    draw_cover(s->index, s->game->title, engine, WIN_X + 36, WIN_Y + 78, 520, 390);

    const float gx = WIN_X + 610, vx = gx + 240, top_y = WIN_Y + 72, step = 60, size = 30;
    /* play time against the most-played game in the library's history */
    uint64_t most = 1;
    for (size_t i = 0; i < app.metadata.count; ++i)
        if (app.metadata.records[i].playtime_seconds > most)
            most = app.metadata.records[i].playtime_seconds;
    char text[NP_PATH_MAX + 64];
    const char *names[] = {"Name", "Play time", "Last played", "Engine", "Kept as", "Backups", "Location"};
    for (int r = 0; r < 7; ++r)
        text_draw(gx, top_y + r * step, 26, FONT_PIXEL, 0xff9a95a0, ALIGN_LEFT, names[r]);
    float y = top_y;
    const char *title = PLAIN(s->game->title);
    text_draw_fit(vx, y - 2, size, FONT_PIXEL, BONE, ALIGN_LEFT, WIN_X + FULL_W - 40 - vx - (s->favorite ? 40 : 0),
                  title);
    if (s->favorite)
        star(fminf(vx + text_width(size, FONT_PIXEL, title), WIN_X + FULL_W - 80) + 24, y + 16, 13, accent);
    y += step;
    if (s->playtime_seconds)
        story_duration(text, sizeof(text), s->playtime_seconds);
    else
        snprintf(text, sizeof(text), "not started");
    text_draw(vx, y - 2, size, FONT_PIXEL, BONE, ALIGN_LEFT, text);
    bar(vx + 230, y + 4, 520, (float)((double)s->playtime_seconds / (double)most));
    y += step;
    if (s->game->played)
    {
        char when[64], date[48];
        time_t t = (time_t)s->game->played;
        struct tm tm;
        story_when(when, sizeof(when), s->game->played, (long long)time(NULL));
        if (localtime_r(&t, &tm) && strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm))
            snprintf(text, sizeof(text), "%s  (%s)", when, date);
        else
            snprintf(text, sizeof(text), "%s", when);
    }
    else
        snprintf(text, sizeof(text), "never");
    text_draw(vx, y - 2, size, FONT_PIXEL, BONE, ALIGN_LEFT, text);
    y += step;
    text_draw(vx, y - 2, size, FONT_PIXEL, BONE, ALIGN_LEFT, s->engine_text);
    y += step;
    text_draw(vx, y - 2, size, FONT_PIXEL, BONE, ALIGN_LEFT, s->game->archive ? "Archive" : "Folder");
    y += step;
    story_backup_count_text(s->game->id, text, sizeof(text));
    if (counted(s->game->id))
    {
        text_draw(vx, y - 2, size, FONT_PIXEL, BONE, ALIGN_LEFT, text);
        bar(vx + 470, y + 4, 280, counted_automatic / 5.0f);
    }
    else
        text_draw(vx, y - 2, size, FONT_PIXEL, DIM, ALIGN_LEFT, text);
    y += step;
    text_draw_fit(vx, y, 24, FONT_PIXEL, BONE, ALIGN_LEFT, WIN_X + FULL_W - 40 - vx, PLAIN(s->location));
    if (s->location_paged)
        text_draw(vx, y + 36, 24, FONT_PIXEL, SOFT, ALIGN_LEFT, "{L1} {R1} scroll the location");

    snprintf(text, sizeof(text), "%s. %s, kept as %s. Its picture, volume and controls can be set just for this "
                                 "game in Preferences.",
             PLAIN(s->game->title), s->engine_text, s->game->archive ? "an archive" : "a folder");
    say(text);
    const Choices choices = {s->labels, s->rows, s->row, NULL};
    draw_textbox(engine, &choices);
    const int glyphs[] = {GLYPH_CROSS, GLYPH_CIRCLE};
    const char *labels[] = {"Select", "Back"};
    app_draw_hints(glyphs, labels, 2, NULL);
}

/* ---------------------------------------------------------------- backups: the save slots */

static const char *slot_moment(const SaveInfo *s)
{
    if (s->kind == SAVE_MANUAL)
        return "Manual";
    if (s->kind == SAVE_RECOVERY)
        return "Recovery";
    return s->moment == SAVE_MOMENT_EARLY      ? "Early"
           : s->moment == SAVE_MOMENT_POSTSTOP ? "After play"
                                               : "Before play";
}


static void bytes_text(char *out, size_t size, uint64_t bytes)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(out, size, "%.0f KB", bytes / 1024.0);
    else
        snprintf(out, size, "%llu bytes", (unsigned long long)bytes);
}

static void dashed(float x, float y, float w, float h, uint32_t argb)
{
    for (float d = x + 6; d < x + w - 6; d += 18)
    {
        draw_rect(d, y, fminf(9, x + w - 6 - d), 2, argb);
        draw_rect(d, y + h - 2, fminf(9, x + w - 6 - d), 2, argb);
    }
    for (float d = y + 6; d < y + h - 6; d += 18)
    {
        draw_rect(x, d, 2, fminf(9, y + h - 6 - d), argb);
        draw_rect(x + w - 2, d, 2, fminf(9, y + h - 6 - d), argb);
    }
}

static void draw_slots(const StoryBackups *b, int game_index, StorySlots shown)
{
    const float x0 = WIN_X + 33, y = WIN_Y + 76, h = 400, gap = 16, new_w = 230;
    if (shown != STORY_SLOTS_LISTED)
    {
        /* not listed (still looking, or a list, restore, delete or backup failed): New backup, and in the slots'
         * place what happened, never "empty" slots that would read as no backups */
        float x = x0;
        if (b->top == 0)
        {
            const bool on = b->row == 0;
            draw_rrect_flat(x, y, new_w, h, 7, 0x59000000);
            draw_rrect_outline(x, y, new_w, h, 7, 3, on ? accent : 0x59ece7e1);
            draw_rect(x + new_w / 2 - 3, y + 120, 6, 60, on ? WHITE : SOFT);
            draw_rect(x + new_w / 2 - 30, y + 147, 60, 6, on ? WHITE : SOFT);
            text_draw(x + new_w / 2, y + 220, 28, FONT_PIXEL, on ? WHITE : BONE, ALIGN_CENTER, "New backup");
            x += new_w + gap;
        }
        const float w = WIN_X + FULL_W - 33 - x;
        if (shown == STORY_SLOTS_FAILED)
        {
            draw_rrect_outline(x, y, w, h, 7, 3, argb_alpha(TH_DANGER, 0.8f));
            text_draw(x + 36, y + 110, 32, FONT_PIXEL, BONE, ALIGN_LEFT, "The backups are not listed now");
            text_draw_fit(x + 36, y + 170, 28, FONT_PIXEL, TH_DANGER, ALIGN_LEFT, w - 72, PLAIN(b->error));
            text_draw(x + 36, y + 240, 28, FONT_PIXEL, SOFT, ALIGN_LEFT, "{Triangle} Try again");
        }
        else
            text_draw(x + 36, y + 110, 32, FONT_PIXEL, SOFT, ALIGN_LEFT, "Looking at the backups...");
        return;
    }
    /* with New backup in view: it and five slots; scrolled past it: six slots */
    const float slot_w = b->top == 0 ? (FULL_W - 66 - new_w - 5 * gap) / 5 : (FULL_W - 66 - 5 * gap) / 6;
    float x = x0;
    int slots_drawn = 0;
    for (int row = b->top; row < b->top + STORY_SLOT_CELLS && (row <= b->listed || slots_drawn < 5); ++row)
    {
        const bool on = row == b->row;
        if (row == 0)
        {
            draw_rrect_flat(x, y, new_w, h, 7, 0x59000000);
            draw_rrect_outline(x, y, new_w, h, 7, 3, on ? accent : 0x59ece7e1);
            if (on)
                halo(x, y, new_w, h, 26, argb_alpha(accent, 0.45f));
            draw_rect(x + new_w / 2 - 3, y + 120, 6, 60, on ? WHITE : SOFT);
            draw_rect(x + new_w / 2 - 30, y + 147, 60, 6, on ? WHITE : SOFT);
            text_draw(x + new_w / 2, y + 220, 28, FONT_PIXEL, on ? WHITE : BONE, ALIGN_CENTER, "New backup");
            text_draw_fit(x + new_w / 2, y + 262, 24, FONT_PIXEL, SOFT, ALIGN_CENTER, new_w - 24,
                          b->available ? "the saves as they are" : "stop the game first");
            x += new_w + gap;
            continue;
        }
        ++slots_drawn;
        if (row > b->listed)
        {
            dashed(x, y, slot_w, h, 0x59ece7e1);
            text_draw(x + slot_w / 2, y + h / 2 - 16, 26, FONT_PIXEL, 0xff6a6570, ALIGN_CENTER, "empty");
            x += slot_w + gap;
            continue;
        }
        const SaveInfo *info = &b->status->list[row - 1];
        draw_rrect_flat(x, y, slot_w, h, 7, 0x59000000);
        if (on)
            halo(x, y, slot_w, h, 26, argb_alpha(accent, 0.5f));
        draw_rrect_outline(x, y, slot_w, h, 7, 3, on ? accent : 0x59ece7e1);
        PlatTexture *tex = game_index >= 0 ? covers_get(game_index) : NULL;
        if (tex)
            draw_picture(tex, x + 15, y + 15, slot_w - 30, 170, on ? 0xffd0d0d0 : 0xff9a9a9a);
        else
            eclipse_cover(x + 15, y + 15, slot_w - 30, 170, on ? 1.0f : 0.7f, true);
        char text[64], size_text[32];
        snprintf(text, sizeof(text), "Slot %zu", b->page_offset + (size_t)row);
        text_draw(x + 15, y + 200, 28, FONT_PIXEL, BONE, ALIGN_LEFT, text);
        story_slot_stamp(text, sizeof(text), info->created, false);
        text_draw_fit(x + 15, y + 244, 24, FONT_PIXEL, on ? WHITE : SOFT, ALIGN_LEFT, slot_w - 30, text);
        bytes_text(size_text, sizeof(size_text), info->bytes);
        snprintf(text, sizeof(text), "%s \xC2\xB7 %s", slot_moment(info), size_text);
        text_draw_fit(x + 15, y + 284, 24, FONT_PIXEL, SOFT, ALIGN_LEFT, slot_w - 30, text);
        text_draw_fit(x + 15, y + 320, 24, FONT_PIXEL, info->shared ? accent : SOFT, ALIGN_LEFT, slot_w - 30,
                      info->shared ? "Shared location" : "Used by this game");
        x += slot_w + gap;
    }
}

/* The confirmation's facts, as the panel states them: when, what, the recovery copy, shared effects, locations. */
static void draw_confirmation(const StoryBackups *b)
{
    const SavesStatus *st = b->status;
    const float x = WIN_X + 36, w = FULL_W - 72;
    float y = WIN_Y + 76;
    char stamp[48], size_text[32], text[NP_PATH_MAX + 128];
    story_slot_stamp(stamp, sizeof(stamp), st->detail.created, true);
    bytes_text(size_text, sizeof(size_text), st->detail.bytes);
    snprintf(text, sizeof(text), "Slot %zu  \xC2\xB7  %s  \xC2\xB7  %s  \xC2\xB7  %s", b->page_offset + (size_t)b->row,
             stamp, slot_moment(&st->detail), size_text);
    text_draw_fit(x, y, 32, FONT_PIXEL, BONE, ALIGN_LEFT, w, text);
    y += 54;
    const char *notes[] = {
        b->deleting ? "This deletes this snapshot. The game's current saves are unchanged."
                    : "A verified recovery copy of the current native saves is created before replacement.",
        "Shared save locations may affect other games. Other games can also use an associated location.",
        core_is_renpy(b->game->core) ? "Only listed native locations are covered; custom Python file writes are "
                                       "outside this backup."
                                     : "Only the listed native save locations are covered by this snapshot."};
    for (int i = 0; i < 3; ++i, y += 40)
        text_draw_fit(x, y, 24, FONT_PIXEL, i ? SOFT : WHITE, ALIGN_LEFT, w, notes[i]);
    y += 10;
    const int count = (int)st->layout.count;
    for (int i = b->root_row; i < count && i < b->root_row + 3; i++, y += 66)
    {
        snprintf(text, sizeof(text), "Save location %d / %d: %s, %s", i + 1, count,
                 st->layout.entries[i].scope == DR_SAVE_SHARED ? "SHARED" : "used by this game",
                 backup_scope_kind(st->layout.entries[i].kind));
        text_draw_fit(x, y, 24, FONT_PIXEL, st->layout.entries[i].scope == DR_SAVE_SHARED ? accent : BONE,
                      ALIGN_LEFT, w, text);
        int start = b->root_column * 100;
        const char *path = st->layout.entries[i].path;
        if (start >= (int)strlen(path))
            start = 0;
        while (start > 0 && ((unsigned char)path[start] & 0xc0) == 0x80)
            start--;
        text_draw_fit(x + 24, y + 30, 24, FONT_PIXEL, SOFT, ALIGN_LEFT, w - 24, PLAIN(path + start));
    }
    text_draw_fit(x, WIN_Y + WIN_H - 52, 24, FONT_PIXEL, SOFT, ALIGN_LEFT, w,
                  "{Up}{Down} scroll locations   {L1}{R1} scroll long paths");
}

void story_draw_backups(const StoryBackups *b)
{
    panel_tick();
    /* the panel holds its own copy of the game; find its index for the picture by its id, on this snapshot */
    int index = -1;
    for (int i = 0; i < app.library_view.count && index < 0; ++i)
        if (!strcmp(app.library.games[app.library_view.indices[i]].id, b->game->id))
            index = app.library_view.indices[i];
    accent_toward(cover_accent(index));
    frame_backdrop();
    draw_header();
    draw_window(WIN_X, WIN_Y, FULL_W, WIN_H, b->confirmation ? (b->deleting ? "DELETE?" : "RESTORE?") : "BACKUPS");
    const char *title = PLAIN(b->game->title);
    text_draw_fit(WIN_X + FULL_W - 30, WIN_Y + 26, 20, FONT_PIXEL, SOFT, ALIGN_RIGHT, 1100, title);

    char text[640], stamp[48];
    Choices choices = {NULL, 0, 0, NULL};
    const StorySlots shown = story_slots_state(b->list_ok, b->error);
    if (b->confirmation)
    {
        draw_confirmation(b);
        story_slot_stamp(stamp, sizeof(stamp), b->status->detail.created, false);
        if (b->deleting)
            snprintf(text, sizeof(text), "Delete slot %zu, from %s? The game's current saves are not changed.",
                     b->page_offset + (size_t)b->row, stamp);
        else
            snprintf(text, sizeof(text), "Restore slot %zu, from %s? Your current save is kept as a recovery copy.",
                     b->page_offset + (size_t)b->row, stamp);
        if (b->error && b->error[0])
            story_plain(text, sizeof(text), b->error);
        choices.prompt = b->confirm_armed ? (b->deleting ? "{Cross} Yes, delete   {Circle} No"
                                                         : "{Cross} Yes, restore   {Circle} No")
                                          : "Let go of {Cross}, then press it again   {Circle} No";
    }
    else if (b->working)
    {
        const SavesStatus *st = b->status;
        eclipse_status(WIN_X + 60, WIN_Y + 110, 70);
        char progress[96];
        snprintf(progress, sizeof(progress), "%s: %llu / %llu",
                 st->phase == MT_APPLYING   ? "Applying"
                 : st->phase == MT_ROLLBACK ? "Recovering originals"
                                            : "Preparing",
                 (unsigned long long)st->done, (unsigned long long)st->total);
        text_draw(WIN_X + 160, WIN_Y + 124, 32, FONT_PIXEL, BONE, ALIGN_LEFT, progress);
        /* the figures are drawn above, not typed: an update must not restart the line */
        snprintf(text, sizeof(text), "Working on it. {Options} cancels safely; {Circle} goes back while it finishes.");
        choices.prompt = "{Options} Cancel   {Circle} Back";
    }
    else
    {
        draw_slots(b, index, shown);
        if (shown == STORY_SLOTS_FAILED)
        {
            const size_t n = strlen(b->error);
            snprintf(text, sizeof(text), "%s%s The backups are not shown until they are listed again: {Triangle} "
                                         "tries.",
                     PLAIN(b->error), strchr(".!?", b->error[n - 1]) ? "" : ".");
        }
        else if (b->error && b->error[0])
            story_plain(text, sizeof(text), b->error);
        else if (shown == STORY_SLOTS_LOOKING)
            snprintf(text, sizeof(text), "Looking at the backups of \"%s\"...", title);
        else if (b->status->count == 0)
            snprintf(text, sizeof(text), "%s",
                     b->automatic_on
                         ? "No backups yet. Automatic backups are on for this game: one is made before and after "
                           "each session when the save has changed, and the last five are kept."
                         : "No backups yet. Automatic backups are off for this game, so none are made when you "
                           "play. New backup makes one now.");
        else if (b->row == 0)
            snprintf(text, sizeof(text), b->available ? "A new backup keeps the saves of \"%s\" as they are now."
                                                      : "A new backup of \"%s\" can be made once the game has "
                                                        "stopped safely.",
                     title);
        else
            snprintf(text, sizeof(text), "These are the backups of \"%s\", the newest first.", title);
        if (shown == STORY_SLOTS_FAILED)
            choices.prompt = b->row == 0 ? "{Cross} Make a backup   {Triangle} Try again   {Circle} Back"
                                         : "{Triangle} Try again   {Circle} Back";
        else
            choices.prompt = b->row == 0 ? "{Cross} Make a backup   {Circle} Back"
                                         : "{Cross} Restore   {Square} Delete   {Circle} Back";
    }
    say(text);
    if (b->confirmation)
        story_line_finish(&line); /* a question that destroys or replaces is shown whole, never typed */
    draw_textbox("Dulcamara", &choices);
    if (b->confirmation)
    {
        int g[] = {GLYPH_CROSS, GLYPH_CIRCLE};
        const char *l[] = {b->confirm_armed ? "Confirm" : "Release buttons", "Cancel"};
        app_draw_hints(g, l, 2, NULL);
    }
    else
    {
        int g[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_TRIANGLE, GLYPH_CIRCLE};
        const char *l[] = {"Create / restore", "Delete snapshot", "Refresh", "Back"};
        app_draw_hints(g, l, 4, NULL);
    }
}

/* ---------------------------------------------------------------- the chapter card */

void story_chapter_draw(int index, const char *label)
{
    if (index < 0 || index >= app.library.count)
        return;
    const Game *g = &app.library.games[index];
    const GameUserData *d = user_data(g);
    char engine[48], kicker[160], where[200];
    engine_name(g, index, engine, sizeof(engine));
    story_chapter_lines(kicker, sizeof(kicker), where, sizeof(where), engine, d ? d->playtime_seconds : 0, g->played,
                        (long long)time(NULL));
    const float W = plat_width(), H = plat_height();
    plat_fill_rectf(0, 0, W, H, 0xff000000);
    const uint32_t tint = cover_accent(index);
    /* the title, wrapped to two lines at most, centred */
    float size = 64;
    size_t starts[2], lengths[2];
    bool overflow;
    char title[sizeof(g->title) * 2];
    story_plain(title, sizeof(title), g->title);
    int lines = story_wrap(title, 1500, pixel_measure, &size, starts, lengths, 2, &overflow);
    const float block = 36 + 30 + lines * 80 + 30 + 40;
    float y = (H - block) * 0.5f - 20;
    const float kw = spaced_width(20, FONT_MONO, 6, kicker);
    spaced((W - kw) * 0.5f, y, 20, FONT_MONO, tint, 6, kicker);
    y += 60;
    for (int i = 0; i < lines; ++i, y += 80)
    {
        char part[sizeof(title)];
        size_t n = lengths[i] < sizeof(part) - 1 ? lengths[i] : sizeof(part) - 1;
        memcpy(part, title + starts[i], n);
        part[n] = 0;
        if (i == lines - 1 && overflow)
        {
            char rest[sizeof(title)];
            snprintf(rest, sizeof(rest), "%s", title + starts[i]);
            text_draw_fit(W * 0.5f, y, size, FONT_PIXEL, 0xfff2f0ee, ALIGN_CENTER, 1500, rest);
        }
        else
            text_draw(W * 0.5f, y, size, FONT_PIXEL, 0xfff2f0ee, ALIGN_CENTER, part);
    }
    y += 22;
    hgradient(W * 0.5f - 165, y, 330, 3, tint);
    y += 30;
    text_draw_fit(W * 0.5f, y, 32, FONT_PIXEL, 0xff9a95a0, ALIGN_CENTER, 1500, where);
    /* the transition's own state, small: what the app is doing now */
    char state[96];
    snprintf(state, sizeof(state), "%s", label);
    for (char *c = state; *c; ++c)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 'a' + 'A');
    const float sw = spaced_width(24, FONT_MONO, 5, state);
    eclipse_status((W - sw) * 0.5f - 64, 908, 48);
    spaced((W - sw) * 0.5f, 920, 24, FONT_MONO, SOFT, 5, state);
}
