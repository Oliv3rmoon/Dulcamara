/*
 * Dulcamara - Ren'Py 8 as a libretro core: the libretro API (renamed renpy_retro_* in the PS5 build, as
 * tools/build-renpy.sh does). Ren'Py runs on its own thread (python.c); each retro_run hands it one frame's time and
 * takes back the picture it drew (handoff.c, frames.c), and the frontend's sound callback plays its mixer (audio.c).
 * The engine is <core assets directory>/renpy8, and the saves <save directory>/saves/renpy. Ren'Py 7's core is the
 * same file, built by tools/build-renpy7.sh with python2.c and DR_ENGINE_FOLDER "renpy7".
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "libretro.h"

#include "../dulcamara_libretro.h"
#include "audio.h"
#include "frames.h"
#include "handoff.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "python.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DR_ENGINE_FOLDER
#define DR_ENGINE_FOLDER "renpy8" /* in the core assets folder */
#define DR_RENPY_VERSION "8.5.3"
#endif

#define FRAME_USEC 16667
#define SOUND_FRAMES 800 /* a frame's worth at 48 kHz */

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;
static retro_usec_t frame_usec = FRAME_USEC;
static bool loaded, shutdown_sent, sound_by_callback, bitmasks;
static int picture_w, picture_h;
static struct retro_dulcamara_text_input text_told; /* what the frontend was last told of the text Ren'Py wants */

static void emit(int level, const char *line)
{
    if (log_cb)
        log_cb(level >= 2 ? RETRO_LOG_WARN : RETRO_LOG_INFO, "%s\n", line);
    else
    {
        /* straight to the descriptor: on the PS5 the core's fprintf is ps5_console.c's, which logs through here */
        (void)!write(STDERR_FILENO, line, strlen(line));
        (void)!write(STDERR_FILENO, "\n", 1);
    }
}

static void frame_time(retro_usec_t usec)
{
    frame_usec = usec > 0 ? usec : FRAME_USEC;
}

static void sound(void)
{
    int16_t samples[SOUND_FRAMES * 2];
    audio_render(samples, SOUND_FRAMES);
    if (audio_batch_cb)
        audio_batch_cb(samples, SOUND_FRAMES);
}

static void sound_state(bool enabled)
{
    (void)enabled;
}

static bool is_folder(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* path and the folders above it, as mkdir -p */
static bool make_folders(const char *path)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; ++s)
        if (*s == '/')
        {
            *s = '\0';
            if (mkdir(p, 0777) != 0 && errno != EEXIST)
                return false;
            *s = '/';
        }
    return mkdir(p, 0777) == 0 || errno == EEXIST;
}

static void read_keys(void); /* below, with the controller */

/* A tester's switches: each DULCAMARA_<NAME>=<value> line of <the data folder>/renpy-test.txt is set for the game
 * (the core's own settings set after it still win), in the log as it is: graphics and sound choices the core makes
 * on the console (DULCAMARA_GL_CPU_LOAD, DULCAMARA_GL_WRITE_BACK, DULCAMARA_GL_READ_DIRECT, DULCAMARA_MIPMAPS, ...)
 * can be tried one by one on a console without a build of their own. */
static void read_test_switches(const char *folder)
{
    char path[1100], line[512];
    snprintf(path, sizeof(path), "%s/renpy-test.txt", folder);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    for (int n = 0; n < 64 && fgets(line, sizeof(line), f); ++n)
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *equals = strchr(line, '=');
        if (!equals || strncmp(line, "DULCAMARA_", 10))
            continue;
        *equals = '\0';
        bool name_ok = true;
        for (const char *c = line; *c && name_ok; ++c)
            name_ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!name_ok)
            continue;
        setenv(line, equals + 1, 1);
        log_queue(1, "renpy: renpy-test.txt sets %s=%s", line, equals + 1);
    }
    fclose(f);
}

/* The frontend's on-screen keyboard (RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK), while Ren'Py wants text: a character
 * is Ren'Py's text; a key the input's own (Backspace, Return to confirm, the arrows, Home and End). */
static void keyboard(bool down, unsigned keycode, uint32_t character, uint16_t modifiers)
{
    (void)modifiers;
    if (character)
    {
        if (down)
            input_type_text(character);
        return;
    }
    static const struct
    {
        unsigned keycode;
        const char *name; /* input_bind's */
    } KEYS[] = {
        {RETROK_BACKSPACE, "backspace"}, {RETROK_RETURN, "return"}, {RETROK_KP_ENTER, "return"},
        {RETROK_DELETE, "delete"},       {RETROK_LEFT, "left"},     {RETROK_RIGHT, "right"},
        {RETROK_HOME, "home"},           {RETROK_END, "end"},
    };
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); ++i)
        if (KEYS[i].keycode == keycode)
            input_type_key(KEYS[i].name, down);
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    /* the core's options: Dulcamara's Settings > Ren'Py, a game's keys (each RetroPad button's action, input.h), and
     * where its sounds' two files are (Sounds for this game: the path without .list and .txt, none by default) */
    static const struct retro_variable variables[] = {
        {"renpy_skip_unseen", "Fast-forward skips unread text too; off|on"},
        {"renpy_sounds", "Sounds for this game, the path of its list and its sounds off; "},
        {"renpy_key_b", "Cross; default"},       {"renpy_key_y", "Square; default"},
        {"renpy_key_select", "Create; default"}, {"renpy_key_start", "Options; default"},
        {"renpy_key_up", "Up; default"},         {"renpy_key_down", "Down; default"},
        {"renpy_key_left", "Left; default"},     {"renpy_key_right", "Right; default"},
        {"renpy_key_a", "Circle; default"},      {"renpy_key_x", "Triangle; default"},
        {"renpy_key_l", "L1; default"},          {"renpy_key_r", "R1; default"},
        {"renpy_key_l2", "L2; default"},         {"renpy_key_l3", "L3; default"},
        {"renpy_key_r3", "R3; default"},         {NULL, NULL},
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)variables);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
    (void)cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

void retro_init(void)
{
    struct retro_log_callback l;
    log_cb = environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &l) ? l.log : NULL;
    log_set_direct(emit);
}

void retro_deinit(void)
{
    log_cb = NULL;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "Ren'Py";
    info->library_version = DR_RENPY_VERSION;
    info->valid_extensions = "rpa|rpyc|rpy";
    info->need_fullpath = true;
    info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *av)
{
    memset(av, 0, sizeof(*av));
    av->geometry.base_width = picture_w ? (unsigned)picture_w : 1280;
    av->geometry.base_height = picture_h ? (unsigned)picture_h : 720;
    av->geometry.max_width = DR_MAX_W;
    av->geometry.max_height = DR_MAX_H;
    av->geometry.aspect_ratio = (float)av->geometry.base_width / (float)av->geometry.base_height;
    av->timing.fps = 60.0;
    av->timing.sample_rate = DR_AUDIO_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port, (void)device;
}

void retro_reset(void)
{
}

bool retro_load_game(const struct retro_game_info *info)
{
    if (!info || !info->path || !environ_cb)
        return false;
    const char *assets = NULL, *save = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY, &assets) || !assets)
    {
        emit(2, "renpy: the frontend gave no core assets folder, where the engine (" DR_ENGINE_FOLDER "/) is");
        return false;
    }
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) || !save)
        environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &save);
    char engine[1024], marker[1100], game_folder[1100], saves[1024];
    snprintf(engine, sizeof(engine), "%s/" DR_ENGINE_FOLDER, assets);
    snprintf(marker, sizeof(marker), "%s/renpy/__init__.py", engine);
    snprintf(game_folder, sizeof(game_folder), "%s/game", info->path);
    snprintf(saves, sizeof(saves), "%s/saves/renpy", save ? save : info->path);
    log_step("loading %s, engine %s", info->path, engine);
    struct stat st;
    if (stat(marker, &st) != 0)
    {
        log_queue(2, "renpy: no engine in %s", engine);
        log_flush(emit);
        return false;
    }
    if (!is_folder(game_folder))
    {
        log_queue(2, "renpy: %s has no game folder", info->path);
        log_flush(emit);
        return false;
    }
    if (!make_folders(saves))
        log_queue(2, "renpy: cannot make %s", saves);

    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
        return false;
    struct retro_frame_time_callback timing = {frame_time, FRAME_USEC};
    environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &timing);
    struct retro_audio_callback audio = {sound, sound_state};
    sound_by_callback = environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio);
    bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
    struct retro_keyboard_callback typing = {keyboard};
    environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &typing);
    memset(&text_told, 0, sizeof(text_told));
    frame_usec = FRAME_USEC;
    picture_w = picture_h = 0;
    shutdown_sent = false;

    char error[256];
    log_step("the frontend's picture, clock and sound are set");
    read_test_switches(save ? save : info->path);
    /* read by the engine's dulcamara_errors, which holds Ren'Py's preference to it */
    struct retro_variable skip_unseen = {"renpy_skip_unseen", NULL};
    const bool skip_all = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &skip_unseen) && skip_unseen.value &&
                          !strcmp(skip_unseen.value, "on");
    setenv("DULCAMARA_SKIP_UNSEEN", skip_all ? "1" : "0", 1);
    /* and the sounds' files: dulcamara_errors lists the game's sounds in <it>.list and plays nothing for those
     * <it>.txt has off */
    struct retro_variable sounds = {"renpy_sounds", NULL};
    const char *sounds_base =
        environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &sounds) && sounds.value ? sounds.value : "";
    setenv("DULCAMARA_SOUNDS", sounds_base, 1);
    /* and the logs' folder beside the frontend's dulcamara.log, where dulcamara_errors keeps Ren'Py's errors */
    char logs[1100];
    snprintf(logs, sizeof(logs), "%s/logs", save ? save : info->path);
    setenv("DULCAMARA_LOGS", logs, 1);
    if (*sounds_base)
        log_queue(1, "renpy: the game's sounds are listed in %s.list, the ones off in %s.txt", sounds_base,
                  sounds_base);
    read_keys();
    log_step("starting Ren'Py's thread");
    if (!python_start(engine, info->path, saves, error, sizeof(error)))
    {
        emit(2, error);
        return false;
    }
    log_step("Ren'Py's thread started; the game is loaded");
    log_queue(1, "renpy: %s, engine %s, saves %s", info->path, engine, saves);
    loaded = true;
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count)
{
    (void)type, (void)info, (void)count;
    return false;
}

void retro_unload_game(void)
{
    if (!loaded)
        return;
    python_stop();
    log_flush(emit);
    loaded = false;
}

/* each RetroPad button's renpy_key_* option (R2 is the frontend's fast-forward), given to input.c */
static void read_keys(void)
{
    static const char *const NAMES[16] = {"b", "y",  "select", "start", "up", "down", "left", "right",
                                          "a", "x",  "l",      "r",     "l2", NULL,   "l3",   "r3"};
    for (unsigned b = 0; b < 16; ++b)
    {
        if (!NAMES[b])
            continue;
        char key[32];
        snprintf(key, sizeof(key), "renpy_key_%s", NAMES[b]);
        struct retro_variable v = {key, NULL};
        const char *value = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) ? v.value : NULL;
        if (!input_bind(b, value))
            log_queue(2, "renpy: %s = %s is not a key the core knows; the button keeps its own", key, value);
        else if (value && *value && strcmp(value, "default"))
            log_queue(1, "renpy: %s = %s", key, value);
    }
}

static uint16_t pad_buttons(void)
{
    if (!input_state_cb)
        return 0;
    if (bitmasks)
        return (uint16_t)input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    uint16_t bits = 0;
    for (unsigned id = 0; id < 16; ++id)
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id))
            bits |= (uint16_t)(1u << id);
    return bits;
}

/* The picture for the frontend: Ren'Py's newest, with the cursor on it while it is shown (a copy, redrawn as the
 * cursor moves: Ren'Py does not draw again for a pointer that moves over nothing); NULL for the same as before. */
static const uint32_t *picture_with_cursor(const uint32_t *picture, int w, int h, bool fresh)
{
    static uint32_t *copy;
    static size_t capacity;
    static bool drawn;
    static int drawn_x, drawn_y;
    int x = 0, y = 0;
    const bool shown = picture && input_cursor(w, h, &x, &y);
    if (!shown)
    {
        const bool restore = drawn && picture;
        drawn = false;
        return fresh || restore ? picture : NULL;
    }
    if (!fresh && drawn && x == drawn_x && y == drawn_y)
        return NULL;
    const size_t n = (size_t)w * (size_t)h;
    if (n > capacity)
    {
        uint32_t *grown = realloc(copy, n * sizeof(uint32_t));
        if (!grown)
            return fresh ? picture : NULL;
        copy = grown;
        capacity = n;
    }
    memcpy(copy, picture, n * sizeof(uint32_t));
    frames_draw_cursor(copy, w, h, x, y);
    drawn = true;
    drawn_x = x;
    drawn_y = y;
    return copy;
}

void retro_run(void)
{
    if (input_poll_cb)
        input_poll_cb();
    bool keys_changed = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &keys_changed) && keys_changed)
        read_keys();
    const int16_t lx = input_state_cb ? input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                       RETRO_DEVICE_ID_ANALOG_X)
                                      : 0;
    const int16_t ly = input_state_cb ? input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                       RETRO_DEVICE_ID_ANALOG_Y)
                                      : 0;
    const uint16_t buttons = pad_buttons();
    static uint16_t last_buttons;
    static int button_changes;
    if (buttons != last_buttons && button_changes < 12)
    {
        ++button_changes;
        log_queue(1, "renpy: the frontend's buttons: 0x%04x", buttons);
    }
    last_buttons = buttons;
    input_set(buttons, lx, ly);
    bool fast = false;
    const bool skip = environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &fast) && fast;
    static bool last_skip;
    static int skip_changes;
    if (skip != last_skip && skip_changes < 40)
    {
        ++skip_changes;
        log_queue(1, "renpy: fast-forward %s (Ren'Py's skip)", skip ? "on" : "off");
    }
    last_skip = skip;
    input_set_skip(skip);
    if (input_state_cb)
        input_set_pointer(input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X),
                          input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y),
                          input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0);
    handoff_frame(frame_usec, frame_usec * 3 / 4);
    log_flush(emit);
    /* the text Ren'Py wants, as it changes: the frontend shows its keyboard for it, away from the game's own input */
    struct retro_dulcamara_text_input text = {false, 0, 0};
    text.wanted = input_text_wanted(&text.top, &text.bottom);
    if (text.wanted != text_told.wanted || text.top != text_told.top || text.bottom != text_told.bottom)
    {
        text_told = text;
        environ_cb(RETRO_ENVIRONMENT_DULCAMARA_TEXT_INPUT, &text);
    }

    int w = 0, h = 0;
    bool fresh = false;
    const uint32_t *picture = frames_take(&w, &h, &fresh);
    if (picture && (w != picture_w || h != picture_h))
    {
        picture_w = w;
        picture_h = h;
        struct retro_game_geometry geometry = {(unsigned)w, (unsigned)h, DR_MAX_W, DR_MAX_H, (float)w / (float)h};
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
    }
    if (video_cb)
        video_cb(picture_with_cursor(picture, w, h, fresh), w ? (unsigned)w : 1280, h ? (unsigned)h : 720,
                 (size_t)(w ? w : 1280) * sizeof(uint32_t));
    if (!sound_by_callback)
        sound();
    if (handoff_engine_ended() && !shutdown_sent)
    {
        shutdown_sent = true;
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

size_t retro_serialize_size(void)
{
    return 0; /* Ren'Py's own saves */
}

bool retro_serialize(void *data, size_t size)
{
    (void)data, (void)size;
    return false;
}

bool retro_unserialize(const void *data, size_t size)
{
    (void)data, (void)size;
    return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index, (void)enabled, (void)code;
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    (void)id;
    return 0;
}
