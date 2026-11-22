/* Production renderer model with counted resources and captured geometry. */
#include "ui/eclipse.h"
#include "ui/draw.h"
#include "platform/platform.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct PlatTexture
{
    int id;
};
static struct PlatTexture tex[32];
static int created, loads, fonts, allocations, draws;
static float first_x, first_y;
void *tracked_malloc(size_t n)
{
    allocations++;
    return malloc(n);
}
void *tracked_calloc(size_t n, size_t z)
{
    allocations++;
    return calloc(n, z);
}
void tracked_free(void *p)
{
    free(p);
}
unsigned char *stbi_load(const char *p, int *w, int *h, int *n, int channels)
{
    (void)p;
    (void)channels;
    loads++;
    *w = *h = 256;
    *n = 4;
    return calloc(256 * 256, 4);
}
void stbi_image_free(void *p)
{
    free(p);
}
void plat_asset_path(char *out, size_t n, const char *p)
{
    snprintf(out, n, "%s", p);
}
PlatTexture *plat_texture_create(const uint8_t *p, int w, int h, bool smooth)
{
    (void)p;
    (void)w;
    (void)h;
    (void)smooth;
    return &tex[created++];
}
void plat_texture_free(PlatTexture *p)
{
    (void)p;
}
int text_render_rgba(uint8_t *p, int w, int h, int y, float size, int weight, uint32_t color,
                     int max, const char *s)
{
    (void)p;
    (void)w;
    (void)h;
    (void)y;
    (void)size;
    (void)weight;
    (void)color;
    (void)max;
    (void)s;
    fonts++;
    return 20;
}
void np_log(const char *format, ...)
{
    (void)format;
}
int plat_width(void)
{
    return 1920;
}
int plat_height(void)
{
    return 1080;
}
void plat_draw_texture(PlatTexture *p, float x, float y, float w, float h, uint32_t tint,
                       bool blend)
{
    (void)p;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)tint;
    (void)blend;
}
void plat_draw_texture_region(PlatTexture *p, int sx, int sy, int sw, int sh, float x, float y,
                              float w, float h, uint32_t tint)
{
    (void)p;
    (void)sx;
    (void)sy;
    (void)sw;
    (void)sh;
    (void)w;
    (void)h;
    (void)tint;
    if (!draws++)
    {
        first_x = x;
        first_y = y;
    }
}
void plat_draw_mesh(PlatTexture *p, const PlatVertex *v, int count, const int *indices, int ic)
{
    (void)p;
    (void)v;
    (void)count;
    (void)indices;
    (void)ic;
}
void plat_fill_rectf(float x, float y, float w, float h, uint32_t color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}
float pad_glyph_width(enum PadGlyph g, float size)
{
    (void)g;
    return size;
}
uint32_t argb_alpha(uint32_t color, float alpha)
{
    return (color & 0xffffff) | ((uint32_t)((color >> 24) * alpha) << 24);
}
static void initialize(void)
{
    created = loads = fonts = allocations = draws = 0;
    eclipse_init();
    assert(!eclipse_glyph(GLYPH_START, 0, 0, 24, 1)); /* use the measured OPTIONS label */
    assert(created == 10 && loads == 9 && fonts == 10);
}
int main(void)
{
    initialize();
    int initial = allocations;
    for (int i = 0; i < 60; i++)
        eclipse_tick(1.0f / 60, false, false);
    eclipse_background(false);
    float x60 = first_x, y60 = first_y;
    for (int i = 0; i < 600; i++)
    {
        eclipse_background(true);
        eclipse_surface(10, 10, 500, 400, 0xffffffff);
        eclipse_status(0, 0, 40);
    }
    assert(allocations == initial && created == 10 && loads == 9 && fonts == 10);
    draws = 0;
    eclipse_tick(1.0f / 60, false, true);
    eclipse_background(false);
    assert(first_x == x60 && first_y == y60);
    eclipse_shutdown();
    initialize();
    for (int i = 0; i < 30; i++)
        eclipse_tick(1.0f / 30, false, false);
    eclipse_background(false);
    assert(fabsf(first_x - x60) < 0.01f && fabsf(first_y - y60) < 0.01f);
    eclipse_tick(0.01f, true, false);
    draws = 0;
    eclipse_background(false);
    float x = first_x;
    for (int i = 0; i < 60; i++)
        eclipse_tick(1.0f / 60, true, false);
    draws = 0;
    eclipse_background(false);
    assert(first_x == x);
    eclipse_shutdown();
    puts("PASS motion: 30/60Hz match, gameplay suspend, still mode, 600 frames zero image "
         "allocations/decodes/uploads/rasterization");
}
