/*
 * Dulcamara - Ren'Py's core: the pictures Ren'Py draws, on their way to the frontend. Three buffers, so that Ren'Py
 * can draw the next picture while the frontend still shows the last one: Ren'Py writes the back one and publishes
 * it, the frontend takes the newest and keeps it until it takes another.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_FRAMES_H
#define DR_FRAMES_H

#include <stdbool.h>
#include <stdint.h>

void frames_reset(void); /* no pictures, the buffers freed */

/* Ren'Py's thread: the buffer for the next picture, w x h XRGB8888, rows top to bottom. NULL without memory. */
uint32_t *frames_back(int w, int h);
/* Ren'Py's thread: what was written into frames_back is the newest picture. */
void frames_publish(void);
/* The frontend: the newest picture, valid until the next call; fresh when it is new since the last call. NULL
 * before the first. */
const uint32_t *frames_take(int *w, int *h, bool *fresh);

/* OpenGL's rows run bottom to top: copies src's rows into dst, top to bottom. */
void frames_copy_flipped(uint32_t *dst, const uint32_t *src, int w, int h);
/* An arrow cursor drawn into a w x h picture with its tip at x, y, bigger on bigger pictures. */
void frames_draw_cursor(uint32_t *pixels, int w, int h, int x, int y);
/* w x h made to fit max_w x max_h, keeping its shape; sizes that fit already are kept. */
void frames_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h);

#endif
