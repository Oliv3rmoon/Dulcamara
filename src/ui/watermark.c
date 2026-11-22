/* Generated inputs validated by tools/gen-watermark.py. GPL-3.0-or-later */
#include "watermark.h"
#include "eclipse.h"
#include "watermark_config.h"
#include "draw.h"
#include "text.h"
#include "../platform/platform.h"
void watermark_draw(void)
{
    const char *label = NP_WATERMARK_TEXT;
    if (!label[0])
        return;
    float w = text_width(18, FONT_BOLD, label) + 32;
    float x = 60, y = 116;
    if (!eclipse_quiet_backing(x, y, w, 30))
        plat_fill_rectf(x, y, w, 30, 0xe808080b);
    text_draw(x + 16, y + 4, 18, FONT_BOLD, 0xffe2dadd, ALIGN_LEFT, label);
}
