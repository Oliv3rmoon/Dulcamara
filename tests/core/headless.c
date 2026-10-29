/*
 * Dulcamara - EasyRPG Player's libretro core, headless: a frontend with no screen and no sound card that
 * loads a game, runs it for some frames, presses a few buttons and reports what came out. tests/test_core.py runs
 * it on EasyRPG's TestGame; it fails unless the game loads, every frame has a picture and the pictures change.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   easyrpg-headless <game folder or RPG_RT.ldb> <system folder> [frames] [another game]
 *
 * With another game, the first is unloaded and the second loaded and run in the same process, as the app does when
 * one game is quit and the next started.
 */
#include "libretro.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *system_dir;
static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_audio_callback audio_cb;
static struct retro_frame_time_callback frame_time_cb;
static bool audio_cb_set, frame_time_set, shutdown_asked;
static unsigned frames_drawn, frames_null, last_w, last_h;
static uint64_t last_hash, distinct_pictures;
static unsigned last_colours;
static bool last_uniform = true;
static uint64_t audio_frames, audio_loud;
static uint16_t buttons; /* RetroPad bits held this frame */
static int verbose;
static const char *shot_path; /* EASYRPG_HEADLESS_SHOT: the last picture, as a PPM */

static void log_printf(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_WARN && !verbose)
        return;
    va_list va;
    va_start(va, fmt);
    fprintf(stderr, "core: ");
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixel_format = *(const enum retro_pixel_format *)data;
        return pixel_format == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_printf;
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        audio_cb = *(const struct retro_audio_callback *)data;
        audio_cb_set = audio_cb.callback != NULL;
        return true;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        frame_time_cb = *(const struct retro_frame_time_callback *)data;
        frame_time_set = frame_time_cb.callback != NULL;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* every option at its default: the first value its SET_VARIABLES offered ("Disabled") */
        struct retro_variable *var = data;
        var->value = "Disabled";
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        shutdown_asked = true;
        return true;
    default:
        return false; /* the VFS, MIDI out, core assets...: the core does without */
    }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data)
    {
        ++frames_null;
        return;
    }
    ++frames_drawn;
    last_w = width;
    last_h = height;
    /* FNV-1a over the picture, and how many colours it has (up to 256 counted) */
    uint64_t hash = 1469598103934665603ull;
    uint32_t seen[256];
    unsigned colours = 0;
    for (unsigned y = 0; y < height; ++y)
    {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)data + y * pitch);
        for (unsigned x = 0; x < width; ++x)
        {
            const uint32_t c = row[x] & 0xffffffu;
            hash = (hash ^ c) * 1099511628211ull;
            if (colours < 256)
            {
                unsigned k = 0;
                while (k < colours && seen[k] != c)
                    ++k;
                if (k == colours)
                    seen[colours++] = c;
            }
        }
    }
    if (shot_path)
    {
        FILE *f = fopen(shot_path, "wb");
        if (f)
        {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    const uint32_t c = ((const uint32_t *)((const uint8_t *)data + y * pitch))[x];
                    const unsigned char rgb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        }
    }
    if (hash != last_hash)
        ++distinct_pictures;
    last_hash = hash;
    last_colours = colours;
    last_uniform = colours <= 1;
}

static void audio_sample(int16_t left, int16_t right)
{
    (void)left, (void)right;
    ++audio_frames;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    for (size_t i = 0; i < frames * 2; ++i)
        if (data[i] > 256 || data[i] < -256)
        {
            ++audio_loud;
            break;
        }
    audio_frames += frames;
    return frames;
}

static void input_poll(void)
{
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0)
        return 0;
    if (device == RETRO_DEVICE_JOYPAD)
        return id < 16 && (buttons >> id) & 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <game folder or RPG_RT.ldb> <system folder> [frames]\n", argv[0]);
        return 2;
    }
    const char *game = argv[1];
    system_dir = argv[2];
    const int frames = argc > 3 ? atoi(argv[3]) : 900;
    verbose = getenv("EASYRPG_HEADLESS_VERBOSE") != NULL;
    shot_path = getenv("EASYRPG_HEADLESS_SHOT");

    struct retro_system_info info;
    retro_get_system_info(&info);
    printf("core: %s %s, extensions %s, needs the full path: %s\n", info.library_name, info.library_version,
           info.valid_extensions, info.need_fullpath ? "yes" : "no");

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_game_info game_info = {game, NULL, 0, NULL};
    if (!retro_load_game(&game_info))
    {
        printf("FAIL: the core did not load %s\n", game);
        return 1;
    }
    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    printf("loaded: %s\n", game);
    printf("av: %ux%u (max %ux%u), %.2f fps, %.0f Hz sound; audio callback %s, frame time callback %s\n",
           av.geometry.base_width, av.geometry.base_height, av.geometry.max_width, av.geometry.max_height,
           av.timing.fps, av.timing.sample_rate, audio_cb_set ? "yes" : "no", frame_time_set ? "yes" : "no");
    if (audio_cb_set && audio_cb.set_state)
        audio_cb.set_state(true);

    /* The logo, then the title screen; A (Decision) starts a new game, then the hero walks down and right. */
    uint64_t hash_at_title = 0;
    int ran = 0;
    for (int f = 0; f < frames && !shutdown_asked; ++f, ++ran)
    {
        buttons = 0;
        if ((f >= 600 && f < 606) || (f >= 700 && f < 706))
            buttons = 1u << RETRO_DEVICE_ID_JOYPAD_A;
        else if (f >= 760 && f < 800)
            buttons = 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
        else if (f >= 800 && f < 840)
            buttons = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
        if (f == 599)
            hash_at_title = last_hash;
        if (frame_time_set)
            frame_time_cb.callback(frame_time_cb.reference);
        retro_run();
        if (audio_cb_set)
            audio_cb.callback();
    }
    const bool moved_on = frames > 600 && last_hash != hash_at_title;
    printf("frames: %d run, %u with a picture, %u without; last picture %ux%u, %s, %u%s colours; %llu different "
           "pictures\n",
           ran, frames_drawn, frames_null, last_w, last_h,
           pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? "XRGB8888" : "another pixel format", last_colours,
           last_colours >= 256 ? "+" : "", (unsigned long long)distinct_pictures);
    printf("input: the picture %s after Decision on the title screen\n", moved_on ? "changed" : "did not change");
    printf("audio: %llu frames of sound (%.1f s), %llu batches with sound in them\n", (unsigned long long)audio_frames,
           av.timing.sample_rate > 0 ? audio_frames / av.timing.sample_rate : 0.0, (unsigned long long)audio_loud);
    retro_unload_game();

    /* the first game's verdict, before a second one changes the counts */
    bool ok = true;
    if (frames_drawn < (unsigned)ran * 9 / 10)
        ok = false, printf("FAIL: fewer pictures than frames\n");
    if (last_w == 0 || last_h == 0 || last_uniform)
        ok = false, printf("FAIL: the last picture is empty or one colour\n");
    if (distinct_pictures < 3)
        ok = false, printf("FAIL: the picture hardly changed\n");
    if (pixel_format != RETRO_PIXEL_FORMAT_XRGB8888)
        ok = false, printf("FAIL: the core did not ask for XRGB8888\n");
    if (shutdown_asked)
        ok = false, printf("FAIL: the core asked to shut down\n");

    bool second_ok = true;
    if (argc > 4)
    {
        /* the next game, in the same process: the core must start over cleanly */
        frames_drawn = frames_null = 0;
        distinct_pictures = 0;
        shutdown_asked = false;
        struct retro_game_info next = {argv[4], NULL, 0, NULL};
        second_ok = retro_load_game(&next);
        if (second_ok && audio_cb_set && audio_cb.set_state)
            audio_cb.set_state(true);
        for (int f = 0; second_ok && f < 300 && !shutdown_asked; ++f)
        {
            buttons = 0;
            if (frame_time_set)
                frame_time_cb.callback(frame_time_cb.reference);
            retro_run();
            if (audio_cb_set)
                audio_cb.callback();
        }
        second_ok = second_ok && frames_drawn >= 270 && distinct_pictures >= 2 && !last_uniform;
        printf("second game: %s, %u pictures, %llu different\n", second_ok ? "loaded and ran" : "FAILED", frames_drawn,
               (unsigned long long)distinct_pictures);
        if (second_ok)
            retro_unload_game();
    }
    retro_deinit();

    ok = ok && second_ok;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
