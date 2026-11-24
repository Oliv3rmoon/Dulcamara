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
/* the sound: a fake queue the console's output drains (play), a core whose callback gives 800 frames (none while
 * `sound_busy`, as Ren'Py's core while its thread holds the mixer's lock), and the log's lines about it */
static bool with_sound, sound_busy;
static size_t queued;
static int sound_calls, ran_out_lines;
static retro_audio_sample_batch_t batch;
static char last_ran_out[512];
void np_log(const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (strstr(line, "sound ran out"))
    {
        ++ran_out_lines;
        snprintf(last_ran_out, sizeof(last_ran_out), "%s", line);
    }
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
    queued = 0;
}
void plat_audio_push(const int16_t *s, size_t n)
{
    (void)s;
    queued += n;
}
size_t plat_audio_queued_frames(void)
{
    return queued;
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
    batch = cb;
}
static void sound_callback(void)
{
    ++sound_calls;
    static int16_t frames[800 * 2];
    if (!sound_busy)
        batch(frames, 800);
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
    if (with_sound)
    {
        struct retro_audio_callback cb = {sound_callback, NULL};
        assert(env(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &cb));
    }
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
/* the console's output plays `us` of the queue (48 kHz), and that much time passes */
static void play(uint64_t us)
{
    const size_t played = (size_t)(us * 48 / 1000);
    queued = queued > played ? queued - played : 0;
    now += us;
}

static uint64_t underruns(void)
{
    HostCounters c;
    host_counters(&c);
    return c.audio_underruns;
}

/* a main-loop pass at 60 Hz: a refresh's sound played, then the top-up */
static int pass(double seconds, bool grows)
{
    play(16667);
    const int calls = sound_calls;
    host_feed_audio(seconds, grows);
    return sound_calls - calls;
}

static void start_game(Game *game)
{
    char error[256];
    assert(host_load(game, "/system", error, sizeof(error)));
    assert(host_release_checkpoint(1) && host_state() == HOST_RUNNING);
    queued = 0;
    sound_calls = ran_out_lines = 0;
}

/* host_feed_audio, deterministic: what is kept queued, how it grows after an underrun and is refilled, the log */
static void check_audio_cushion(Game *game)
{
    with_sound = true;
    start_game(game);
    now = 1000000;
    /* a Ren'Py game's sound kept 0.15 s ahead (7200 frames), filled in one pass */
    assert(pass(0.15, true) == 9 && queued == 7200);
    for (int i = 0; i < 30; ++i)
        assert(pass(0.15, true) == 1 && queued >= 7200 && queued < 8000);
    assert(underruns() == 0 && ran_out_lines == 0);
    /* the core gives nothing for 0.2 s (its mixer's lock held): the queue runs out once, logged, and what is kept
     * ahead doubles */
    sound_busy = true;
    for (int i = 0; i < 12; ++i)
        pass(0.15, true);
    assert(queued == 0 && underruns() == 1 && ran_out_lines == 1);
    assert(strstr(last_ran_out, "underrun 1 this game") && strstr(last_ran_out, "nothing queued, 16.7 ms after the last "
                                                                                 "top-up, which left 16.7 ms queued") &&
           strstr(last_ran_out, "now kept 300 ms ahead (was 150)"));
    /* refilled in one pass once the core gives sound again: 0.3 s (14400 frames), 18 calls */
    sound_busy = false;
    assert(pass(0.15, true) == 18 && queued >= 14400 && queued < 15200);
    /* another underrun within the second: counted and grown to the most, 0.4 s, not logged */
    sound_busy = true;
    for (int i = 0; i < 20; ++i)
        pass(0.15, true);
    assert(underruns() == 2 && ran_out_lines == 1);
    sound_busy = false;
    assert(pass(0.15, true) == 24 && queued == 19200);
    /* the next, more than a second after the last line: logged; 0.4 s stays the most */
    for (int i = 0; i < 30; ++i)
        assert(pass(0.15, true) == 1);
    sound_busy = true;
    for (int i = 0; i < 30; ++i)
        pass(0.15, true);
    assert(underruns() == 3 && ran_out_lines == 2 && strstr(last_ran_out, "now kept 400 ms ahead (was 400)"));
    sound_busy = false;
    pass(0.15, true);
    /* a pause (Dulcamara's menu: no top-ups for 2 s) runs the queue down: not an underrun */
    play(2000000);
    assert(queued == 0);
    pass(0.15, true);
    assert(underruns() == 3 && queued == 19200);
    /* the first 20 of a game are logged, a second apart at least */
    for (int k = 0; k < 25; ++k)
    {
        sound_busy = true;
        for (int i = 0; i < 70; ++i) /* 1.17 s */
            pass(0.15, true);
        sound_busy = false;
        pass(0.15, true);
    }
    assert(underruns() == 28 && ran_out_lines == 20);
    /* a new game: 0.15 s again, its underruns logged again */
    host_request_stop();
    assert(host_poll() == HOST_IDLE);
    start_game(game);
    assert(pass(0.15, true) == 9 && queued == 7200);
    sound_busy = true;
    for (int i = 0; i < 12; ++i)
        pass(0.15, true);
    sound_busy = false;
    assert(underruns() == 1 && ran_out_lines == 1 && strstr(last_ran_out, "now kept 300 ms ahead (was 150)"));
    host_request_stop();
    assert(host_poll() == HOST_IDLE);
    /* a game whose amount does not grow (EasyRPG's): 0.07 s, 6 calls a pass at most, as before; underruns logged */
    start_game(game);
    assert(pass(0.07, false) == 5 && queued == 4000);
    sound_busy = true;
    for (int i = 0; i < 8; ++i)
        pass(0.07, false);
    sound_busy = false;
    assert(underruns() == 1 && ran_out_lines == 1 && strstr(last_ran_out, "now kept 70 ms ahead (was 70)"));
    assert(pass(0.07, false) == 5 && queued == 4000);
    host_request_stop();
    assert(host_poll() == HOST_IDLE);
    with_sound = false;
}

int main(void)
{
    Game game = {0};
    strcpy(game.path, "/native/game");
    char error[256];
    Settings settings = {0};
    settings.button_map[0][0] = RETRO_DEVICE_ID_JOYPAD_B;
    PadState pad = {.connected = true, .buttons = 1, .lx = 20000};
    check_audio_cushion(&game);
    now = 0;
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
         "counters, unsupported core, sticky unsafe timeout, the game's sound kept queued");
}
