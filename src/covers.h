/*
 * Nightshade Player - the library's covers: each game's title screen picture (library_title_image), read and
 * decoded on a background thread (PNG and BMP through stb_image, RPG Maker's XYZ through rpgmaker.c), uploaded on
 * the main thread.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_COVERS_H
#define NP_COVERS_H

#include "library.h"
#include "platform/platform.h"

/* (Re)binds the covers to a freshly scanned library. */
void covers_start(Library *lib);
void covers_stop(void);
/* Main thread, once a frame: uploads finished pictures, asks for the ones around `center` and frees far ones. */
void covers_update(int center);
/* The picture of game `index` (twice its size, pixel-sharp), or NULL while it loads or when the game has none. */
PlatTexture *covers_get(int index);
/* true once the game's picture was looked for and there is none */
bool covers_missing(int index);
/* The picture's average colour, 0 until it has loaded. */
uint32_t covers_color(int index);
/* Decodes a title picture (PNG, BMP or XYZ by its contents) to RGBA; free() it. Used by the loader and the tests. */
uint8_t *covers_decode(const uint8_t *data, size_t len, int *width, int *height);

#endif
