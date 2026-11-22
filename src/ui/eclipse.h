/* Cached generated skin and bounded visual clock. GPL-3.0-or-later */
#ifndef NP_ECLIPSE_H
#define NP_ECLIPSE_H
#include "../nightshade.h"
void eclipse_init(void);
bool eclipse_still(void);
void eclipse_shutdown(void);
void eclipse_tick(float dt, bool still, bool gameplay);
void eclipse_background(bool dim);
bool eclipse_quiet_backing(float x, float y, float w, float h);
bool eclipse_surface(float x, float y, float w, float h, uint32_t tint);
bool eclipse_glyph(int glyph, float cx, float cy, float size, float alpha);
bool eclipse_icon(int icon, float x, float y, float size, uint32_t tint);
void eclipse_cover(float x, float y, float w, float h, float alpha, bool placeholder);
void eclipse_strip(float x, float y, float w, float h);
void eclipse_status(float x, float y, float size);
#endif
