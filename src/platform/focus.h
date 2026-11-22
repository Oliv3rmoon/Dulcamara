/* SDL foreground state, shared by event handling and real event tests. */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_FOCUS_H
#define NP_FOCUS_H
#include <SDL2/SDL.h>
#include <stdbool.h>
static inline bool plat_focus_event(bool background, const SDL_Event *event)
{
    if (event->type == SDL_APP_WILLENTERBACKGROUND || event->type == SDL_APP_DIDENTERBACKGROUND)
        return true;
    if (event->type == SDL_APP_DIDENTERFOREGROUND)
        return false;
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        return true;
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
        return false;
    return background;
}
#endif
