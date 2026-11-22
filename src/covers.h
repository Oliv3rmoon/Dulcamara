/*
 * Dulcamara - the library's covers: each game's title screen picture (library_title_image), read and
 * decoded on a background thread (PNG and BMP through stb_image, RPG Maker's XYZ through rpgmaker.c), uploaded on
 * the main thread.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_COVERS_H
#define NP_COVERS_H

#include "library.h"
#define COVERS_DECODED_BUDGET (8u << 20)
#define COVERS_TEXTURE_BUDGET (32u << 20)
#define COVERS_UPLOAD_BUDGET (4u << 20)
#define COVERS_SOURCE_BUDGET (16u << 20)
#include "platform/platform.h"

/* (Re)binds the covers to a freshly scanned library. */
void covers_start(Library *lib);
void covers_stop(void);
/* Main thread, once a frame: uploads finished pictures, asks for the ones around `center` and frees far ones. */
void covers_update(int center);
/* Main thread, once per frame, snapshot indexes in priority order (chosen first).
 * At most 21 requests; no upload larger than 4 MiB, no more than one per call.
 * Texture accounting includes the complete RGBA mip chain. The worker owns an
 * additional source pixel buffer capped at 16 MiB; codec scratch and the
 * compressed input (at most 16 MiB) are separate from the decoded cache. */
void covers_update_visible(const int *indexes, int count);
typedef struct {
    size_t decoded_bytes, texture_bytes, last_upload_bytes;
    uint64_t snapshot_generation;
    bool working;
} CoverStats;
CoverStats covers_stats(void);
/* The picture of game `index` (twice its size, pixel-sharp), or NULL while it loads or when the game has none. */
PlatTexture *covers_get(int index);
/* true once the game's picture was looked for and there is none */
bool covers_missing(int index);
/* The picture's average colour, 0 until it has loaded. */
uint32_t covers_color(int index);
/* Worker-discovered engine metadata; never writes the immutable snapshot. */
int covers_engine(int index);
/* Decodes a title picture (PNG, BMP or XYZ by its contents) to RGBA; free() it. Used by the loader and the tests. */
uint8_t *covers_decode(const uint8_t *data, size_t len, int *width, int *height);

#endif
