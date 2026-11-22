/* ImageGen source components, one decode/upload each. GPL-3.0-or-later */
#include "eclipse.h"
#include "draw.h"
#include "icons.h"
#include "text.h"
#include "../platform/platform.h"
#include "stb_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum
{
    BACKGROUND,
    PANEL,
    BUTTON,
    FRAME,
    PLACEHOLDER,
    CONTROLLERS,
    ICONS,
    STRIP,
    LOADING,
    ASSETS
};
static PlatTexture *textures[ASSETS], *glyphs;
static int widths[ASSETS], heights[ASSETS];
static double visual_time;
static bool still_mode;
static const char *files[ASSETS] = {"backgrounds/eclipse-ascii-imagegen.png",
                                    "ui/eclipse/panel.png",
                                    "ui/eclipse/button.png",
                                    "ui/eclipse/cover-frame.png",
                                    "ui/eclipse/placeholder-cover.png",
                                    "ui/eclipse/controller-atlas.png",
                                    "ui/eclipse/icon-atlas.png",
                                    "ui/eclipse/chrome-strip.png",
                                    "ui/eclipse/loading-ring.png"};
void eclipse_init(void)
{
    visual_time = 0;
    still_mode = false;
    size_t bytes = 0;
    for (int i = 0; i < ASSETS; i++)
    {
        char path[NP_PATH_MAX];
        plat_asset_path(path, sizeof(path), files[i]);
        int w, h, n;
        unsigned char *p = stbi_load(path, &w, &h, &n, 4);
        if (!p)
        {
            np_log("eclipse: missing %s", files[i]);
            continue;
        }
        /* Bounded working resolution. Box filtering preserves alpha, no frame allocations. */
        int step = i == BACKGROUND ? 2 : (i == LOADING ? 4 : 2);
        int ow = w / step, oh = h / step;
        unsigned char *small = malloc((size_t)ow * oh * 4);
        if (small)
        {
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++)
                    for (int c = 0; c < 4; c++)
                    {
                        unsigned sum = 0;
                        for (int dy = 0; dy < step; dy++)
                            for (int dx = 0; dx < step; dx++)
                                sum += p[((y * step + dy) * w + x * step + dx) * 4 + c];
                        small[(y * ow + x) * 4 + c] = (unsigned char)(sum / (step * step));
                    }
            textures[i] = plat_texture_create(small, ow, oh, true);
            widths[i] = ow;
            heights[i] = oh;
            bytes += (size_t)ow * oh * 4;
            free(small);
        }
        stbi_image_free(p);
    }
    /* Dedicated fixed atlas: rasterized once, independent of text cache eviction/languages. */
    unsigned char *atlas = calloc(256 * 32, 4);
    if (atlas)
    {
        const char *alphabet = "01.:+*#<>/";
        for (int i = 0; i < 10; i++)
        {
            unsigned char tile[24 * 32 * 4] = {0};
            char c[2] = {alphabet[i], 0};
            text_render_rgba(tile, 24, 32, 0, 20, FONT_MONO, 0xffffffff, 24, c);
            for (int y = 0; y < 32; y++)
                memcpy(atlas + (y * 256 + i * 24) * 4, tile + y * 24 * 4, 24 * 4);
        }
        glyphs = plat_texture_create(atlas, 256, 32, true);
        free(atlas);
        bytes += 256 * 32 * 4;
    }
    np_log("eclipse: cached %d image slots + fixed 10-glyph atlas, %zu RGBA bytes (CPU plus "
           "GPU/SDL copies); no per-frame image allocations",
           ASSETS, bytes);
}
void eclipse_shutdown(void)
{
    for (int i = 0; i < ASSETS; i++)
    {
        plat_texture_free(textures[i]);
        textures[i] = NULL;
    }
    plat_texture_free(glyphs);
    glyphs = NULL;
}
bool eclipse_still(void)
{
    return still_mode;
}
void eclipse_tick(float dt, bool still, bool gameplay)
{
    still_mode = still;
    if (!still && !gameplay && dt > 0 && dt <= 0.1f)
        visual_time += dt;
}
static void slice(int i, int sx, int sy, int sw, int sh, float x, float y, float w, float h,
                  float border, uint32_t tint)
{
    if (!textures[i])
        return;
    int b = sw / 12;
    if (b > sh / 6)
        b = sh / 6;
    float bx = fminf(border, w / 2), by = fminf(border, h / 2);
    int xx[4] = {sx, sx + b, sx + sw - b, sx + sw}, yy[4] = {sy, sy + b, sy + sh - b, sy + sh};
    float dx[4] = {x, x + bx, x + w - bx, x + w}, dy[4] = {y, y + by, y + h - by, y + h};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            plat_draw_texture_region(textures[i], xx[c], yy[r], xx[c + 1] - xx[c],
                                     yy[r + 1] - yy[r], dx[c], dy[r], dx[c + 1] - dx[c],
                                     dy[r + 1] - dy[r], tint);
}
bool eclipse_quiet_backing(float x, float y, float w, float h)
{
    if (!textures[PANEL])
        return false;
    /* Quiet, opaque interior of the generated panel; no decorative border. */
    plat_draw_texture_region(textures[PANEL], widths[PANEL] / 3, heights[PANEL] / 3,
                             widths[PANEL] / 3, heights[PANEL] / 3, x, y, w, h, 0xff707070);
    return true;
}
bool eclipse_surface(float x, float y, float w, float h, uint32_t tint)
{
    if (w < 80 || h < 40 || !textures[PANEL])
        return false;
    int i = w > 400 && h > 180 ? PANEL : BUTTON;
    int sx = i == PANEL ? 25 : 38, sy = i == PANEL ? 31 : 42;
    int sw = i == PANEL ? 673 : 550, sh = i == PANEL ? 478 : 546;
    /* Keep the source black/red palette; caller controls opacity and selected glow. */
    uint32_t a = tint & 0xff000000u;
    slice(i, sx, sy, sw, sh, x, y, w, h, i == PANEL ? 32 : 16, a | 0xffffff);
    if ((tint & 0xff0000u) > 0x600000u && (tint & 0xff00u) < 0x6000u)
    {
        float pulse = still_mode ? 1.0f : 0.8f + 0.2f * sinf((float)visual_time * 3);
        plat_fill_rectf(x + 8, y + 8, 3, h - 16, argb_alpha(0xffff2546, pulse));
        plat_fill_rectf(x + 12, y + 8, w - 24, h - 16, argb_alpha(0x30ff1538, pulse));
    }
    return true;
}
void eclipse_background(bool dim)
{
    float t = still_mode ? 0 : (float)visual_time;
    float z = 1.02f + 0.008f * sinf(t * 0.15f), w = plat_width() * z, h = plat_height() * z;
    if (textures[BACKGROUND])
        plat_draw_texture(textures[BACKGROUND], (plat_width() - w) / 2, (plat_height() - h) / 2, w,
                          h, 0xffaaaaaa, true);
    else
        plat_fill_rectf(0, 0, plat_width(), plat_height(), 0xff08090b);
    if (glyphs)
        for (int i = 0; i < 64; i++)
        {
            float u = (float)i / 64, angle = u * 6.2831853f + t * (0.015f + (i % 3) * 0.004f);
            float radius = 360 + (i % 7) * 42;
            float x = 960 + cosf(angle) * radius * 1.9f, y = 530 + sinf(angle) * radius * 0.85f;
            if (i % 8 == 0)
            {
                float px = 960 + cosf(angle - .08f) * radius * 1.9f,
                      py = 530 + sinf(angle - .08f) * radius * .85f;
                PlatVertex trail[6] = {
                    {px, py - 1, 0, 0, 0x00ff2446}, {x, y - 1, 0, 0, 0x55ff2446},
                    {x, y + 1, 0, 0, 0x55ff2446},   {px, py - 1, 0, 0, 0x00ff2446},
                    {x, y + 1, 0, 0, 0x55ff2446},   {px, py + 1, 0, 0, 0x00ff2446}};
                plat_draw_mesh(NULL, trail, 6, NULL, 0);
            }
            int g = (i + (int)(t * 0.6f)) % 10;
            float a = 0.22f + 0.15f * sinf(t * 0.6f + i);
            plat_draw_texture_region(glyphs, g * 24, 0, 24, 32, x, y, 18, 24,
                                     argb_alpha(0xffff3552, a));
        }
    if (dim)
        plat_fill_rectf(0, 0, plat_width(), plat_height(), 0x98000000);
}
void eclipse_cover(float x, float y, float w, float h, float alpha, bool placeholder)
{
    int i = placeholder ? PLACEHOLDER : FRAME;
    if (!textures[i])
        return;
    if (placeholder)
        plat_draw_texture(textures[i], x, y, w, h, argb_alpha(0xffffffff, alpha), true);
    else
        slice(i, 23, 25, 677, 493, x - 9, y - 9, w + 18, h + 18, 28, argb_alpha(0xffffffff, alpha));
}
static void sprite(int i, int cell, int rows, float x, float y, float w, float h, uint32_t tint)
{
    if (!textures[i])
        return;
    int col = cell % 4, row = cell / 4;
    int sx = widths[i] * col / 4, sy = heights[i] * row / rows;
    int sw = widths[i] * (col + 1) / 4 - sx, sh = heights[i] * (row + 1) / rows - sy;
    /* Atlas cells have quiet black margins: crop the center for legible small marks. */
    int px = i == CONTROLLERS ? 0 : sw / 8, py = i == CONTROLLERS ? 0 : sh / 8;
    plat_draw_texture_region(textures[i], sx + px, sy + py, sw - 2 * px, sh - 2 * py, x, y, w, h,
                             tint);
}
bool eclipse_glyph(int g, float cx, float cy, float size, float alpha)
{
    int c = -1;
    switch (g)
    {
    case GLYPH_CROSS:
        c = 0;
        break;
    case GLYPH_CIRCLE:
        c = 1;
        break;
    case GLYPH_SQUARE:
        c = 2;
        break;
    case GLYPH_TRIANGLE:
        c = 3;
        break;
    case GLYPH_L1:
        c = 4;
        break;
    case GLYPH_L2:
        c = 5;
        break;
    case GLYPH_R1:
        c = 6;
        break;
    case GLYPH_R2:
        c = 7;
        break;
    case GLYPH_START:
        c = 8;
        break;
    case GLYPH_TOUCHPAD:
        c = 9;
        break;
    default:
        break;
    }
    if (c < 0 || !textures[CONTROLLERS])
        return false;
    float w = size * 1.25f;
    sprite(CONTROLLERS, c, 3, cx - w / 2, cy - size * .625f, w, size * 1.25f,
           argb_alpha(0xffffffff, alpha));
    return true;
}
bool eclipse_icon(int icon, float x, float y, float size, uint32_t tint)
{
    int c = -1;
    switch (icon)
    {
    case ICON_SEARCH:
        c = 0;
        break;
    case ICON_STAR:
    case ICON_HEART:
        c = 1;
        break;
    case ICON_CLOCK:
    case ICON_HISTORY:
        c = 2;
        break;
    case ICON_CATEGORY:
        c = 3;
        break;
    case ICON_SETTINGS:
    case ICON_ADJUSTMENTS:
        c = 4;
        break;
    case ICON_DEVICE_TV:
    case ICON_PHOTO:
        c = 5;
        break;
    case ICON_VOLUME:
        c = 6;
        break;
    case ICON_DEVICE_GAMEPAD_2:
        c = 7;
        break;
    case ICON_FOLDER:
        c = 8;
        break;
    case ICON_DEVICE_FLOPPY:
        c = 9;
        break;
    case ICON_ROTATE:
        c = 10;
        break;
    case ICON_PLAYER_PLAY:
        c = 11;
        break;
    case ICON_INFO_CIRCLE:
        c = 12;
        break;
    case ICON_ALERT_TRIANGLE:
        c = 13;
        break;
    case ICON_DOWNLOAD:
        c = 15;
        break;
    default:
        break;
    }
    if (c < 0 || !textures[ICONS])
        return false;
    sprite(ICONS, c, 4, x, y, size, size, (tint & 0xff000000u) | 0xffffff);
    return true;
}
void eclipse_strip(float x, float y, float w, float h)
{
    slice(STRIP, 13, 149, 1059, 63, x, y, w, h, 24, 0xddffffff);
}
void eclipse_status(float x, float y, float size)
{
    if (!textures[LOADING])
        return;
    float a = still_mode ? 0 : (float)visual_time * 1.3f, cs = cosf(a), sn = sinf(a);
    PlatVertex v[6];
    const int corners[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++)
    {
        int c = corners[i];
        float u = c == 1 || c == 2 ? 1 : 0, q = c >= 2 ? 1 : 0;
        float dx = (u - .5f) * size, dy = (q - .5f) * size;
        v[i] = (PlatVertex){x + size / 2 + dx * cs - dy * sn, y + size / 2 + dx * sn + dy * cs, u,
                            q, 0xffffffff};
    }
    plat_draw_mesh(textures[LOADING], v, 6, NULL, 0);
}
