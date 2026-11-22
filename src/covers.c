/*
 * Dulcamara - the library's covers (covers.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "covers.h"

#include "rpgmaker.h"

#include "../third_party/miniz/miniz.h"
#include "stb_image.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define KEEP 10        /* pictures kept on each side of the cursor */
#define MAX_FILE (16u << 20)

enum
{
    SLOT_NONE,
    SLOT_QUEUED,
    SLOT_LOADING,
    SLOT_DECODED, /* rgba waits for the main thread */
    SLOT_LOADED,
    SLOT_MISSING
};

typedef struct
{
    int state;
    int priority;
    Game game;
    size_t texture_bytes;
    PlatTexture *texture;
    uint8_t *rgba;
    int w, h;
    int engine;
    uint32_t color;
} Slot;

static Slot *slots;
static int slot_count;
static SDL_Thread *worker;
static SDL_mutex *lock;
static SDL_cond *wake;
static bool quitting;
static uint64_t snapshot_generation;
static size_t decoded_bytes, texture_bytes, last_upload_bytes;
static bool working;
static uint64_t generation; /* a new library: work for the old one is dropped */

/* ---------------------------------------------------------------- decoding */

uint8_t *covers_decode(const uint8_t *data, size_t len, int *w, int *h)
{
    if (!data || len > MAX_FILE || len > INT_MAX) return NULL;
    bool xyz = len >= 8 && !memcmp(data, "XYZ1", 4);
    int width = 0, height = 0, comp;
    if (xyz) { width = data[4] | data[5] << 8; height = data[6] | data[7] << 8; }
    else if (!stbi_info_from_memory(data, (int)len, &width, &height, &comp)) return NULL;
    if (width <= 0 || height <= 0 || (uint64_t)width * height * 4 > COVERS_SOURCE_BUDGET) return NULL;
    uint8_t *rgba = xyz ? rpgm_decode_xyz(data, len, w, h) : stbi_load_from_memory(data, (int)len, w, h, &comp, 4);
    if (rgba) for (size_t i = 0; i < (size_t)*w * *h; ++i) rgba[i * 4 + 3] = 255;
    return rgba; /* stb uses the same malloc/free allocator */
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
    if ((size_t)w * h * 4 > COVERS_UPLOAD_BUDGET || w > 1024 || h > 1024)
    {
        int nw = w >= h ? 1024 : (int)((int64_t)w * 1024 / h);
        int nh = h >= w ? 1024 : (int)((int64_t)h * 1024 / w);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        uint8_t *small = malloc((size_t)nw * nh * 4);
        if (!small) { free(rgba); return; }
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                memcpy(small + ((size_t)y * nw + x) * 4,
                       rgba + ((size_t)((int64_t)y * h / nh) * w + (int64_t)x * w / nw) * 4, 4);
        free(rgba); rgba = small; w = nw; h = nh;
    }
    else if (w <= 640 && h <= 480 && (size_t)w * h * 16 <= COVERS_UPLOAD_BUDGET)
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

static size_t pixels(const Slot *s) { return (size_t)s->w * s->h * 4; }
static size_t mip_bytes(int w, int h)
{
    size_t bytes = 0;
    for (;;) { bytes += (size_t)w * h * 4; if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    return bytes;
}
static int worker_main(void *unused)
{
    (void)unused;
    SDL_LockMutex(lock);
    while (!quitting) {
        int pick = -1;
        if (decoded_bytes + COVERS_UPLOAD_BUDGET <= COVERS_DECODED_BUDGET)
            for (int i = 0; i < slot_count; ++i)
                if (slots[i].state == SLOT_QUEUED && (pick < 0 || slots[i].priority < slots[pick].priority)) pick = i;
        if (pick < 0) { SDL_CondWait(wake, lock); continue; }
        Game copy = slots[pick].game;
        uint64_t gen = generation;
        slots[pick].state = SLOT_LOADING; working = true;
        SDL_UnlockMutex(lock);
        Slot result = {0}; load_one(&copy, &result);
        SDL_LockMutex(lock); working = false;
        if (gen == generation && pick < slot_count && slots[pick].state == SLOT_LOADING && slots[pick].priority < INT_MAX) {
            Slot *s = &slots[pick];
            s->rgba = result.rgba; s->w = result.w; s->h = result.h;
            s->engine = result.engine; s->color = result.color; s->state = result.state;
            if (s->rgba) decoded_bytes += pixels(s);
        } else free(result.rgba);
    }
    SDL_UnlockMutex(lock); return 0;
}

static void clear_slot(Slot *s)
{
    if (s->texture) { plat_texture_free(s->texture); texture_bytes -= s->texture_bytes; }
    if (s->rgba) { decoded_bytes -= pixels(s); free(s->rgba); }
    s->texture = NULL; s->rgba = NULL; s->texture_bytes = 0; s->state = SLOT_NONE;
}
static void free_slots(void)
{
    for (int i = 0; i < slot_count; ++i) clear_slot(&slots[i]);
    free(slots); slots = NULL; slot_count = 0;
}
void covers_start(Library *lib)
{
    if (!lock) {
        lock = SDL_CreateMutex(); wake = SDL_CreateCond();
        if (!lock || !wake) {
            if (lock) SDL_DestroyMutex(lock);
            if (wake) SDL_DestroyCond(wake);
            lock = NULL; wake = NULL; return;
        }
    }
    SDL_LockMutex(lock); ++generation; free_slots();
    snapshot_generation = lib->generation; last_upload_bytes = 0; quitting = false;
    slot_count = lib->count;
    slots = slot_count ? calloc((size_t)slot_count, sizeof(Slot)) : NULL;
    if (!slots) slot_count = 0;
    for (int i = 0; i < slot_count; ++i) { slots[i].game = lib->games[i]; slots[i].priority = INT_MAX; }
    SDL_UnlockMutex(lock);
    if (!worker) worker = SDL_CreateThread(worker_main, "covers", NULL);
}
void covers_stop(void)
{
    if (!lock) return;
    SDL_LockMutex(lock); quitting = true; SDL_CondSignal(wake); SDL_UnlockMutex(lock);
    if (worker) SDL_WaitThread(worker, NULL);
    worker = NULL; free_slots();
    SDL_DestroyCond(wake); SDL_DestroyMutex(lock); wake = NULL; lock = NULL;
}
void covers_update_visible(const int *indexes, int count)
{
    if (!lock) return;
    SDL_LockMutex(lock); last_upload_bytes = 0;
    for (int i = 0; i < slot_count; ++i) slots[i].priority = INT_MAX;
    if (count > KEEP * 2 + 1) count = KEEP * 2 + 1;
    for (int i = 0; i < count; ++i)
        if (indexes[i] >= 0 && indexes[i] < slot_count && slots[indexes[i]].priority == INT_MAX)
            slots[indexes[i]].priority = i;
    int pick = -1;
    for (int i = 0; i < slot_count; ++i) {
        Slot *s = &slots[i];
        if (s->priority == INT_MAX) {
            if (s->state != SLOT_MISSING) clear_slot(s);
            continue;
        }
        if (s->state == SLOT_NONE) s->state = SLOT_QUEUED;
        if (s->state == SLOT_DECODED && (pick < 0 || s->priority < slots[pick].priority)) pick = i;
    }
    if (pick >= 0) {
        Slot *s = &slots[pick]; size_t bytes = mip_bytes(s->w, s->h);
        while (texture_bytes + bytes > COVERS_TEXTURE_BUDGET) {
            int worst = -1;
            for (int i = 0; i < slot_count; ++i)
                if (slots[i].texture && slots[i].priority > s->priority &&
                    (worst < 0 || slots[i].priority > slots[worst].priority)) worst = i;
            if (worst < 0) break;
            clear_slot(&slots[worst]);
        }
        if (texture_bytes + bytes <= COVERS_TEXTURE_BUDGET) {
            /* GPU allocation is on the main thread, outside the worker lock. No
             * scan/shelf operation may call start concurrently with this call. */
            uint8_t *rgba = s->rgba; int w = s->w, h = s->h;
            s->rgba = NULL; s->state = SLOT_LOADED;
            SDL_UnlockMutex(lock);
            PlatTexture *texture = plat_texture_create_mips(rgba, w, h); free(rgba);
            SDL_LockMutex(lock);
            decoded_bytes -= pixels(s);
            s->texture = texture; s->state = texture ? SLOT_LOADED : SLOT_MISSING;
            s->texture_bytes = texture ? bytes : 0; texture_bytes += s->texture_bytes;
            last_upload_bytes = (size_t)w * h * 4;
        }
    }
    SDL_CondSignal(wake); SDL_UnlockMutex(lock);
}
void covers_update(int center)
{
    int indexes[KEEP * 2 + 1], n = 0; indexes[n++] = center;
    for (int i = 1; i <= KEEP; ++i) { indexes[n++] = center - i; indexes[n++] = center + i; }
    covers_update_visible(indexes, n);
}
PlatTexture *covers_get(int index)
{
    /* Texture pointers are created/freed only by main-thread calls. */
    return index >= 0 && index < slot_count ? slots[index].texture : NULL;
}
bool covers_missing(int index)
{
    if (!lock) return false;
    SDL_LockMutex(lock); bool missing = index >= 0 && index < slot_count && slots[index].state == SLOT_MISSING;
    SDL_UnlockMutex(lock); return missing;
}
uint32_t covers_color(int index)
{
    if (!lock) return 0;
    SDL_LockMutex(lock); uint32_t color = index >= 0 && index < slot_count ? slots[index].color : 0;
    SDL_UnlockMutex(lock); return color;
}
int covers_engine(int index)
{
    if (!lock) return 0;
    SDL_LockMutex(lock); int engine = index >= 0 && index < slot_count ? slots[index].engine : 0;
    SDL_UnlockMutex(lock); return engine;
}
CoverStats covers_stats(void)
{
    if (!lock) return (CoverStats){0};
    SDL_LockMutex(lock);
    CoverStats result = {decoded_bytes, texture_bytes, last_upload_bytes, snapshot_generation, working};
    SDL_UnlockMutex(lock); return result;
}
