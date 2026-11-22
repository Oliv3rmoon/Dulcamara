/* Real filesystem/SDL workers. Only the GPU allocation boundary is substituted.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "library.h"
#include "covers.h"
#include "userdata.h"
#include "core/cores.h"
#include "../third_party/miniz/miniz.h"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

struct PlatTexture { int w, h; uint8_t red; };
static int uploads, first_red;
PlatTexture *plat_texture_create_mips(const uint8_t *rgba, int w, int h)
{
    PlatTexture *p = malloc(sizeof(*p));
    assert(p);
    *p = (PlatTexture){w, h, rgba[0]};
    if (!uploads) first_red = rgba[0];
    ++uploads;
    return p;
}
void plat_texture_free(PlatTexture *p) { free(p); }

typedef struct { double values[8192]; int count; } Timing;
static Timing poll_timing, request_timing, view_timing, cover_timing;
static void measured(Timing *timing, uint64_t start)
{
    double ms = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    assert(ms < 100.0);
    assert(timing->count < 8192); timing->values[timing->count++] = ms;
}
static int compare_ms(const void *a, const void *b) { double x=*(const double *)a, y=*(const double *)b; return (x>y)-(x<y); }
static void report_time(const char *name, Timing *timing)
{
    if (!timing->count) return;
    qsort(timing->values, (size_t)timing->count, sizeof(double), compare_ms);
    printf("%s: n=%d p50=%.4f ms p95=%.4f ms max=%.4f ms\n", name, timing->count,
           timing->values[timing->count/2], timing->values[(timing->count-1)*95/100], timing->values[timing->count-1]);
}
static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(!fclose(f));
}
static void folder(const char *root, const char *name, const char *title)
{
    char dir[NP_PATH_MAX], path[NP_PATH_MAX];
    path_join(dir, sizeof(dir), root, name); assert(make_dirs(dir));
    path_join(path, sizeof(path), dir, "RPG_RT.ldb"); write_text(path, "LcfDataBase");
    path_join(path, sizeof(path), dir, "RPG_RT.lmt"); write_text(path, "LcfMapTree");
    path_join(path, sizeof(path), dir, "RPG_RT.ini");
    char ini[512]; snprintf(ini, sizeof(ini), "[RPG_RT]\nGameTitle=%s\n", title); write_text(path, ini);
}
static LibraryScanStatus wait_scan(LibraryScanner *s, Library *active, Settings *settings)
{
    uint64_t until = SDL_GetTicks64() + 20000;
    LibraryScanStatus status;
    do {
        uint64_t before = SDL_GetPerformanceCounter();
        Library ready = {0};
        if (library_scanner_poll(s, &ready, &status)) {
            library_free(active); *active = ready;
            library_restore_selection(active, settings);
        }
        measured(&poll_timing, before);
        if (status.state != LIBRARY_SCAN_RUNNING) return status;
        SDL_Delay(1);
    } while (SDL_GetTicks64() < until);
    assert(!"scan timed out"); return status;
}
static void scan_test(const char *root)
{
    Library active = {0}; Settings settings = {0};
    LibraryScanner *s = library_scanner_create(); assert(s);
    assert(library_scanner_request(s, root, NULL));
    LibraryScanStatus status = wait_scan(s, &active, &settings);
    assert(status.state == LIBRARY_SCAN_DONE && active.count == 0);
    uint64_t before = SDL_GetPerformanceCounter();
    for (int i = 0; i < 1000; ++i) {
        char name[32]; snprintf(name, sizeof(name), "Game%04d", i); folder(root, name, name);
    }
    char path[NP_PATH_MAX]; path_join(path, sizeof(path), root, "Collection"); assert(make_dirs(path));
    folder(path, "日本語", "夜の物語");
    path_join(path, sizeof(path), root, "packed.zip");
    mz_zip_archive zip = {0}; assert(mz_zip_writer_init_file(&zip, path, 0));
    assert(mz_zip_writer_add_mem(&zip, "Game/RPG_RT.ldb", "x", 1, 0));
    assert(mz_zip_writer_add_mem(&zip, "Game/RPG_RT.lmt", "x", 1, 0));
    assert(mz_zip_writer_finalize_archive(&zip)); assert(mz_zip_writer_end(&zip));
    assert(library_scanner_request(s, root, NULL));
    status = wait_scan(s, &active, &settings);
    assert(status.state == LIBRARY_SCAN_DONE && active.count == 1002 && status.visited >= 1002);
    bool unicode = false, archive = false;
    for (int i = 0; i < active.count; ++i) {
        unicode |= !strcmp(active.games[i].title, "夜の物語"); archive |= active.games[i].archive;
    }
    assert(unicode && archive);
    char index_path[NP_PATH_MAX]; path_join(index_path, sizeof(index_path), root, "scan-index.bin");
    assert(library_cache_save(&active, root, index_path));
    Library cached = {0}; assert(library_cache_load(&cached, root, index_path) && cached.count == 1002);
    for (int i=0; i<cached.count; ++i) if (cached.games[i].archive) assert(library_revalidate(&cached.games[i]));
    library_free(&cached);
    assert(library_select(&active, &settings, 599)); char keep[24]; strcpy(keep, settings.selected_game_id);
    for (int i = 0; i < 30; ++i) {
        uint64_t t = SDL_GetPerformanceCounter();
        assert(library_scanner_request(s, root, NULL));
        library_scanner_cancel(s);
        measured(&request_timing, t);
    }
    status = wait_scan(s, &active, &settings);
    assert(status.state == LIBRARY_SCAN_CANCELLED && active.count == 1002);
    assert(!strcmp(keep, settings.selected_game_id));
    char missing[NP_PATH_MAX]; path_join(missing, sizeof(missing), root, "missing");
    assert(library_scanner_request(s, missing, NULL));
    status = wait_scan(s, &active, &settings);
    assert(status.state == LIBRARY_SCAN_ERROR && status.error[0] && active.count == 1002);
    assert(!chmod(root, 0000));
    assert(library_scanner_request(s, root, NULL));
    status = wait_scan(s, &active, &settings);
    assert(!chmod(root, 0700));
    assert(status.state == LIBRARY_SCAN_ERROR && active.count == 1002);
    char unreadable[NP_PATH_MAX]; path_join(unreadable, sizeof(unreadable), root, "Collection/日本語");
    assert(!chmod(unreadable, 0000));
    assert(library_scanner_request(s, root, NULL));
    status = wait_scan(s, &active, &settings);
    assert(!chmod(unreadable, 0700));
    assert(status.state == LIBRARY_SCAN_ERROR && active.count == 1002);
    path_join(unreadable, sizeof(unreadable), root, "packed.zip");
    assert(!chmod(unreadable, 0000));
    assert(library_scanner_request(s, root, NULL));
    status = wait_scan(s, &active, &settings);
    assert(!chmod(unreadable, 0600));
    assert(status.state == LIBRARY_SCAN_ERROR && active.count == 1002);
    /* View work is independent of an in-flight scan and scales to 1000 games. */
    LibraryView view = {0}; LibraryFilter filter = {.section=-1};
    UserMetadata favorites = {0};
    for (int i=0; i<1002; i+=3) assert(user_metadata_set_favorite(&favorites, active.games[i].id, true));
    for (int i=0; i<200; ++i) {
        uint64_t t = SDL_GetPerformanceCounter();
        filter.favorites_only = i%2 != 0;
        assert(library_view_build(&view, &active, &favorites, &filter, i%2));
        assert(view.count == (filter.favorites_only ? 334 : 1002));
        measured(&view_timing, t);
    }
    library_view_free(&view); user_metadata_free(&favorites);
    assert(library_scanner_request(s, root, NULL));
    assert(library_select(&active, &settings, 27)); strcpy(keep, settings.selected_game_id);
    Game *retained = active.games;
    uint64_t until = SDL_GetTicks64() + 10000;
    do {
        library_scanner_poll(s, NULL, &status); /* runtime holds an active index */
        assert(active.games == retained && !strcmp(active.games[settings.last_game].id, keep));
        SDL_Delay(1);
    } while (status.state == LIBRARY_SCAN_RUNNING && SDL_GetTicks64() < until);
    assert(status.state == LIBRARY_SCAN_DONE);
    status = wait_scan(s, &active, &settings);
    assert(status.state == LIBRARY_SCAN_DONE && !strcmp(active.games[settings.last_game].id, keep));
    printf("scan: 1002 entries, cancel/repeat/error/selection, elapsed %.2f ms (including fixture creation)\n",
           (SDL_GetPerformanceCounter()-before)*1000.0/SDL_GetPerformanceFrequency());
    library_scanner_destroy(s); library_free(&active);
    report_time("poll + publication/selection", &poll_timing);
    report_time("request + cancel", &request_timing);
    report_time("view sort/filter 1002", &view_timing);
}
static void nested_renpy_test(const char *root)
{
    char content[NP_PATH_MAX], script[NP_PATH_MAX];
    path_join(content, sizeof(content), root, "Collection/Renpy/game"); assert(make_dirs(content));
    path_join(script, sizeof(script), content, "script.rpy"); write_text(script, "label start:\n    return\n");
    LibraryScanner *scanner = library_scanner_create(); assert(scanner);
    Library active = {0}; Settings settings = {0};
    assert(library_scanner_request(scanner, root, NULL));
    assert(wait_scan(scanner, &active, &settings).state == LIBRARY_SCAN_DONE && active.count == 1);
    Game *retained = active.games; char id[24]; strcpy(id, active.games[0].id);
    assert(!chmod(content, 0000));
    assert(library_scanner_request(scanner, root, NULL));
    LibraryScanStatus status = wait_scan(scanner, &active, &settings);
    assert(!chmod(content, 0700));
    assert(status.state == LIBRARY_SCAN_ERROR && status.error[0]);
    assert(active.games == retained && active.count == 1 && !strcmp(active.games[0].id, id));
    /* A readable but genuinely empty content directory is a successful removal. */
    assert(!unlink(script));
    assert(library_scanner_request(scanner, root, NULL));
    assert(wait_scan(scanner, &active, &settings).state == LIBRARY_SCAN_DONE && active.count == 0);
    library_scanner_destroy(scanner); library_free(&active);
    puts("nested Ren'Py: unreadable content retains snapshot, genuinely empty content removes game");
}
static void cache_test(const char *root)
{
    folder(root, "日本語", "夜の物語"); folder(root, "Alpha", "Alpha");
    Library lib = {0}, cached = {0}; library_scan(&lib, root);
    char cache[NP_PATH_MAX]; path_join(cache, sizeof(cache), root, "index.bin");
    assert(library_cache_save(&lib, root, cache));
    assert(library_cache_load(&cached, root, cache) && cached.count == 2);
    assert(!memcmp(lib.games, cached.games, sizeof(Game) * 2));
    /* Cache is observable before a blocked reconciliation finishes. */
    char blocked_ini[NP_PATH_MAX]; path_join(blocked_ini, sizeof(blocked_ini), root, "Alpha/RPG_RT.ini");
    assert(!unlink(blocked_ini)); assert(!mkfifo(blocked_ini, 0600));
    LibraryScanner *cold = library_scanner_create(); assert(cold);
    assert(library_scanner_request(cold, root, cache));
    Library cold_result = {0}; LibraryScanStatus cold_status = {0}; bool received = false;
    for (int i=0; i<1000 && !received; ++i) {
        received = library_scanner_poll(cold, &cold_result, &cold_status);
        if (!received) SDL_Delay(1);
    }
    assert(received && cold_result.count == 2 && cold_status.cached && cold_status.state == LIBRARY_SCAN_RUNNING);
    uint64_t cached_generation = cold_result.generation;
    int writer = -1;
    for (int i=0; i<1000 && writer < 0; ++i) { writer = open(blocked_ini, O_WRONLY | O_NONBLOCK); if (writer < 0) SDL_Delay(1); }
    assert(writer >= 0); close(writer);
    Settings cold_settings = {0};
    assert(wait_scan(cold, &cold_result, &cold_settings).state == LIBRARY_SCAN_DONE);
    assert(cold_result.generation != cached_generation);
    library_scanner_destroy(cold); library_free(&cold_result);
    assert(!unlink(blocked_ini)); write_text(blocked_ini, "[RPG_RT]\nGameTitle=Alpha\n");

    char file[NP_PATH_MAX]; path_join(file, sizeof(file), root, "Alpha/RPG_RT.lmt"); assert(!unlink(file));
    for (int i = 0; i < cached.count; ++i)
        if (!strcmp(cached.games[i].title, "Alpha")) assert(!library_revalidate(&cached.games[i]));
    assert(library_cache_load(&cached, root, cache)); /* locations are checked at launch/reconciliation */
    LibraryScanner *s = library_scanner_create(); Settings settings = {0};
    assert(library_scanner_request(s, root, cache));
    assert(wait_scan(s, &cached, &settings).state == LIBRARY_SCAN_DONE && cached.count == 1);
    library_scanner_destroy(s);
    assert(library_cache_save(&lib, root, cache));
    FILE *f = fopen(cache, "rb"); assert(f); assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); fclose(f);
    assert(!truncate(cache, n - 1));
    assert(!library_cache_load(&cached, root, cache) && cached.count == 1); /* invalid retains prior */
    assert(library_cache_save(&lib, root, cache));
    assert(!library_cache_load(&cached, "/wrong/root", cache));
    /* A syntactically complete index with a duplicate ID is invalid. */
    Game original = lib.games[1]; lib.games[1] = lib.games[0];
    assert(library_cache_save(&lib, root, cache));
    assert(!library_cache_load(&cached, root, cache)); lib.games[1] = original;
    assert(library_cache_save(&lib, root, cache));
    f = fopen(cache, "ab"); assert(f); assert(fputc('x', f) == 'x'); assert(!fclose(f));
    assert(!library_cache_load(&cached, root, cache));
    lib.games[0].core = NP_CORES;
    assert(!library_cache_save(&lib, root, cache));
    library_free(&lib); library_free(&cached);
    puts("cache: complete validation, cold start reconciliation and vanished launch");
}
static void unicode_bounds_test(const char *root)
{
    char name[200] = "a";
    for (int i=0; i<54; ++i) strcat(name, "夜");
    folder(root, name, name);
    char path[NP_PATH_MAX]; path_join(path, sizeof(path), root, name); Game game;
    assert(library_read_folder(path, &game));
    assert(strlen(game.name) == 157 && strlen(game.title) == 157);
    assert(library_game_valid(&game, root));
    char file[NP_PATH_MAX]; path_join(file, sizeof(file), path, "RPG_RT.lmt"); assert(!unlink(file));
    path_join(file, sizeof(file), path, "game"); assert(make_dirs(file));
    path_join(file, sizeof(file), path, "game/script.rpy"); write_text(file, "label start:\n    return\n");
    char launcher[210]; snprintf(launcher, sizeof(launcher), "%s.sh", name);
    path_join(file, sizeof(file), path, launcher); write_text(file, "");
    assert(library_read_folder(path, &game));
    assert(strlen(game.title) == 157 && library_game_valid(&game, root));
    puts("unicode: long folder and launcher titles truncate only at codepoint boundaries");
}
static void view_test(void)
{
    Game games[4] = {
        {.id="a", .title="Zulu", .name="Folder alpha", .core=CORE_EASYRPG, .played=20},
        {.id="b", .title="Alpha", .name="second", .core=CORE_EASYRPG, .played=10},
        {.id="c", .title="夜の物語", .name="日本語", .core=CORE_RENPY, .played=30},
        {.id="d", .title="Beta", .name="other", .core=CORE_EASYRPG}
    };
    Library lib = {.games=games, .count=4}; Game before[4]; memcpy(before, games, sizeof(games));
    LibraryView view = {0}; LibraryFilter filter = {.section=-1}; UserMetadata metadata = {0};
    Settings settings = {.last_game=2}; library_restore_selection(&lib, &settings);
    assert(library_view_build(&view, &lib, &metadata, &filter, 0));
    assert(view.count == 4 && view.indices[0] == 1 && view.indices[1] == 3 && view.indices[2] == 0);
    /* Startup first rebuilds an empty view while the asynchronous scan runs. */
    Library empty = {0}; LibraryView pending = {0}; Settings saved = {.last_game=1};
    assert(library_view_build(&pending, &empty, &metadata, &filter, 0));
    assert(library_view_restore_selection(&pending, &empty, &saved) == -1);
    assert(saved.last_game == 1 && !saved.selected_game_id[0]);
    assert(library_view_build(&pending, &lib, &metadata, &filter, 0));
    assert(library_view_restore_selection(&pending, &lib, &saved) == 1);
    assert(!strcmp(saved.selected_game_id, "d") && saved.last_game == 3);
    library_view_free(&pending);
    Settings legacy = {.last_game=0};
    library_view_restore_selection(&view, &lib, &legacy);
    assert(!strcmp(legacy.selected_game_id, "b") && legacy.last_game == 1);

    assert(library_view_selection(&view, &lib, &settings) == 3);
    assert(library_view_build(&view, &lib, &metadata, &filter, 1));
    assert(view.indices[0] == 0 && view.indices[1] == 1);
    assert(user_metadata_set_favorite(&metadata, "c", true));
    filter.favorites_only = true;
    assert(library_view_build(&view, &lib, &metadata, &filter, 0) && view.count == 1 && view.indices[0] == 2);
    filter.favorites_only = false; filter.recent_only = true;
    assert(library_view_build(&view, &lib, &metadata, &filter, 0) && view.count == 3);
    filter.recent_only = false; strcpy(filter.search, "ALPHA");
    assert(library_view_build(&view, &lib, &metadata, &filter, 0) && view.count == 2);
    assert(library_view_selection(&view, &lib, &settings) == 0 && !strcmp(settings.selected_game_id,"c"));
    strcpy(filter.search, "夜の");
    assert(library_view_build(&view, &lib, &metadata, &filter, 0) && view.count == 1 && view.indices[0] == 2);
    strcpy(filter.search, "\xA4\x9C"); /* tail of 夜: never match inside UTF-8 */
    assert(library_view_build(&view, &lib, &metadata, &filter, 0) && view.count == 0);
    filter.search[0] = 0; filter.section=SECTION_RENPY;
    assert(library_view_build(&view, &lib, &metadata, &filter, 0) && view.count == 1);
    assert(!memcmp(before, games, sizeof(games)));
    lib.count = 0;
    assert(library_view_build(&view, &lib, &metadata, &filter, 0) && view.count == 0);
    assert(library_view_selection(&view, &lib, &settings) == -1);
    library_view_free(&view); user_metadata_free(&metadata);
    puts("view: immutable indexes, favorites, recents, sections, UTF-8 search, selection");
}
static void picture(const char *root, const char *name, unsigned char red)
{
    folder(root, name, name);
    char dir[NP_PATH_MAX], path[NP_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/RPG_RT.ldb", root, name);
    FILE *database = fopen(path, "wb"); assert(database);
    const char ldb[] = "\x0b" "LcfDataBase\x1d\x00\x16\x01\x00";
    assert(fwrite(ldb, 1, sizeof(ldb)-1, database) == sizeof(ldb)-1); assert(!fclose(database));
    snprintf(dir, sizeof(dir), "%s/%s/Title", root, name); assert(make_dirs(dir));
    path_join(path, sizeof(path), dir, "cover.xyz");
    size_t size = 768 + 512 * 512;
    unsigned char *raw = calloc(1, size); assert(raw); raw[0] = red;
    mz_ulong len = mz_compressBound(size); unsigned char *compressed = malloc(len); assert(compressed);
    assert(mz_compress(compressed, &len, raw, size) == MZ_OK); free(raw);
    FILE *f = fopen(path, "wb"); assert(f);
    const unsigned char header[] = {'X','Y','Z','1',0,2,0,2};
    assert(fwrite(header, 1, 8, f) == 8); assert(fwrite(compressed, 1, len, f) == len); assert(!fclose(f));
    free(compressed);
}
static void covers_test(const char *root)
{
    const uint8_t huge_xyz[] = {'X','Y','Z','1',255,255,255,255};
    int width=0, height=0; assert(!covers_decode(huge_xyz, sizeof(huge_xyz), &width, &height));
    for (int i = 0; i < 24; ++i) { char name[24]; snprintf(name, sizeof(name), "cover%02d", i); picture(root, name, (unsigned char)(i+1)); }
    Library lib = {0}; library_scan(&lib, root); library_sort(&lib, 0); lib.generation=21;
    assert(lib.count == 24);
    Game before = lib.games[14];
    covers_start(&lib);
    int visible[] = {14, 0, 23, 10, 11, 12, 13, 15, 16, 17, 18};
    covers_update_visible(visible, 11);
    /* Mutating/freeing caller storage cannot redirect pending work. */
    memset(lib.games, 0, (size_t)lib.count * sizeof(Game)); library_free(&lib);
    uint64_t end = SDL_GetTicks64() + 10000;
    bool got = false;
    do {
        int was = uploads; uint64_t t = SDL_GetPerformanceCounter();
        covers_update_visible(visible, 11);
        measured(&cover_timing, t);
        assert(uploads-was <= 1);
        CoverStats stats = covers_stats();
        assert(stats.decoded_bytes <= COVERS_DECODED_BUDGET && stats.texture_bytes <= COVERS_TEXTURE_BUDGET);
        assert(stats.last_upload_bytes <= COVERS_UPLOAD_BUDGET && stats.snapshot_generation == 21);
        if (covers_get(14)) { assert(covers_get(14)->red == 15); got = true; }
        SDL_Delay(2);
    } while (SDL_GetTicks64() < end && uploads < 8);
    assert(got && uploads >= 3 && first_red == 15);
    int engine = 0;
    engine = covers_engine(14);
    assert(engine == 2003);
    /* Pending old work must never publish into a new generation's slot. */
    Library replacement = {.games=&before, .count=1, .generation=22};
    before.location[0] = 0; before.path[0] = 0;
    covers_start(&replacement);
    for (int i=0; i<100; ++i) { covers_update(0); SDL_Delay(1); assert(!covers_get(0)); }
    assert(covers_missing(0) && covers_stats().snapshot_generation == 22);
    covers_stop();
    /* Worker lifecycle may restart after teardown. */
    covers_start(&replacement); covers_update(0);
    for (int i=0; i<100 && !covers_missing(0); ++i) { SDL_Delay(1); covers_update(0); }
    assert(covers_missing(0)); covers_stop();
    /* Force an old worker to wait inside real filesystem I/O while publishing
     * a replacement. Its missing-cover completion must not mark the new slot. */
    picture(root, "blocked", 99); picture(root, "replacement", 77);
    char path[NP_PATH_MAX], location[NP_PATH_MAX];
    snprintf(path, sizeof(path), "%s/blocked/Title/cover.xyz", root);
    assert(!unlink(path)); assert(!mkfifo(path, 0600));
    Game blocked, fresh;
    path_join(location, sizeof(location), root, "blocked"); assert(library_read_folder(location, &blocked));
    path_join(location, sizeof(location), root, "replacement"); assert(library_read_folder(location, &fresh));
    Library old = {.games=&blocked, .count=1, .generation=30};
    covers_start(&old); covers_update(0);
    for (int i=0; i<1000 && !covers_stats().working; ++i) SDL_Delay(1);
    assert(covers_stats().working); SDL_Delay(20);
    Library newer = {.games=&fresh, .count=1, .generation=31};
    covers_start(&newer); covers_update(0);
    int writer = -1;
    for (int i=0; i<1000 && writer < 0; ++i) { writer = open(path, O_WRONLY | O_NONBLOCK); if (writer < 0) SDL_Delay(1); }
    assert(writer >= 0); close(writer);
    for (int i=0; i<1000 && !covers_get(0); ++i) { SDL_Delay(1); covers_update(0); }
    assert(covers_get(0) && covers_get(0)->red == 77 && covers_stats().snapshot_generation == 31);
    covers_stop();
    report_time("cover update (test texture backend)", &cover_timing);
    puts("covers: owned copies, visible priority, generation swap (including blocked old worker), bounded uploads/cache, restart");
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    if (!strcmp(argv[2], "covers")) {
        covers_test(argv[1]);
    } else {
        if (!strcmp(argv[2], "scan")) scan_test(argv[1]);
        else if (!strcmp(argv[2], "cache")) cache_test(argv[1]);
        else if (!strcmp(argv[2], "nested-renpy")) nested_renpy_test(argv[1]);
        else if (!strcmp(argv[2], "unicode")) unicode_bounds_test(argv[1]);
        else view_test();
    }
    puts("PASS"); return 0;
}
