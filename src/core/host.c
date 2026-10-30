/*
 * Dulcamara - the libretro host (host.h), from SwanStationPS5's: the cores are linked in as static archives
 * and driven through their CoreInfo (cores.h). Only the environment calls the cores rely on are answered; the rest
 * get "unsupported", as libretro allows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "host.h"

#include "cores.h"

#include "../platform/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_toast(const char *message); /* main.c: a short notice on screen */

#define MAX_OPTIONS 128

typedef struct
{
    char key[64];
    char value[64];
} Option;

static const CoreInfo *core;
static bool loaded, shutdown_asked, fast_forward;
static char system_dir[NP_PATH_MAX];
static char assets_dir[NP_PATH_MAX];
static char game_path[NP_PATH_MAX];
static Option options[MAX_OPTIONS];
static int option_count;

static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_system_av_info av;
static struct retro_audio_callback audio_cb;
static struct retro_frame_time_callback frame_time_cb;
static bool audio_cb_set, audio_enabled;

static const void *frame_data;
static unsigned frame_w, frame_h;
static size_t frame_pitch;
static bool frame_fresh;

static uint16_t pad_bits; /* RetroPad buttons held, bit = RETRO_DEVICE_ID_JOYPAD_* */
static int16_t pad_lx, pad_ly;
static int volume = 10;

/* ---------------------------------------------------------------- options */

static Option *find_option(const char *key)
{
    for (int i = 0; i < option_count; ++i)
        if (!strcmp(options[i].key, key))
            return &options[i];
    return NULL;
}

/* A core option at its default: the first value offered (or the one named). */
static void default_option(const char *key, const char *value)
{
    if (!key || !value || find_option(key) || option_count == MAX_OPTIONS)
        return;
    Option *o = &options[option_count++];
    str_copy(o->key, sizeof(o->key), key);
    str_copy(o->value, sizeof(o->value), value);
}

/* Legacy variables look like "Description; default|other|..." */
static void register_variables(const struct retro_variable *vars)
{
    for (; vars && vars->key; ++vars)
    {
        const char *semi = vars->value ? strchr(vars->value, ';') : NULL;
        const char *start = semi ? semi + 1 : vars->value ? vars->value : "";
        while (*start == ' ')
            ++start;
        char value[64];
        size_t n = strcspn(start, "|");
        n = n < sizeof(value) - 1 ? n : sizeof(value) - 1;
        memcpy(value, start, n);
        value[n] = '\0';
        default_option(vars->key, value);
    }
}

static void register_definitions_v1(const struct retro_core_option_definition *d)
{
    for (; d && d->key; ++d)
        default_option(d->key, d->default_value ? d->default_value : d->values[0].value);
}

static void register_definitions_v2(const struct retro_core_options_v2 *v2)
{
    if (!v2)
        return;
    for (const struct retro_core_option_v2_definition *d = v2->definitions; d && d->key; ++d)
        default_option(d->key, d->default_value ? d->default_value : d->values[0].value);
}

/* ---------------------------------------------------------------- the core's callbacks */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_INFO)
        return; /* EasyRPG's debug lines: many a frame */
    char line[768];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    np_log("%s: %s", core ? core->id : "core", line);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_0RGB1555)
            return false;
        pixel_format = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        /* the app's own read-only assets: Ren'Py's engine is assets/renpy8 */
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
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        register_variables(data);
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        register_definitions_v1(data);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        register_definitions_v1(((const struct retro_core_options_intl *)data)->us);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        register_definitions_v2(data);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        register_definitions_v2(((const struct retro_core_options_v2_intl *)data)->us);
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* every option at its default; a core that asks for one it never declared gets NULL, as libretro says */
        struct retro_variable *var = data;
        const Option *o = var && var->key ? find_option(var->key) : NULL;
        var->value = o ? o->value : NULL;
        return o != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        const struct retro_game_geometry *g = data;
        av.geometry.base_width = g->base_width;
        av.geometry.base_height = g->base_height;
        av.geometry.aspect_ratio = g->aspect_ratio;
        np_log("host: the picture is now %ux%u", g->base_width, g->base_height);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        av = *(const struct retro_system_av_info *)data;
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        shutdown_asked = true;
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        if (data)
            *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = 3; /* video and audio */
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = fast_forward;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
        const struct retro_message *m = data;
        if (m && m->msg)
            app_toast(m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    {
        const struct retro_message_ext *m = data;
        if (m && m->msg && m->target != RETRO_MESSAGE_TARGET_LOG)
            app_toast(m->msg);
        else if (m && m->msg)
            np_log("%s: %s", core ? core->id : "core", m->msg);
        return true;
    }
    default:
        /* the VFS, MIDI out, a hardware context...: the cores do without */
        return false;
    }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data)
        return; /* the same picture again */
    frame_data = data;
    frame_w = width;
    frame_h = height;
    frame_pitch = pitch;
    frame_fresh = true;
}

static void audio_sample(int16_t left, int16_t right)
{
    int16_t frame[2] = {left, right};
    plat_audio_push(frame, 1);
}

static size_t audio_batch(const int16_t *samples, size_t frames)
{
    if (volume >= 10)
    {
        plat_audio_push(samples, frames);
        return frames;
    }
    int16_t scaled[2048];
    for (size_t done = 0; done < frames;)
    {
        size_t n = frames - done < 1024 ? frames - done : 1024;
        for (size_t i = 0; i < n * 2; ++i)
            scaled[i] = (int16_t)(samples[done * 2 + i] * volume / 10);
        plat_audio_push(scaled, n);
        done += n;
    }
    return frames;
}

static void input_poll(void)
{
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0)
        return 0;
    switch (device)
    {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return (int16_t)pad_bits;
        return id < 16 ? (pad_bits >> id) & 1 : 0;
    case RETRO_DEVICE_ANALOG:
        /* the left stick only: EasyRPG reads the right one as the number keys and the triggers as its own
         * fast-forward and debug keys, which Dulcamara keeps for itself */
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? pad_lx : id == RETRO_DEVICE_ID_ANALOG_Y ? pad_ly : 0;
        return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- the game */

bool host_load(const Game *g, const char *system, char *error, size_t error_size)
{
    if (loaded)
        host_unload();
    if (!g || g->core < 0 || g->core >= CORE_COUNT)
    {
        snprintf(error, error_size, "No core runs this game.");
        return false;
    }
    if (!CORES[g->core].init)
    {
        snprintf(error, error_size, "%s games play on the console.", CORES[g->core].name);
        return false;
    }
    core = &CORES[g->core];
    str_copy(system_dir, sizeof(system_dir), system);
    plat_asset_path(assets_dir, sizeof(assets_dir), "");
    str_copy(game_path, sizeof(game_path), g->path);
    option_count = 0;
    shutdown_asked = false;
    fast_forward = false;
    audio_cb_set = audio_enabled = false;
    memset(&frame_time_cb, 0, sizeof(frame_time_cb));
    pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    frame_data = NULL;
    frame_fresh = false;
    pad_bits = 0;
    pad_lx = pad_ly = 0;

    struct retro_system_info info;
    memset(&info, 0, sizeof(info));
    core->get_system_info(&info);
    np_log("host: %s %s (libretro API %u) loads %s", info.library_name ? info.library_name : core->name,
           info.library_version ? info.library_version : "", core->api_version(), g->path);

    core->set_environment(environment);
    core->set_video_refresh(video_refresh);
    core->set_audio_sample(audio_sample);
    core->set_audio_sample_batch(audio_batch);
    core->set_input_poll(input_poll);
    core->set_input_state(input_state);
    core->init();

    struct retro_game_info game = {game_path, NULL, 0, NULL};
    if (!core->load_game(&game))
    {
        core->deinit();
        snprintf(error, error_size, "%s could not start this game.", core->name);
        np_log("host: %s refused %s", core->name, game_path);
        core = NULL;
        return false;
    }
    memset(&av, 0, sizeof(av));
    core->get_system_av_info(&av);
    if (av.timing.fps <= 0)
        av.timing.fps = 60.0;
    if (av.timing.sample_rate <= 0)
        av.timing.sample_rate = 48000.0;
    core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    np_log("host: %ux%u at %.2f fps, sound %.0f Hz%s", av.geometry.base_width, av.geometry.base_height,
           av.timing.fps, av.timing.sample_rate, audio_cb_set ? " (asked for by the frontend)" : "");
    plat_audio_open((int)av.timing.sample_rate);
    plat_audio_clear();
    if (audio_cb_set && audio_cb.set_state)
        audio_cb.set_state(true);
    audio_enabled = true;
    loaded = true;
    return true;
}

void host_unload(void)
{
    if (!loaded)
        return;
    if (audio_cb_set && audio_cb.set_state)
        audio_cb.set_state(false);
    core->unload_game();
    core->deinit();
    np_log("host: %s stopped", core->name);
    plat_audio_clear();
    loaded = false;
    audio_cb_set = audio_enabled = false;
    frame_data = NULL;
    core = NULL;
}

bool host_loaded(void)
{
    return loaded;
}

const char *host_core_name(void)
{
    return core ? core->name : "";
}

void host_set_input(const PadState *pad, const Settings *s)
{
    uint16_t bits = 0;
    if (pad)
    {
        for (int b = 0; b < MAP_BUTTONS; ++b)
            if ((pad->buttons & BIT(b)) && s->button_map[b] >= 0 && s->button_map[b] < 16)
                bits |= (uint16_t)(1u << s->button_map[b]);
        if (s->stick_dpad)
        {
            const int16_t dead = 16000;
            if (pad->lx < -dead)
                bits |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
            if (pad->lx > dead)
                bits |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
            if (pad->ly < -dead)
                bits |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
            if (pad->ly > dead)
                bits |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
        }
        pad_lx = s->stick_dpad ? pad->lx : 0;
        pad_ly = s->stick_dpad ? pad->ly : 0;
    }
    pad_bits = bits;
}

void host_run(int times, double seconds)
{
    if (!loaded)
        return;
    retro_usec_t usec = (retro_usec_t)(seconds * 1e6);
    if (usec <= 0)
        usec = frame_time_cb.reference > 0 ? frame_time_cb.reference : 16667;
    for (int i = 0; i < times && !shutdown_asked; ++i)
    {
        if (frame_time_cb.callback)
            frame_time_cb.callback(usec);
        core->run();
    }
}

void host_feed_audio(double seconds)
{
    if (!loaded || !audio_cb_set || !audio_enabled)
        return;
    const size_t want = (size_t)(av.timing.sample_rate * seconds);
    /* a few calls at most a refresh: each gives a frame's worth (EasyRPG: 800 at 48 kHz) */
    for (int i = 0; i < 6 && plat_audio_queued_frames() < want; ++i)
        audio_cb.callback();
}

void host_set_volume(int tenths)
{
    volume = tenths < 0 ? 0 : tenths > 10 ? 10 : tenths;
}

void host_set_fast_forward(bool on)
{
    fast_forward = on;
}

bool host_shutdown_requested(void)
{
    return shutdown_asked;
}

void host_reset(void)
{
    if (loaded)
        core->reset();
}

double host_fps(void)
{
    return av.timing.fps > 0 ? av.timing.fps : 60.0;
}

int host_sample_rate(void)
{
    return av.timing.sample_rate > 0 ? (int)av.timing.sample_rate : 48000;
}

float host_aspect(void)
{
    if (av.geometry.aspect_ratio > 0.0f)
        return av.geometry.aspect_ratio;
    if (frame_w && frame_h)
        return (float)frame_w / (float)frame_h;
    if (av.geometry.base_width && av.geometry.base_height)
        return (float)av.geometry.base_width / (float)av.geometry.base_height;
    return 4.0f / 3.0f;
}

const void *host_frame(int *width, int *height, size_t *pitch, int *format, bool *fresh)
{
    if (!frame_data)
        return NULL;
    *width = (int)frame_w;
    *height = (int)frame_h;
    *pitch = frame_pitch;
    *format = (int)pixel_format;
    if (fresh)
        *fresh = frame_fresh;
    frame_fresh = false;
    return frame_data;
}

bool host_can_save_states(void)
{
    return loaded && core->serialize_size() > 0;
}
