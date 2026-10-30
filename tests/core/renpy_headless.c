/*
 * Dulcamara - Ren'Py 8's libretro core, headless: a frontend with no screen and no sound card that loads a Ren'Py
 * game, plays it with the controller as a player would, and reports what came out. tests/test_renpy_core.py runs it
 * on tests/renpy-game. The game says where it is in its log ("DULCAMARA-TEST: ..."), and the frontend acts on that.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   renpy-headless <assets folder> play|load|start|error <game folder> <data folder> [--then <game folder>]
 *
 *   play   Cross through the dialogue, Down and Cross to choose Right; the game saves and quits.
 *   load   the game loads that save (DULCAMARA_TEST_LOAD=1); Cross, and it quits.
 *   start  the game starts and shows a picture; it is then closed while it waits.
 *   skip   with fast-forward on from the start, the dialogue goes by without Cross (DULCAMARA_TEST_SKIP=1); then
 *          fast-forward is off, and Down and Cross choose in the menu as in "play".
 *   error  the game has a script error: Ren'Py's error is logged and shown; it is then closed.
 *   --then a second game in the same process afterwards, as "start".
 */
#include "libretro.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 7200 /* two minutes of the game's time for each step */

static const char *assets_dir, *data_dir;
static struct retro_audio_callback audio_cb;
static struct retro_frame_time_callback frame_time_cb;
static bool audio_cb_set, frame_time_set, shutdown_asked, xrgb, fast_forward;
static unsigned picture_w, picture_h, geometry_w, geometry_h;
static uint64_t picture_hash, with_picture, dupes, distinct, sound_frames, sound_loud, refreshes;
static uint16_t buttons;
static char log_text[1 << 18];
static size_t log_used;
static int ran;

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    char line[4096];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    const size_t n = strlen(line);
    if (log_used + n + 1 < sizeof(log_text))
    {
        memcpy(log_text + log_used, line, n + 1);
        log_used += n;
    }
    if (getenv("RENPY_HEADLESS_VERBOSE") || strstr(line, "DULCAMARA-TEST: ") || strstr(line, "Ren'Py reported") ||
        strstr(line, ".rpy") || strstr(line, "Dulcamara starts"))
        printf("log: %s", line);
}

static bool logged(const char *what)
{
    return strstr(log_text, what) != NULL;
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        xrgb = *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
        return xrgb;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = data_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *(const char **)data = assets_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        audio_cb = *(const struct retro_audio_callback *)data;
        audio_cb_set = audio_cb.callback != NULL;
        return true;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        frame_time_cb = *(const struct retro_frame_time_callback *)data;
        frame_time_set = frame_time_cb.callback != NULL;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        geometry_w = ((const struct retro_game_geometry *)data)->base_width;
        geometry_h = ((const struct retro_game_geometry *)data)->base_height;
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        shutdown_asked = true;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = fast_forward;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data)
            *(bool *)data = true;
        return true;
    default:
        return false;
    }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    ++refreshes;
    if (!data)
    {
        ++dupes;
        return;
    }
    ++with_picture;
    uint64_t hash = 1469598103934665603ull;
    for (unsigned y = 0; y < height; ++y)
    {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)data + y * pitch);
        for (unsigned x = 0; x < width; ++x)
            hash = (hash ^ (row[x] & 0xffffffu)) * 1099511628211ull;
    }
    if (hash != picture_hash)
        ++distinct;
    picture_hash = hash;
    picture_w = width;
    picture_h = height;
}

static void audio_sample(int16_t left, int16_t right)
{
    (void)left, (void)right;
    ++sound_frames;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    for (size_t i = 0; i < frames * 2; ++i)
        if (data[i] > 256 || data[i] < -256)
        {
            ++sound_loud;
            break;
        }
    sound_frames += frames;
    return frames;
}

static void input_poll(void)
{
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)buttons;
    return id < 16 ? (buttons >> id) & 1 : 0;
}

static void frame(void)
{
    if (frame_time_set)
        frame_time_cb.callback(frame_time_cb.reference);
    retro_run();
    if (audio_cb_set)
        audio_cb.callback();
    ++ran;
}

static bool until_logged(const char *what)
{
    for (int i = 0; i < MAX_FRAMES && !logged(what) && !shutdown_asked; ++i)
        frame();
    if (!logged(what))
        printf("FAIL: never logged \"%s\"\n", what);
    return logged(what);
}

static bool until_picture(void)
{
    for (int i = 0; i < MAX_FRAMES && !with_picture && !shutdown_asked; ++i)
        frame();
    if (!with_picture)
        printf("FAIL: no picture\n");
    return with_picture > 0;
}

static bool until_shutdown(void)
{
    for (int i = 0; i < MAX_FRAMES && !shutdown_asked; ++i)
        frame();
    if (shutdown_asked)
        printf("ended by the game\n");
    else
        printf("FAIL: the game did not end\n");
    return shutdown_asked;
}

static void frames(int n)
{
    for (int i = 0; i < n && !shutdown_asked; ++i)
        frame();
}

static void press(unsigned id)
{
    buttons = (uint16_t)(1u << id);
    frames(6);
    buttons = 0;
}

static bool load(const char *game)
{
    with_picture = dupes = distinct = refreshes = sound_frames = sound_loud = 0;
    picture_hash = 0;
    shutdown_asked = false;
    log_used = 0;
    log_text[0] = '\0';
    ran = 0;
    struct retro_game_info info = {game, NULL, 0, NULL};
    if (!retro_load_game(&info))
    {
        printf("the core did not load %s\n", game);
        return false;
    }
    if (audio_cb_set && audio_cb.set_state)
        audio_cb.set_state(true);
    return true;
}

static void report(void)
{
    printf("frames: %d run, %llu with a new picture, %llu the same picture again, %llu different pictures\n", ran,
           (unsigned long long)with_picture, (unsigned long long)dupes, (unsigned long long)distinct);
    printf("picture: %ux%u, %s\n", picture_w, picture_h, xrgb ? "XRGB8888" : "another pixel format");
    printf("sound: %s (%llu frames, %llu batches with sound in them)\n", sound_loud > 10 ? "playing" : "silent",
           (unsigned long long)sound_frames, (unsigned long long)sound_loud);
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <assets folder> play|load|start|error <game folder> <data folder> [--then <game>]\n",
                argv[0]);
        return 2;
    }
    assets_dir = argv[1];
    const char *mode = argv[2], *game = argv[3];
    data_dir = argv[4];
    const char *then = argc > 6 && !strcmp(argv[5], "--then") ? argv[6] : NULL;

    struct retro_system_info info;
    retro_get_system_info(&info);
    printf("core: %s %s\n", info.library_name, info.library_version);
    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    bool ok = load(game);
    if (!ok)
        printf("FAIL: the first game did not load\n");
    bool changed = true, ended = true;
    if (ok && !strcmp(mode, "play"))
    {
        ok = until_logged("DULCAMARA-TEST: started") && until_picture();
        frames(60);
        const uint64_t before = picture_hash;
        press(RETRO_DEVICE_ID_JOYPAD_B); /* Cross */
        ok = ok && until_logged("DULCAMARA-TEST: advanced");
        frames(60);
        changed = picture_hash != before;
        printf("input: the picture %s after Cross\n", changed ? "changed" : "did not change");
        press(RETRO_DEVICE_ID_JOYPAD_DOWN);
        frames(30);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ok = ok && until_logged("DULCAMARA-TEST: choice=") && until_logged("DULCAMARA-TEST: saved");
        frames(30);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ended = until_shutdown();
    }
    else if (ok && !strcmp(mode, "skip"))
    {
        fast_forward = true;
        ok = until_logged("DULCAMARA-TEST: started") && until_picture() && until_logged("DULCAMARA-TEST: advanced");
        printf("fast-forward: the dialogue %s without Cross\n", ok ? "went by" : "did not go by");
        fast_forward = false;
        frames(30);
        press(RETRO_DEVICE_ID_JOYPAD_DOWN);
        frames(30);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ok = ok && until_logged("DULCAMARA-TEST: choice=");
    }
    else if (ok && !strcmp(mode, "load"))
    {
        ok = until_logged("DULCAMARA-TEST: loaded") && until_logged("DULCAMARA-TEST: choice=") && until_picture();
        frames(60);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ended = until_shutdown();
    }
    else if (ok && (!strcmp(mode, "start") || !strcmp(mode, "error")))
    {
        ok = (!strcmp(mode, "error") ? until_logged("Ren'Py reported an error") : until_logged("DULCAMARA-TEST: started"))
             && until_picture();
        frames(60);
    }
    else if (ok)
    {
        printf("FAIL: no mode %s\n", mode);
        ok = false;
    }
    if (geometry_w)
        printf("geometry: %ux%u\n", geometry_w, geometry_h);
    report();
    retro_unload_game();
    printf("stopped\n");

    const bool played = !strcmp(mode, "play") || !strcmp(mode, "load");
    bool verdict = ok && changed && ended;
    if (refreshes != (uint64_t)ran)
        verdict = false, printf("FAIL: %llu frames called video_refresh, of %d\n", (unsigned long long)refreshes, ran);
    if (!xrgb)
        verdict = false, printf("FAIL: the core did not ask for XRGB8888\n");
    if (distinct < (played ? 2u : 1u))
        verdict = false, printf("FAIL: the picture hardly changed\n");
    if (!strcmp(mode, "play") && sound_loud <= 10)
        verdict = false, printf("FAIL: no sound\n");

    if (then)
    {
        /* A core may refuse a second game in one process; then it must say why. */
        if (load(then))
        {
            const bool second = until_logged("DULCAMARA-TEST: started") && until_picture();
            frames(60);
            const unsigned w = picture_w, h = picture_h;
            retro_unload_game(); /* the core's last log lines are printed here: the report comes after them */
            printf("second game: %s, picture %ux%u, stopped\n", second ? "started" : "FAILED", w, h);
            verdict = verdict && second;
        }
        else
        {
            const bool said_why = log_used > 0;
            printf("second game: refused%s\n", said_why ? "" : ", without a reason in the log");
            verdict = verdict && said_why;
        }
    }
    retro_deinit();
    printf(verdict ? "PASS\n" : "FAIL\n");
    return verdict ? 0 : 1;
}
