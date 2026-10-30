/*
 * Dulcamara - Ren'Py 8 as a libretro core: the libretro API (renamed renpy_retro_* in the PS5 build, as
 * tools/build-renpy.sh does). Ren'Py runs on its own thread (python.c); each retro_run hands it one frame's time and
 * takes back the picture it drew (handoff.c, frames.c), and the frontend's sound callback plays its mixer (audio.c).
 * The engine is <core assets directory>/renpy8, and the saves <save directory>/saves/renpy. Ren'Py 7's core is the
 * same file, built by tools/build-renpy7.sh with python2.c and DR_ENGINE_FOLDER "renpy7".
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "libretro.h"

#include "audio.h"
#include "frames.h"
#include "handoff.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "python.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

static void emit(int level, const char *line)
{
    if (log_cb)
        log_cb(level >= 2 ? RETRO_LOG_WARN : RETRO_LOG_INFO, "%s\n", line);
    else
        fprintf(stderr, "%s\n", line);
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

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
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
    frame_usec = FRAME_USEC;
    picture_w = picture_h = 0;
    shutdown_sent = false;

    char error[256];
    if (!python_start(engine, info->path, saves, error, sizeof(error)))
    {
        emit(2, error);
        return false;
    }
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

void retro_run(void)
{
    if (input_poll_cb)
        input_poll_cb();
    const int16_t lx = input_state_cb ? input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                       RETRO_DEVICE_ID_ANALOG_X)
                                      : 0;
    const int16_t ly = input_state_cb ? input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                       RETRO_DEVICE_ID_ANALOG_Y)
                                      : 0;
    input_set(pad_buttons(), lx, ly);
    bool fast = false;
    input_set_skip(environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &fast) && fast);
    if (input_state_cb)
        input_set_pointer(input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X),
                          input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y),
                          input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0);
    handoff_frame(frame_usec, frame_usec * 3 / 4);
    log_flush(emit);

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
        video_cb(picture && fresh ? picture : NULL, w ? (unsigned)w : 1280, h ? (unsigned)h : 720,
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
