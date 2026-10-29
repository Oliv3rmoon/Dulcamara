/*
 * Nightshade Player - the library's covers (covers.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "covers.h"

#include "rpgmaker.h"

#include "../third_party/miniz/miniz.h"
#include "stb_image.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEEP 10        /* pictures kept on each side of the cursor */
#define MAX_FILE (16u << 20)

enum
{
    SLOT_NONE,
    SLOT_QUEUED,
    SLOT_DECODED, /* rgba waits for the main thread */
    SLOT_LOADED,
    SLOT_MISSING
};

typedef struct
{
    int state;
    PlatTexture *texture;
    uint8_t *rgba;
    int w, h;
    int engine;
    uint32_t color;
} Slot;

static Library *library;
static Slot *slots;
static int slot_count;
static SDL_Thread *worker;
static SDL_mutex *lock;
static SDL_cond *wake;
static bool quitting;
static int generation; /* a new library: work for the old one is dropped */

/* ---------------------------------------------------------------- decoding */

uint8_t *covers_decode(const uint8_t *data, size_t len, int *w, int *h)
{
    uint8_t *rgba = rpgm_decode_xyz(data, len, w, h);
    if (rgba)
        return rgba;
    int comp;
    unsigned char *px = stbi_load_from_memory(data, (int)len, w, h, &comp, 4);
    if (!px)
        return NULL;
    /* stb's buffer to one of ours (free()) */
    uint8_t *copy = malloc((size_t)*w * *h * 4);
    if (copy)
    {
        memcpy(copy, px, (size_t)*w * *h * 4);
        /* RPG Maker draws a title picture opaque: its transparent colour is still a colour */
        for (size_t i = 0; i < (size_t)*w * *h; ++i)
            copy[i * 4 + 3] = 255;
    }
    stbi_image_free(px);
    return copy;
}

static uint8_t *read_whole(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 && (unsigned long)size <= MAX_FILE ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len = data ? (size_t)size : 0;
    return data;
}

static uint8_t *read_entry(const char *archive, const char *entry, size_t *len)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, archive, 0))
        return NULL;
    uint8_t *data = NULL;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    char name[NP_PATH_MAX];
    for (mz_uint i = 0; i < n && !data; ++i)
    {
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        for (char *p = name; *p; ++p)
            if (*p == '\\')
                *p = '/';
        if (strcmp(name, entry) != 0)
            continue;
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st) && st.m_uncomp_size <= MAX_FILE)
        {
            size_t got = 0;
            void *heap = mz_zip_reader_extract_to_heap(&zip, i, &got, 0);
            if (heap)
            {
                data = malloc(got ? got : 1);
                if (data)
                    memcpy(data, heap, got);
                *len = got;
                mz_free(heap);
            }
        }
    }
    mz_zip_reader_end(&zip);
    return data;
}

/* 2x, nearest: pixel art stays sharp at the size the shelf shows it; mipmaps make the smaller ones */
static uint8_t *double_size(const uint8_t *rgba, int w, int h)
{
    uint8_t *out = malloc((size_t)w * h * 16);
    if (!out)
        return NULL;
    for (int y = 0; y < h * 2; ++y)
        for (int x = 0; x < w * 2; ++x)
            memcpy(out + ((size_t)y * w * 2 + x) * 4, rgba + ((size_t)(y / 2) * w + x / 2) * 4, 4);
    return out;
}

static void load_one(Game *g, Slot *result)
{
    char where[NP_PATH_MAX];
    result->state = SLOT_MISSING;
    if (!library_title_image(g, where, sizeof(where)))
    {
        result->engine = g->engine;
        return;
    }
    result->engine = g->engine;
    size_t len = 0;
    uint8_t *data = g->archive ? read_entry(g->location, where, &len) : read_whole(where, &len);
    if (!data)
        return;
    int w = 0, h = 0;
    uint8_t *rgba = covers_decode(data, len, &w, &h);
    free(data);
    if (!rgba || w <= 0 || h <= 0)
    {
        free(rgba);
        np_log("covers: %s: %s is not a picture it can read", g->title, where);
        return;
    }
    uint64_t r = 0, gr = 0, b = 0;
    for (size_t i = 0; i < (size_t)w * h; ++i)
        r += rgba[i * 4], gr += rgba[i * 4 + 1], b += rgba[i * 4 + 2];
    const size_t n = (size_t)w * h;
    result->color = 0xff000000u | (uint32_t)(r / n) << 16 | (uint32_t)(gr / n) << 8 | (uint32_t)(b / n);
    if (w <= 640 && h <= 480)
    {
        uint8_t *big = double_size(rgba, w, h);
        if (big)
        {
            free(rgba);
            rgba = big;
            w *= 2;
            h *= 2;
        }
    }
    result->rgba = rgba;
    result->w = w;
    result->h = h;
    result->state = SLOT_DECODED;
}

static int worker_main(void *unused)
{
    (void)unused;
    SDL_LockMutex(lock);
    while (!quitting)
    {
        int pick = -1;
        for (int i = 0; i < slot_count && pick < 0; ++i)
            if (slots[i].state == SLOT_QUEUED)
                pick = i;
        if (pick < 0)
        {
            SDL_CondWait(wake, lock);
            continue;
        }
        Game copy = library->games[pick];
        const int gen = generation;
        SDL_UnlockMutex(lock);
        Slot result;
        memset(&result, 0, sizeof(result));
        load_one(&copy, &result);
        SDL_LockMutex(lock);
        if (gen == generation && pick < slot_count && slots[pick].state == SLOT_QUEUED)
        {
            slots[pick].rgba = result.rgba;
            slots[pick].w = result.w;
            slots[pick].h = result.h;
            slots[pick].engine = result.engine;
            slots[pick].color = result.color;
            slots[pick].state = result.state;
        }
        else
            free(result.rgba);
    }
    SDL_UnlockMutex(lock);
    return 0;
}

/* ---------------------------------------------------------------- the main thread */

static void free_slots(void)
{
    for (int i = 0; i < slot_count; ++i)
    {
        if (slots[i].texture)
            plat_texture_free(slots[i].texture);
        free(slots[i].rgba);
    }
    free(slots);
    slots = NULL;
    slot_count = 0;
}

void covers_start(Library *lib)
{
    if (!lock)
    {
        lock = SDL_CreateMutex();
        wake = SDL_CreateCond();
    }
    SDL_LockMutex(lock);
    ++generation;
    free_slots();
    library = lib;
    slot_count = lib->count;
    slots = slot_count ? calloc((size_t)slot_count, sizeof(Slot)) : NULL;
    if (!slots)
        slot_count = 0;
    SDL_UnlockMutex(lock);
    if (!worker)
        worker = SDL_CreateThread(worker_main, "covers", NULL);
}

void covers_stop(void)
{
    if (!lock)
        return;
    SDL_LockMutex(lock);
    quitting = true;
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
    if (worker)
        SDL_WaitThread(worker, NULL);
    worker = NULL;
    free_slots();
}

void covers_update(int center)
{
    if (!lock)
        return;
    SDL_LockMutex(lock);
    bool asked = false;
    for (int i = 0; i < slot_count; ++i)
    {
        Slot *s = &slots[i];
        const int distance = i > center ? i - center : center - i;
        if (s->state == SLOT_DECODED)
        {
            s->texture = plat_texture_create_mips(s->rgba, s->w, s->h);
            free(s->rgba);
            s->rgba = NULL;
            s->state = s->texture ? SLOT_LOADED : SLOT_MISSING;
            library->games[i].engine = s->engine;
        }
        else if (s->state == SLOT_MISSING && s->engine && !library->games[i].engine)
            library->games[i].engine = s->engine;
        if (distance <= KEEP && s->state == SLOT_NONE)
        {
            s->state = SLOT_QUEUED;
            asked = true;
        }
        else if (distance > KEEP + 4 && (s->state == SLOT_LOADED || s->state == SLOT_QUEUED))
        {
            if (s->texture)
                plat_texture_free(s->texture);
            s->texture = NULL;
            s->state = SLOT_NONE;
        }
    }
    if (asked)
        SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
}

PlatTexture *covers_get(int index)
{
    return index >= 0 && index < slot_count ? slots[index].texture : NULL;
}

bool covers_missing(int index)
{
    return index >= 0 && index < slot_count && slots[index].state == SLOT_MISSING;
}

uint32_t covers_color(int index)
{
    return index >= 0 && index < slot_count ? slots[index].color : 0;
}
