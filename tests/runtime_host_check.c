/* Real host state machine with a controlled core/platform boundary. */
#include "core/host.h"
#include "core/cores.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static retro_environment_t env;
static retro_input_state_t input;
static retro_video_refresh_t video;
static uint64_t now;
static int unloaded, deinitialized, ran, audio_clears;
static bool stopped, refuse, extension = true;
static struct dr_save_layout layout;
void np_log(const char *fmt, ...)
{
    (void)fmt;
}
void app_toast(const char *s)
{
    (void)s;
}
void str_copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}
void plat_asset_path(char *dst, size_t n, const char *s)
{
    (void)s;
    str_copy(dst, n, "/assets");
}
void plat_pad_touch(bool on)
{
    (void)on;
}
uint64_t plat_ticks_us(void)
{
    return now;
}
bool plat_audio_open(int rate)
{
    (void)rate;
    return true;
}
void plat_audio_clear(void)
{
    ++audio_clears;
}
void plat_audio_push(const int16_t *s, size_t n)
{
    (void)s;
    (void)n;
}
size_t plat_audio_queued_frames(void)
{
    return 0;
}
static void copy(struct dr_save_layout *out)
{
    *out = layout;
}
static bool release(uint32_t epoch)
{
    if (!layout.checkpoint || layout.generation != epoch)
        return false;
    layout.checkpoint = false;
    return true;
}
static void stop(void)
{
    stopped = true;
}
static enum dr_stop_result poll(void)
{
    if (!stopped)
        return DR_STOP_RUNNING;
    if (refuse)
        return DR_STOP_PENDING;
    layout.writers_finished = true;
    return DR_STOP_CLEAN;
}
static void set_env(retro_environment_t cb)
{
    env = cb;
}
static void set_input(retro_input_state_t cb)
{
    input = cb;
}
static void set_video(retro_video_refresh_t cb)
{
    video = cb;
}
static void set_audio(retro_audio_sample_t cb)
{
    (void)cb;
}
static void set_batch(retro_audio_sample_batch_t cb)
{
    (void)cb;
}
static void set_poll(retro_input_poll_t cb)
{
    (void)cb;
}
static void noop(void)
{
}
static void deinit(void)
{
    ++deinitialized;
}
static void unload(void)
{
    ++unloaded;
}
static unsigned version(void)
{
    return 1;
}
static void info(struct retro_system_info *out)
{
    memset(out, 0, sizeof(*out));
}
static void av(struct retro_system_av_info *out)
{
    memset(out, 0, sizeof(*out));
}
static void controller(unsigned a, unsigned b)
{
    (void)a;
    (void)b;
}
static size_t serialize(void)
{
    return 0;
}
static bool load(const struct retro_game_info *game)
{
    assert(!strcmp(game->path, "/native/game"));
    stopped = false;
    memset(&layout, 0, sizeof(layout));
    layout.version = DR_RUNTIME_VERSION;
    layout.size = sizeof(layout);
    layout.count = layout.generation = 1;
    layout.available = layout.complete = true;
    layout.preinitial = false;
    layout.initial = layout.checkpoint = true;
    strcpy(layout.entries[0].path, "/native/game/saves");
    layout.entries[0].kind = DR_SAVE_DIRECTORY;
    layout.entries[0].scope = DR_SAVE_GAME;
    layout.entries[0].policy = DR_SAVE_READ_WRITE;
    struct dr_runtime_interface api = {
        DR_RUNTIME_VERSION, sizeof(api), false, copy, release, stop, poll, NULL};
    if (extension)
        assert(env(DR_ENVIRONMENT_RUNTIME, &api));
    return true;
}
static void run(void)
{
    ++ran;
    static uint32_t pixel;
    video(&pixel, 1, 1, 4);
    now += 10;
}
const CoreInfo CORES[CORE_COUNT] = {{.id = "test",
                                     .name = "controlled core",
                                     .set_environment = set_env,
                                     .set_video_refresh = set_video,
                                     .set_audio_sample = set_audio,
                                     .set_audio_sample_batch = set_batch,
                                     .set_input_poll = set_poll,
                                     .set_input_state = set_input,
                                     .init = noop,
                                     .deinit = deinit,
                                     .api_version = version,
                                     .get_system_info = info,
                                     .get_system_av_info = av,
                                     .set_controller_port_device = controller,
                                     .reset = noop,
                                     .run = run,
                                     .serialize_size = serialize,
                                     .load_game = load,
                                     .unload_game = unload}};
int main(void)
{
    Game game = {0};
    strcpy(game.path, "/native/game");
    char error[256];
    Settings settings = {0};
    settings.button_map[0][0] = RETRO_DEVICE_ID_JOYPAD_B;
    PadState pad = {.connected = true, .buttons = 1, .lx = 20000};
    for (int i = 0; i < 10; i++)
    {
        assert(host_load(&game, "/system", error, sizeof(error)));
        assert(host_state() == HOST_LOADING && host_owns_runtime() && host_snapshot_eligible());
        assert(!host_load(&game, "/system", error, sizeof(error)));
        assert(!host_release_checkpoint(2) && host_release_checkpoint(1));
        assert(host_state() == HOST_RUNNING && !host_snapshot_eligible());
        host_set_input(&pad, &settings);
        assert(input(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B));
        host_reset_transient();
        assert(!input(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B));
        PadState disconnected = {0};
        host_set_input(&disconnected, &settings);
        int clears = audio_clears;
        host_set_input(&disconnected, &settings);
        assert(audio_clears == clears); /* disconnected idle must not repeatedly silence audio */
        host_run(2, .016);
        HostCounters counters;
        host_counters(&counters);
        assert(counters.engine_calls == 2 && counters.engine_us == 20 &&
               counters.fresh_pictures == 2 && counters.frame_bytes == 4);
        host_request_stop();
        assert(host_state() == HOST_STOPPING && host_owns_runtime());
        host_run(2, .016);
        assert(ran == (i + 1) * 2);
        assert(host_poll() == HOST_IDLE && !host_owns_runtime() && host_snapshot_eligible());
    }
    assert(host_load(&game, "/system", error, sizeof(error)));
    struct dr_save_entry valid = layout.entries[0];
    for (unsigned field = 0; field < 3; ++field)
    {
        layout.entries[0] = valid;
        if (field == 0) layout.entries[0].kind = 999;
        if (field == 1) layout.entries[0].scope = 999;
        if (field == 2) layout.entries[0].policy = 999;
        assert(!host_snapshot_eligible());
        struct dr_save_layout invalid;
        assert(host_copy_save_layout(&invalid) && !invalid.complete && !invalid.available);
    }
    layout.entries[0] = valid;
    layout.entries[0].kind = DR_SAVE_RENPY_FILE;
    assert(host_snapshot_eligible());
    strcpy(layout.entries[0].suffix, ".unexpected");
    assert(!host_snapshot_eligible());
    layout.entries[0] = valid;
    layout.entries[0].policy = DR_SAVE_UNKNOWN;
    assert(host_snapshot_eligible()); /* recognized, but not a claim of writable access */
    layout.entries[0].kind = DR_SAVE_RENPY_SLOTS;
    strcpy(layout.entries[0].suffix,"../bad");assert(!host_snapshot_eligible());
    memset(layout.entries[0].suffix,'x',sizeof(layout.entries[0].suffix));assert(!host_snapshot_eligible());
    layout.entries[0]=valid;
    strcpy(layout.entries[0].suffix,".unexpected");assert(!host_snapshot_eligible());
    layout.entries[0]=valid;
    layout.count = DR_SAVE_MAX + 1;
    assert(!host_snapshot_eligible());
    layout.count = 1;
    host_request_stop();
    assert(host_poll() == HOST_IDLE);
    /* An individual early discovery must not end loading before the complete initial union. */
    assert(host_load(&game, "/system", error, sizeof(error)));
    layout.initial = false;
    layout.preinitial = true;
    assert(host_snapshot_eligible());
    assert(host_snapshot_eligible() && host_release_checkpoint(1));
    assert(host_state() == HOST_LOADING);
    layout.preinitial = false;
    layout.initial = layout.checkpoint = true;
    layout.generation = 2;
    assert(host_snapshot_eligible() && host_release_checkpoint(2));
    assert(host_state() == HOST_RUNNING);
    host_request_stop();
    assert(host_poll() == HOST_IDLE);
    extension = false;
    assert(host_load(&game, "/system", error, sizeof(error)));
    assert(!host_snapshot_eligible());
    host_request_stop();
    assert(!host_snapshot_eligible());
    extension = true;
    refuse = true;
    assert(host_load(&game, "/system", error, sizeof(error)));
    host_request_stop();
    int before = unloaded, before_deinit = deinitialized;
    assert(host_poll() == HOST_STOPPING);
    now += 10000001;
    assert(host_poll() == HOST_FAILURE && host_owns_runtime() && !host_snapshot_eligible());
    assert(unloaded == before && deinitialized == before_deinit);
    refuse = false;
    assert(host_poll() == HOST_FAILURE);
    assert(!host_snapshot_eligible() && !host_load(&game, "/system", error, sizeof(error)));
    struct dr_save_layout out;
    assert(host_copy_save_layout(&out) && !out.writers_finished && !out.checkpoint);
    puts("PASS lifecycle, held launch, clean stop, repeated transitions, transient reset, "
         "counters, unsupported core, sticky unsafe timeout");
}
