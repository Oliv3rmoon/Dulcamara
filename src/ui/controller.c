/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "controller.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
void ui_entry_open(UiEntry *e, const char *value)
{
    kb_reset(&e->keys);
    snprintf(e->value, sizeof(e->value), "%s", value);
}
void ui_entry_clear(UiEntry *e)
{
    e->value[0] = 0;
}
UiEditResult ui_entry_button(UiEntry *e, int button)
{
    uint32_t ch = 0;
    KbAction action = kb_button(&e->keys, button, &ch);
    size_t len = strlen(e->value);
    if (action == KB_RETURN)
        return UI_ACCEPTED;
    if (action == KB_HIDE)
        return UI_CANCELLED;
    if (action == KB_BACKSPACE && len)
    {
        do
        {
            --len;
        } while (len && ((unsigned char)e->value[len] & 0xc0) == 0x80);
        e->value[len] = 0;
    }
    /* The current keyboard emits ASCII; preserve existing UTF-8 queries verbatim. */
    if (action == KB_TYPE && ch >= 32 && ch < 127 && len + 1 < sizeof(e->value))
    {
        e->value[len] = (char)ch;
        e->value[len + 1] = 0;
    }
    return UI_EDITING;
}
void ui_list_move(UiList *l, int delta, int count, int visible)
{
    if (count <= 0)
    {
        l->row = l->top = 0;
        return;
    }
    l->row += delta;
    if (l->row < 0)
        l->row = 0;
    if (l->row >= count)
        l->row = count - 1;
    if (l->top > l->row)
        l->top = l->row;
    if (l->top < l->row - visible + 1)
        l->top = l->row - visible + 1;
    if (l->top > count - visible)
        l->top = count - visible;
    if (l->top < 0)
        l->top = 0;
}
bool ui_save_allowed(bool runtime, bool lifecycle, bool ready)
{
    return !runtime && !lifecycle && ready;
}

void ui_save_timestamp(char *out, size_t size, uint64_t realtime_ns)
{
    time_t seconds = (time_t)(realtime_ns / UINT64_C(1000000000));
    struct tm local;
    if (!localtime_r(&seconds, &local) || !strftime(out, size, "%Y-%m-%d %H:%M:%S", &local))
        snprintf(out, size, "Unknown date");
}
