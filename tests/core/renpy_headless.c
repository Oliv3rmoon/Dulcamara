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
 *   compile  Ren'Py's compile command (DULCAMARA_RENPY_COMMAND=compile): Ren'Py compiles the scripts and ends.
 *   click  a press of the pointer (the touchpad) in the middle of the picture moves the dialogue on, as a click.
 *   skip   with fast-forward on from the start, the dialogue goes by without Cross (DULCAMARA_TEST_SKIP=1); then
 *          fast-forward is off, and Down and Cross choose in the menu as in "play".
 *   keys   a game's own key on a button (renpy_key_l2=return, from the environment): L2 moves the dialogue on.
 *   cursor L3 turns the cursor on: it is drawn on the picture, the left stick moves it, and Cross clicks there.
 *   text   the game asks for a name (DULCAMARA_TEST_INPUT=1): the core says it wants text and where its input is,
 *          takes "Adz", a Backspace, "a" and Return through its keyboard callback as Dulcamara's on-screen keyboard
 *          gives them (core/host.c), and says it has its text; a Return typed then goes nowhere, and Cross is the
 *          game's again.
 *   error  the game has a script error: Ren'Py's error is logged and shown; it is then closed.
 *   --then a second game in the same process afterwards, as "start".
 */
#include "libretro.h"

#include "../../src/core/dulcamara_libretro.h"

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
static bool pointer_pressed; /* the pointer (the touchpad) pressed, at the middle of the picture */
static char log_text[1 << 18];
static size_t log_used;
static int ran;
/* typing: what the core says of the text it wants, its keyboard callback, and the presses held until it polls */
static struct retro_dulcamara_text_input text;
static int text_changes;
static retro_keyboard_event_t keyboard_cb;
static struct
{
    unsigned key;
    uint32_t character;
} typed[32];
static int typed_count;

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
        strstr(line, ".rpy") || strstr(line, "Dulcamara starts") || strstr(line, "not stop") || strstr(line, "File \"") ||
        strstr(line, "still running") || strstr(line, "keyboard"))
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
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        keyboard_cb = ((const struct retro_keyboard_callback *)data)->callback;
        return true;
    case RETRO_ENVIRONMENT_DULCAMARA_TEXT_INPUT:
        text = *(const struct retro_dulcamara_text_input *)data;
        ++text_changes;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = fast_forward;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data)
            *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* the core's options, as Dulcamara sets them: RENPY_HEADLESS_SKIP_UNSEEN=on, and a game's keys by their own
         * names (renpy_key_l2=return) */
        struct retro_variable *variable = data;
        if (!variable || !variable->key)
            return false;
        if (!strncmp(variable->key, "renpy_key_", 10))
        {
            variable->value = getenv(variable->key);
            return variable->value != NULL;
        }
        if (strcmp(variable->key, "renpy_skip_unseen"))
            return false;
        variable->value = getenv("RENPY_HEADLESS_SKIP_UNSEEN") ? getenv("RENPY_HEADLESS_SKIP_UNSEEN") : "off";
        return true;
    }
    default:
        return false;
    }
}

static const uint32_t *last_picture; /* the last picture given, valid until the next frame */
static unsigned last_w, last_h;
static size_t last_pitch;
static int16_t stick_x, stick_y; /* the left stick */

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    ++refreshes;
    if (data)
    {
        last_picture = data;
        last_w = width;
        last_h = height;
        last_pitch = pitch;
    }
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

/* as core/host.c gives the on-screen keyboard's presses: each down and up, as the core polls */
static void input_poll(void)
{
    for (int i = 0; i < typed_count && keyboard_cb; ++i)
    {
        keyboard_cb(true, typed[i].key, typed[i].character, 0);
        keyboard_cb(false, typed[i].key, 0, 0);
    }
    typed_count = 0;
}

static void type_key(unsigned key, uint32_t character)
{
    if (typed_count < (int)(sizeof(typed) / sizeof(typed[0])))
    {
        typed[typed_count].key = key;
        typed[typed_count++].character = character;
    }
}

static void type_text(const char *ascii)
{
    for (; *ascii; ++ascii)
        type_key(RETROK_UNKNOWN, (unsigned char)*ascii);
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port == 0 && device == RETRO_DEVICE_POINTER)
        return id == RETRO_DEVICE_ID_POINTER_PRESSED ? pointer_pressed : 0; /* x and y 0: the middle */
    if (port == 0 && device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
        return id == RETRO_DEVICE_ID_ANALOG_X ? stick_x : stick_y;
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

/* until the core says it wants text typed (or no longer does) */
static bool until_text(bool wanted)
{
    for (int i = 0; i < MAX_FRAMES && text.wanted != wanted && !shutdown_asked; ++i)
        frame();
    if (text.wanted != wanted)
        printf("FAIL: the core never said it %s text\n", wanted ? "wants" : "no longer wants");
    return text.wanted == wanted;
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
    memset(&text, 0, sizeof(text));
    text_changes = typed_count = 0;
    keyboard_cb = NULL;
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
    else if (ok && !strcmp(mode, "compile"))
    {
        ended = until_shutdown();
        ok = ended && !logged("Ren'Py reported an error");
    }
    else if (ok && !strcmp(mode, "click"))
    {
        ok = until_logged("DULCAMARA-TEST: started") && until_picture();
        frames(60);
        pointer_pressed = true;
        frames(6);
        pointer_pressed = false;
        ok = ok && until_logged("DULCAMARA-TEST: advanced");
        printf("pointer: the dialogue %s after a press\n", ok ? "moved on" : "did not move on");
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
    else if (ok && !strcmp(mode, "keys"))
    {
        ok = until_logged("DULCAMARA-TEST: started") && until_picture();
        frames(60);
        press(RETRO_DEVICE_ID_JOYPAD_L2);
        ok = ok && until_logged("DULCAMARA-TEST: advanced");
        printf("keys: the dialogue %s after L2 (Return)\n", ok ? "moved on" : "did not move on");
    }
    else if (ok && !strcmp(mode, "cursor"))
    {
        ok = until_logged("DULCAMARA-TEST: started") && until_picture();
        frames(60);
        press(RETRO_DEVICE_ID_JOYPAD_L3);
        frames(10);
        ok = ok && until_logged("the cursor is on");
        /* the arrow's tip in the middle of the picture: its outline black, white just inside it */
        const unsigned x = (last_w - 1) / 2 + ((last_w - 1) % 2), y = (last_h - 1) / 2 + ((last_h - 1) % 2);
        const uint32_t *row = last_picture ? (const uint32_t *)((const uint8_t *)last_picture + y * last_pitch) : NULL;
        const uint32_t *below = last_picture ? (const uint32_t *)((const uint8_t *)last_picture + (y + 2) * last_pitch)
                                             : NULL;
        const bool drawn = row && below && (row[x] & 0xFFFFFFu) == 0 && (below[x + 1] & 0xFFFFFFu) == 0xFFFFFFu;
        printf("cursor: %s on the picture\n", drawn ? "drawn" : "not drawn");
        stick_x = 32767;
        frames(20);
        stick_x = 0;
        frames(10);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ok = ok && drawn && until_logged("DULCAMARA-TEST: advanced");
        printf("cursor: the dialogue %s after a click with Cross\n", ok ? "moved on" : "did not move on");
    }
    else if (ok && !strcmp(mode, "text"))
    {
        ok = until_logged("DULCAMARA-TEST: started") && until_picture() && until_text(true);
        if (!keyboard_cb)
            ok = false, printf("FAIL: the core gave no keyboard callback\n");
        printf("text: the core asked for text, its input %.0f-%.0f %% down the picture\n", text.top * 100.0f,
               text.bottom * 100.0f);
        frames(10);
        type_text("Adz");
        frames(5);
        type_key(RETROK_BACKSPACE, 0);
        frames(5);
        type_text("a");
        frames(5);
        type_key(RETROK_RETURN, 0);
        ok = ok && until_logged("DULCAMARA-TEST: name=") && until_text(false);
        const bool named = logged("DULCAMARA-TEST: name=Ada\n");
        printf("text: the game %s\n", named ? "got the name typed (Ada)" : "got another name");
        /* typing once the game has its text: the core drops it (a Return here would be the next line's) */
        frames(10);
        type_key(RETROK_RETURN, 0);
        frames(90);
        const bool dropped = !logged("DULCAMARA-TEST: advanced");
        printf("text: a Return typed after the input ended %s\n", dropped ? "went nowhere" : "reached the game");
        press(RETRO_DEVICE_ID_JOYPAD_B);
        const bool advanced = until_logged("DULCAMARA-TEST: advanced");
        printf("text: Cross %s afterwards\n", advanced ? "moved the dialogue on" : "did not move the dialogue on");
        /* told as it changes, not at every frame: on, off, and a move of the input at most */
        ok = ok && named && dropped && advanced && text_changes < 10;
        if (text_changes >= 10)
            printf("FAIL: the core said what text it wants %d times\n", text_changes);
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
    const bool drew = strcmp(mode, "compile") != 0; /* compiling draws nothing */
    if (!xrgb)
        verdict = false, printf("FAIL: the core did not ask for XRGB8888\n");
    if (drew && distinct < (played ? 2u : 1u))
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
