/*
 * Dulcamara - Ren'Py's core: the pictures (frames.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "frames.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint32_t *pixels;
    size_t capacity; /* in pixels */
    int w, h;
} Slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Slot slots[3];
static int back = 0, ready = 1, front = 2;
static bool ready_is_new, any;

void frames_reset(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 3; ++i)
        free(slots[i].pixels);
    memset(slots, 0, sizeof(slots));
    back = 0, ready = 1, front = 2;
    ready_is_new = any = false;
    pthread_mutex_unlock(&lock);
}

uint32_t *frames_back(int w, int h)
{
    if (w <= 0 || h <= 0)
        return NULL;
    pthread_mutex_lock(&lock);
    Slot *s = &slots[back];
    const size_t need = (size_t)w * (size_t)h;
    if (need > s->capacity)
    {
        uint32_t *grown = realloc(s->pixels, need * sizeof(uint32_t));
        if (!grown)
        {
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        s->pixels = grown;
        s->capacity = need;
    }
    s->w = w;
    s->h = h;
    uint32_t *pixels = s->pixels;
    pthread_mutex_unlock(&lock);
    return pixels;
}

void frames_publish(void)
{
    pthread_mutex_lock(&lock);
    const int was_ready = ready;
    ready = back;
    back = was_ready;
    ready_is_new = any = true;
    pthread_mutex_unlock(&lock);
}

const uint32_t *frames_take(int *w, int *h, bool *fresh)
{
    pthread_mutex_lock(&lock);
    const bool is_new = ready_is_new;
    if (is_new)
    {
        const int was_front = front;
        front = ready;
        ready = was_front;
        ready_is_new = false;
    }
    const Slot *s = &slots[front];
    const uint32_t *pixels = any && s->pixels && s->w ? s->pixels : NULL;
    *w = pixels ? s->w : 0;
    *h = pixels ? s->h : 0;
    if (fresh)
        *fresh = is_new && pixels;
    pthread_mutex_unlock(&lock);
    return pixels;
}

void frames_copy_flipped(uint32_t *dst, const uint32_t *src, int w, int h)
{
    for (int y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * (size_t)w, src + (size_t)(h - 1 - y) * (size_t)w, (size_t)w * sizeof(uint32_t));
}

void frames_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h)
{
    if (w <= max_w && h <= max_h)
    {
        *out_w = w;
        *out_h = h;
    }
    else if ((long long)w * max_h >= (long long)h * max_w)
    {
        *out_w = max_w;
        *out_h = (int)(((long long)h * max_w + w / 2) / w);
    }
    else
    {
        *out_h = max_h;
        *out_w = (int)(((long long)w * max_h + h / 2) / h);
    }
}

/* the arrow: X its outline, O its fill */
static const char *const ARROW[] = {
    "X           ", "XX          ", "XOX         ", "XOOX        ", "XOOOX       ", "XOOOOX      ", "XOOOOOX     ",
    "XOOOOOOX    ", "XOOOOOOOX   ", "XOOOOOOOOX  ", "XOOOOOOOOOX ", "XOOOOOOXXXXX", "XOOOXOOX    ", "XOOXXOOX    ",
    "XOX  XOOX   ", "XX   XOOX   ", "X     XOOX  ", "      XOOX  ", "       XX   ",
};

void frames_draw_cursor(uint32_t *pixels, int w, int h, int x, int y)
{
    const int scale = h >= 1000 ? 2 : 1, rows = (int)(sizeof(ARROW) / sizeof(ARROW[0]));
    for (int r = 0; r < rows * scale; ++r)
        for (int c = 0; c < 12 * scale; ++c)
        {
            const char m = ARROW[r / scale][c / scale];
            const int px = x + c, py = y + r;
            if (m != ' ' && px >= 0 && px < w && py >= 0 && py < h)
                pixels[(size_t)py * (size_t)w + (size_t)px] = m == 'X' ? 0xFF000000u : 0xFFFFFFFFu;
        }
}
