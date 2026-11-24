/*
 * Dulcamara - the game picture's size on screen (plat_draw_game), kept apart so a host check can run it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_GAME_FIT_H
#define NP_GAME_FIT_H

#include <stdbool.h>

/* The picture's size in an out_w x out_h area, keeping `aspect`; src_h: the picture's own height. With integer
 * scale, a multiple of the lines the game has (interlaced and 2x frames count as their 240-line selves). Integer
 * scale is for pixel art: an HD picture (480 lines or more, Ren'Py's) fills the area as without it. Its lines halved
 * to 192 (768), 225 (900) or 150 (600) gave black bars of up to 120 pixels on a 3840x2160 output. */
static inline void game_fit(int out_w, int out_h, float aspect, int src_h, bool integer_scale, int *dw_out,
                            int *dh_out)
{
    int lines = src_h;
    while (lines > 288)
        lines /= 2;
    int dh = out_h, dw = (int)(out_h * aspect + 0.5f);
    if (dw > out_w)
    {
        dw = out_w;
        dh = (int)(out_w / aspect + 0.5f);
    }
    if (integer_scale && src_h < 480)
    {
        int k = dh / (lines > 0 ? lines : 240);
        if (k < 1)
            k = 1;
        dh = lines * k;
        dw = (int)(dh * aspect + 0.5f);
        if (dw > out_w)
            dw = out_w;
    }
    *dw_out = dw;
    *dh_out = dh;
}

/* What the log says of the picture's place on screen, once each time it changes: a tester's log then tells black
 * bars the frontend's sizing makes from ones in the game's own pictures. */
typedef struct
{
    int src_w, src_h, out_w, out_h, x, y, w, h, shader;
    bool integer_scale, smooth;
} GameFitNote;

/* True (and *last updated) when now differs from *last: the line is to be logged. */
static inline bool game_fit_changed(GameFitNote *last, const GameFitNote *now)
{
    const bool same = last->src_w == now->src_w && last->src_h == now->src_h && last->out_w == now->out_w &&
                      last->out_h == now->out_h && last->x == now->x && last->y == now->y && last->w == now->w &&
                      last->h == now->h && last->shader == now->shader &&
                      last->integer_scale == now->integer_scale && last->smooth == now->smooth;
    if (!same)
        *last = *now;
    return !same;
}

#endif
