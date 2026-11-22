/* Cached immutable scan results; SDL worker never accesses the active library.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "library.h"
#include "storage.h"
#include <SDL2/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INDEX_MAX_GAMES 65536u
static const unsigned char magic[8] = {'D','U','L','I','D','X',1,0};
/* Fixed little-endian numbers and length-prefixed strings, never native structs. */
static bool put32(FILE *f, uint32_t v)
{
    unsigned char b[4] = {v, v >> 8, v >> 16, v >> 24};
    return fwrite(b, 1, 4, f) == 4;
}
static bool get32(FILE *f, uint32_t *v)
{
    unsigned char b[4]; if (fread(b, 1, 4, f) != 4) return false;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; return true;
}
static bool putstr(FILE *f, const char *s)
{
    size_t n = strlen(s); return n < NP_PATH_MAX && put32(f, (uint32_t)n) && fwrite(s, 1, n, f) == n;
}
static bool getstr(FILE *f, char *s, size_t size)
{
    uint32_t n;
    if (!get32(f, &n) || n >= size || fread(s, 1, n, f) != n || memchr(s, 0, n)) return false;
    s[n] = 0; return true;
}
bool library_cache_save(const Library *lib, const char *root, const char *file)
{
    if (lib->count < 0 || (unsigned)lib->count > INDEX_MAX_GAMES || strlen(root) >= NP_PATH_MAX) return false;
    for (int i = 0; i < lib->count; ++i) if (!library_game_valid(&lib->games[i], root)) return false;
    StorageFile write; if (!storage_begin(&write, file)) return false;
    FILE *f = write.file;
    bool ok = fwrite(magic, 1, sizeof(magic), f) == sizeof(magic) && putstr(f, root) && put32(f, (uint32_t)lib->count);
    for (int i = 0; ok && i < lib->count; ++i) {
        const Game *g = &lib->games[i];
        ok = putstr(f, g->title) && putstr(f, g->name) && putstr(f, g->location) && putstr(f, g->inner) &&
             putstr(f, g->path) && putstr(f, g->id) && put32(f, g->archive) && put32(f, (uint32_t)g->engine) &&
             put32(f, (uint32_t)g->core) && put32(f, (uint32_t)g->played) && put32(f, (uint64_t)g->played >> 32);
    }
    ok = ok && put32(f, 0x454e4421);
    if (!ok) { storage_abort(&write); return false; }
    return storage_commit(&write);
}
static int compare_ids(const void *a, const void *b)
{
    const Game *const *x = a, *const *y = b; return strcmp((*x)->id, (*y)->id);
}
bool library_cache_load(Library *lib, const char *root, const char *file)
{
    FILE *f = fopen(file, "rb"); if (!f) return false;
    unsigned char header[8]; char stored_root[NP_PATH_MAX]; uint32_t count = 0;
    Library next = {0};
    bool ok = fread(header, 1, 8, f) == 8 && !memcmp(header, magic, 8) &&
              getstr(f, stored_root, sizeof(stored_root)) && !strcmp(root, stored_root) &&
              get32(f, &count) && count <= INDEX_MAX_GAMES;
    if (ok && count) { next.games = calloc(count, sizeof(Game)); ok = next.games != NULL; }
    if (ok) next.count = next.capacity = (int)count;
    for (int i = 0; ok && i < next.count; ++i) {
        Game *g = &next.games[i]; uint32_t archive, engine, core, lo, hi;
        ok = getstr(f, g->title, sizeof(g->title)) && getstr(f, g->name, sizeof(g->name)) &&
             getstr(f, g->location, sizeof(g->location)) && getstr(f, g->inner, sizeof(g->inner)) &&
             getstr(f, g->path, sizeof(g->path)) && getstr(f, g->id, sizeof(g->id)) &&
             get32(f, &archive) && archive <= 1 && get32(f, &engine) && engine <= INT_MAX &&
             get32(f, &core) && core < NP_CORES && get32(f, &lo) && get32(f, &hi) && hi <= INT_MAX;
        if (ok) {
            g->archive = archive != 0; g->engine = (int)engine; g->core = (int)core;
            g->played = (long long)((uint64_t)lo | (uint64_t)hi << 32);
            ok = library_game_valid(g, root);
        }
    }
    uint32_t end;
    ok = ok && get32(f, &end) && end == 0x454e4421 && fgetc(f) == EOF && !(ferror)(f);
    if (fclose(f) != 0) ok = false;
    /* Duplicate IDs would alias metadata and selection. Reject the whole cache. */
    if (ok && count > 1) {
        Game **ids = malloc(count * sizeof(*ids));
        if (!ids) ok = false;
        else {
            for (uint32_t i = 0; i < count; ++i) ids[i] = &next.games[i];
            qsort(ids, count, sizeof(*ids), compare_ids);
            for (uint32_t i = 1; i < count; ++i) if (!strcmp(ids[i-1]->id, ids[i]->id)) ok = false;
            free(ids);
        }
    }
    if (!ok) { library_free(&next); return false; }
    library_free(lib); *lib = next; return true;
}

struct LibraryScanner {
    SDL_mutex *lock; SDL_cond *wake; SDL_Thread *thread;
    bool quitting, requested, attempted_cache;
    char root[NP_PATH_MAX], cache[NP_PATH_MAX];
    LibraryScanStatus status;
    uint64_t snapshot_serial;
    Library ready;
    bool has_ready;
};
typedef struct { LibraryScanner *scanner; uint64_t generation; unsigned visited; } ScanContext;
static bool progress(void *opaque, unsigned visited, unsigned found)
{
    ScanContext *context = opaque; LibraryScanner *s = context->scanner;
    context->visited = visited;
    SDL_LockMutex(s->lock);
    bool active = !s->quitting && s->status.generation == context->generation && s->status.state == LIBRARY_SCAN_RUNNING;
    if (active) { s->status.visited = visited; s->status.found = found; }
    SDL_UnlockMutex(s->lock); return active;
}
static bool current(LibraryScanner *s, uint64_t generation)
{
    return !s->quitting && s->status.generation == generation && s->status.state == LIBRARY_SCAN_RUNNING;
}
static void offer(LibraryScanner *s, Library *next, bool cached)
{
    library_free(&s->ready); s->ready = *next; memset(next, 0, sizeof(*next));
    s->ready.generation = ++s->snapshot_serial; s->has_ready = true; s->status.cached = cached;
}
static int scan_worker(void *opaque)
{
    LibraryScanner *s = opaque;
    SDL_LockMutex(s->lock);
    while (!s->quitting) {
        while (!s->requested && !s->quitting) SDL_CondWait(s->wake, s->lock);
        if (s->quitting) break;
        char root[NP_PATH_MAX], cache[NP_PATH_MAX];
        str_copy(root, sizeof(root), s->root); str_copy(cache, sizeof(cache), s->cache);
        ScanContext context = {.scanner=s, .generation=s->status.generation}; s->requested = false;
        bool use_cache = !s->attempted_cache && cache[0]; s->attempted_cache = true;
        SDL_UnlockMutex(s->lock);
        Library next = {0};
        if (use_cache && library_cache_load(&next, root, cache)) {
            SDL_LockMutex(s->lock);
            if (current(s, context.generation)) offer(s, &next, true);
            SDL_UnlockMutex(s->lock); library_free(&next);
        }
        char error[sizeof(s->status.error)];
        bool ok = library_scan_checked(&next, root, progress, &context, error, sizeof(error));
        /* Cache is derived data. Its write stays off the input thread; failure
         * does not turn a successful scan into an empty or failed library. */
        if (ok && cache[0] && progress(&context, context.visited, (unsigned)next.count))
            if (!library_cache_save(&next, root, cache)) np_log("library: could not save index %s", cache);
        SDL_LockMutex(s->lock);
        if (current(s, context.generation)) {
            if (ok) { offer(s, &next, false); s->status.state = LIBRARY_SCAN_DONE; }
            else { s->status.state = LIBRARY_SCAN_ERROR; str_copy(s->status.error, sizeof(s->status.error), error); }
        }
        library_free(&next);
    }
    SDL_UnlockMutex(s->lock); return 0;
}
LibraryScanner *library_scanner_create(void)
{
    LibraryScanner *s = calloc(1, sizeof(*s)); if (!s) return NULL;
    s->lock = SDL_CreateMutex(); s->wake = SDL_CreateCond();
    if (s->lock && s->wake) s->thread = SDL_CreateThread(scan_worker, "library scan", s);
    if (!s->thread) { if (s->wake) SDL_DestroyCond(s->wake); if (s->lock) SDL_DestroyMutex(s->lock); free(s); return NULL; }
    return s;
}
bool library_scanner_request(LibraryScanner *s, const char *root, const char *cache)
{
    if (!s || !root || !*root || strlen(root) >= NP_PATH_MAX || (cache && strlen(cache) >= NP_PATH_MAX)) return false;
    SDL_LockMutex(s->lock);
    uint64_t generation = s->status.generation + 1;
    s->status = (LibraryScanStatus){.state=LIBRARY_SCAN_RUNNING, .generation=generation};
    str_copy(s->root, sizeof(s->root), root); str_copy(s->cache, sizeof(s->cache), cache ? cache : "");
    library_free(&s->ready); s->has_ready = false; s->requested = true;
    SDL_CondSignal(s->wake); SDL_UnlockMutex(s->lock); return true;
}
void library_scanner_cancel(LibraryScanner *s)
{
    if (!s) return;
    SDL_LockMutex(s->lock); ++s->status.generation; s->status.state = LIBRARY_SCAN_CANCELLED;
    s->requested = false; library_free(&s->ready); s->has_ready = false;
    SDL_UnlockMutex(s->lock);
}
bool library_scanner_poll(LibraryScanner *s, Library *result, LibraryScanStatus *status)
{
    if (!s) { if (status) *status = (LibraryScanStatus){.state=LIBRARY_SCAN_ERROR}; return false; }
    SDL_LockMutex(s->lock);
    if (status) *status = s->status;
    bool ready = result && s->has_ready;
    if (ready) { *result = s->ready; memset(&s->ready, 0, sizeof(s->ready)); s->has_ready = false; }
    SDL_UnlockMutex(s->lock); return ready;
}
void library_scanner_destroy(LibraryScanner *s)
{
    if (!s) return;
    SDL_LockMutex(s->lock); s->quitting = true; SDL_CondSignal(s->wake); SDL_UnlockMutex(s->lock);
    SDL_WaitThread(s->thread, NULL); library_free(&s->ready);
    SDL_DestroyCond(s->wake); SDL_DestroyMutex(s->lock); free(s);
}
