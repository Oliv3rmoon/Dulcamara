/*
 * Dulcamara - the game picture's place on screen (src/platform/game_fit.h), checked on the host. The canvas is
 * 1920x1080 and the PS5's output 3840x2160 (twice it). HD pictures (Ren'Py) fill the height of a 16:9 output
 * whether integer scale is on or not; RPG Maker's 240 lines keep their integer multiple.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "platform/game_fit.h"

#include <stdio.h>

static int failures;

static void expect(int w, int h, bool integer_scale, int want_w, int want_h)
{
    int dw = 0, dh = 0;
    game_fit(1920, 1080, (float)w / (float)h, h, integer_scale, &dw, &dh);
    /* on the 3840x2160 output */
    const bool ok = dw * 2 == want_w && dh * 2 == want_h;
    printf("%s %dx%d integer=%d: %dx%d on 3840x2160 (want %dx%d)\n", ok ? "ok  " : "FAIL", w, h, integer_scale,
           dw * 2, dh * 2, want_w, want_h);
    failures += !ok;
}

int main(void)
{
    for (int integer = 0; integer < 2; ++integer)
    {
        expect(1280, 720, integer, 3840, 2160);
        expect(1366, 768, integer, 3840, 2158); /* 1366:768 is a hair wider than 16:9 */
        expect(1600, 900, integer, 3840, 2160);
        expect(1920, 1080, integer, 3840, 2160);
        expect(800, 600, integer, 2880, 2160); /* 4:3: pillarboxed, its own shape */
    }
    expect(320, 240, false, 2880, 2160); /* RPG Maker 2000/2003 */
    expect(320, 240, true, 2560, 1920);  /* integer scale: 4 x 240 lines on the canvas, as before */

    /* the log's line of the picture's place: once, then again only when something in it changes */
    GameFitNote last = {0};
    const GameFitNote hd = {1920, 1080, 1920, 1080, 0, 0, 1920, 1080, 0, true, true};
    GameFitNote moved = hd;
    moved.y = 60;
    moved.h = 960;
    const bool notes[4] = {game_fit_changed(&last, &hd), game_fit_changed(&last, &hd),
                           game_fit_changed(&last, &moved), game_fit_changed(&last, &moved)};
    const bool notes_ok = notes[0] && !notes[1] && notes[2] && !notes[3] && last.h == 960;
    printf("%s the picture's place is noted on change only (%d%d%d%d)\n", notes_ok ? "ok  " : "FAIL", notes[0],
           notes[1], notes[2], notes[3]);
    failures += !notes_ok;
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
