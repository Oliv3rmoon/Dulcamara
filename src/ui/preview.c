/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "preview.h"
#if defined(NP_PREVIEW)
#include "../app.h"
#include "../update.h"
#include "../platform/platform.h"
#include "text.h"
#include "draw.h"
#include "insider_prompt.h"
#include "update_popup.h"
#include <string.h>
static enum
{
    NONE,
    KEYBOARD,
    INSIDER,
    UPDATE,
    MESSAGE,
    RENPY_MENU
} fixture;
static uint32_t menu_input;
void preview_surface_open(const char *name)
{
    fixture = NONE;
    if (!strcmp(name, "renpy-menu"))
    {
        fixture = RENPY_MENU;
        menu_preview_open();
    }
    if (!strcmp(name, "keyboard"))
    {
        fixture = KEYBOARD;
        keyboard_preview_open();
    }
    if (!strcmp(name, "insider"))
    {
        fixture = INSIDER;
        insider_prompt_fixture();
    }
    if (!strncmp(name, "update", 6))
    {
        fixture = UPDATE;
        update_popup_fixture(!strcmp(name, "update-failed")      ? UPDATE_FAILED
                             : !strcmp(name, "update-progress")  ? UPDATE_INSTALLING
                             : !strcmp(name, "update-installed") ? UPDATE_INSTALLED
                                                                 : UPDATE_AVAILABLE);
    }
    if (!strcmp(name, "message"))
    {
        fixture = MESSAGE;
        app_message_box("Offline preview fixture: a production message panel. Existing game data "
                        "remains unchanged. Cross closes this message.");
    }
    np_log("preview surface fixture: %s (offline; no native RenPy/update execution)", name);
}
void preview_surface_input(uint32_t *p)
{
    if (fixture == RENPY_MENU)
    {
        menu_input = *p;
        if (*p & BIT(BTN_CIRCLE))
            fixture = NONE;
        *p = 0;
    }
    else if (fixture == KEYBOARD)
    {
        keyboard_preview_input(*p);
        if (*p & BIT(BTN_CIRCLE))
            fixture = NONE;
        *p = 0;
    }
    else if (fixture == INSIDER)
    {
        if (!insider_prompt_input(*p) || (*p & BIT(BTN_CIRCLE)))
            fixture = NONE;
        *p = 0;
    }
    else if (fixture == UPDATE)
    {
        update_popup_input(p);
        if (!update_popup_is_open())
            fixture = NONE;
    }
}
void preview_surface_draw(void)
{
    if (fixture == RENPY_MENU)
    {
        menu_preview_draw(menu_input);
        menu_input = 0;
    }
    if (fixture == KEYBOARD)
        keyboard_preview_draw();
    if (fixture == INSIDER)
        insider_prompt_draw();
    if (fixture != NONE)
    {
        plat_fill_rectf(560, 116, 1130, 36, 0xef08080b);
        text_draw(
            580, 123, 21, FONT_BOLD, 0xfff0e0e0, ALIGN_LEFT,
            "OFFLINE PREVIEW FIXTURE | production renderer; no live service or Ren'Py execution");
    }
}
#endif
