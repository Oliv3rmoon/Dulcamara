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
 *   sounds the game's sounds (renpy_sounds=<base>, from the environment): the core lists them in <base>.list and
 *          plays nothing for those <base>.txt has off; as "start", run until the list is written and two seconds on,
 *          with or without sound. With RENPY_HEADLESS_SWITCH_OFF=<name>, that sound is then switched off as the game
 *          runs, <base>.txt written as Dulcamara's menu writes it, and the game runs at the console's pace (the core
 *          looks at the file once a second of this computer's time); once the core plays nothing for it, the sound is
 *          counted again from a second and a half on.
 *   error  the game has a script error: Ren'Py's error is logged and shown; it is then closed.
 *   soak   a long session (tests/test_renpy_soak.py's game, DULCAMARA_TEST_SOAK=<scenes>): the game changes scene again
 *          and again and says, for each, which colours are to be where ("DULCAMARA-TEST: soak <i> expect ..."); the
 *          frontend looks at the picture RENPY_HEADLESS_SOAK_DELAY frames on (4 by default) and counts each scene whose
 *          backgrounds, characters and text are all there, late, or missing. Each "soak-stats" line of the game's is
 *          printed with this process's threads, open files and resident memory, as a row of a table. Each frame is
 *          RENPY_HEADLESS_FRAME_USEC of the game's time (the console's 1/60 s by default): longer, and hours of a
 *          game pass in minutes.
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
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <dirent.h>
#endif
#define DR_TEST_SAVES 1
#include "runtime_probe.h"

#define MAX_FRAMES 7200 /* two minutes of the game's time for each step */

static const char *assets_dir, *data_dir;
static struct retro_audio_callback audio_cb;
static struct retro_frame_time_callback frame_time_cb;
static bool audio_cb_set, frame_time_set, shutdown_asked, xrgb, fast_forward;
static unsigned picture_w, picture_h, geometry_w, geometry_h;
static uint64_t picture_hash, with_picture, dupes, distinct, sound_frames, sound_loud, refreshes;
static int sound_peak; /* the loudest sample, against the game's own (its volume as the core plays it) */
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

/* soak: the scene the game said it shows (its expectations), and what was found */
static bool soaking;
static char soak_expect[2048]; /* the newest expectation line's checks, waiting to be looked at */
static int soak_scene = -1, soak_wait = -1, soak_delay = 4, soak_late_frames;
static long soak_seen, soak_ok, soak_late, soak_missing, soak_errors, soak_done, soak_rollbacks, soak_skipped;
static int soak_lenient; /* the scenes after a rollback, which Ren'Py may roll forward through without showing */
static bool soak_traceback; /* Ren'Py's error screen, or a thread of its ended by an exception: the soak ends */
static void soak_line(const char *line);

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    char line[4096];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    const size_t n = strlen(line);
    if (soaking)
    {
        soak_line(line);
        /* the log of hours does not fit: what is looked for in it is only the newest */
        if (log_used + n + 1 >= sizeof(log_text))
            log_used = 0;
    }
    if (log_used + n + 1 < sizeof(log_text))
    {
        memcpy(log_text + log_used, line, n + 1);
        log_used += n;
    }
    if (soaking && strstr(line, "DULCAMARA-TEST: soak"))
        return; /* soak_line's */
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
    case DR_ENVIRONMENT_RUNTIME:
        return probe_environment(data);
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
        /* the core's options, as Dulcamara sets them: RENPY_HEADLESS_SKIP_UNSEEN=on, and a game's keys and its sounds'
         * files by their own names (renpy_key_l2=return, renpy_sounds=<base>) */
        struct retro_variable *variable = data;
        if (!variable || !variable->key)
            return false;
        if (!strncmp(variable->key, "renpy_key_", 10) || !strcmp(variable->key, "renpy_sounds"))
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
static uint32_t *kept;     /* with RENPY_HEADLESS_PICTURE=<file>: the last picture, its rows together, for report() */
static size_t kept_pixels; /* to write there as raw XRGB8888, top row first */
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
    /* with RENPY_HEADLESS_FIRST_PICTURE=<file>: the first picture given (a game's presplash, while it loads), written
     * there at once as raw XRGB8888, top row first */
    const char *first = getenv("RENPY_HEADLESS_FIRST_PICTURE");
    if (first && with_picture == 1)
    {
        FILE *f = fopen(first, "wb");
        for (unsigned y = 0; f && y < height; ++y)
            fwrite((const uint8_t *)data + y * pitch, sizeof(uint32_t), width, f);
        if (f)
            fclose(f);
        printf("first picture: %ux%u\n", width, height);
    }
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
    if (getenv("RENPY_HEADLESS_PICTURE") || soaking)
    {
        const size_t n = (size_t)width * height;
        if (kept_pixels < n)
        {
            uint32_t *grown = realloc(kept, n * sizeof(uint32_t));
            if (!grown)
                return;
            kept = grown;
            kept_pixels = n;
        }
        for (unsigned y = 0; y < height; ++y)
            memcpy(kept + (size_t)y * width, (const uint8_t *)data + y * pitch, width * sizeof(uint32_t));
    }
}

static void audio_sample(int16_t left, int16_t right)
{
    (void)left, (void)right;
    ++sound_frames;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    bool loud = false;
    for (size_t i = 0; i < frames * 2; ++i)
    {
        const int level = data[i] < 0 ? -data[i] : data[i];
        sound_peak = level > sound_peak ? level : sound_peak;
        loud = loud || level > 256;
    }
    sound_loud += loud;
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

static retro_usec_t frame_usec; /* RENPY_HEADLESS_FRAME_USEC, or the core's own (0) */

static void frame(void)
{
    probe_tick();
    if (frame_time_set)
        frame_time_cb.callback(frame_usec ? frame_usec : frame_time_cb.reference);
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

/* until_logged at the console's pace, a frame each 1/60 s of this computer's time, for `seconds` at most: what the core
 * does by that clock (once a second) would otherwise come thousands of frames on, the game running here many times
 * faster than its own time */
static bool until_logged_in_time(const char *what, int seconds)
{
    const struct timespec frame_time = {0, 16666667};
    for (int i = 0; i < seconds * 60 && !logged(what) && !shutdown_asked; ++i)
    {
        frame();
        nanosleep(&frame_time, NULL);
    }
    if (!logged(what))
        printf("FAIL: never logged \"%s\" in %d seconds\n", what, seconds);
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
    sound_peak = 0;
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
    printf("picture: %ux%u, %s, hash %016llx\n", picture_w, picture_h, xrgb ? "XRGB8888" : "another pixel format",
           (unsigned long long)picture_hash);
    printf("sound: %s (%llu frames, %llu batches with sound in them, the loudest sample %d)\n",
           sound_loud > 10 ? "playing" : "silent", (unsigned long long)sound_frames, (unsigned long long)sound_loud,
           sound_peak);
    const char *keep = getenv("RENPY_HEADLESS_PICTURE");
    FILE *f = keep && kept ? fopen(keep, "wb") : NULL;
    if (f)
    {
        fwrite(kept, sizeof(uint32_t), (size_t)picture_w * picture_h, f);
        fclose(f);
    }
}

/* ---------------------------------------------------------------- soak */

/* this process's threads, open files and resident memory (KB); -1 where this system does not say */
static void process_counts(long *threads, long *files, long *resident_kb)
{
    *threads = *files = *resident_kb = -1;
#if defined(__APPLE__)
    thread_act_array_t list;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) == KERN_SUCCESS)
    {
        for (mach_msg_type_number_t i = 0; i < count; ++i)
            mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)list, sizeof(*list) * count);
        *threads = (long)count;
    }
    struct mach_task_basic_info info;
    mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &size) == KERN_SUCCESS)
        *resident_kb = (long)(info.resident_size >> 10);
#else
    DIR *tasks = opendir("/proc/self/task");
    if (tasks)
    {
        long n = 0;
        for (struct dirent *e; (e = readdir(tasks));)
            n += e->d_name[0] != '.';
        closedir(tasks);
        *threads = n;
    }
    FILE *statm = fopen("/proc/self/statm", "r");
    long pages = 0, resident = 0;
    if (statm && fscanf(statm, "%ld %ld", &pages, &resident) == 2)
        *resident_kb = resident * (sysconf(_SC_PAGESIZE) / 1024);
    if (statm)
        fclose(statm);
#endif
    long open = 0;
    const long limit = sysconf(_SC_OPEN_MAX) > 0 && sysconf(_SC_OPEN_MAX) < 65536 ? sysconf(_SC_OPEN_MAX) : 65536;
    for (long fd = 0; fd < limit; ++fd)
        open += fcntl((int)fd, F_GETFD) != -1;
    *files = open;
}

static bool near(uint32_t pixel, unsigned rgb)
{
    for (int shift = 0; shift < 24; shift += 8)
    {
        const int a = (int)((pixel >> shift) & 255), b = (int)((rgb >> shift) & 255);
        if (a - b > 12 || b - a > 12)
            return false;
    }
    return true;
}

/* Each check of an expectation: "x,y=RRGGBB" (that pixel that colour) or "x0,y0,x1,y1#n=RRGGBB" (n pixels of that
 * colour at least in the box); the first that fails is put in `failed` */
static bool soak_check(const char *checks, char *failed, size_t size)
{
    if (!kept || !picture_w)
    {
        snprintf(failed, size, "no picture");
        return false;
    }
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", checks);
    for (char *save = NULL, *check = strtok_r(copy, " ", &save); check; check = strtok_r(NULL, " ", &save))
    {
        int x0, y0, x1, y1, least;
        unsigned rgb;
        bool ok;
        if (sscanf(check, "%d,%d,%d,%d#%d=%x", &x0, &y0, &x1, &y1, &least, &rgb) == 6)
        {
            int found = 0;
            for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < (int)picture_h; ++y)
                for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < (int)picture_w; ++x)
                    found += near(kept[(size_t)y * picture_w + (size_t)x], rgb);
            ok = found >= least;
            if (!ok)
                snprintf(failed, size, "%s (%d found)", check, found);
        }
        else if (sscanf(check, "%d,%d=%x", &x0, &y0, &rgb) == 3)
        {
            const bool inside = x0 >= 0 && y0 >= 0 && x0 < (int)picture_w && y0 < (int)picture_h;
            const uint32_t pixel = inside ? kept[(size_t)y0 * picture_w + (size_t)x0] & 0xFFFFFFu : 0;
            ok = inside && near(pixel, rgb);
            if (!ok)
                snprintf(failed, size, "%s (%06x there)", check, (unsigned)pixel);
        }
        else
        {
            snprintf(failed, size, "%s (not a check)", check);
            ok = false;
        }
        if (!ok)
            return false;
    }
    return true;
}

static void soak_report_missing(int scene, const char *failed, bool lenient)
{
    if (lenient)
    {
        ++soak_skipped;
        return;
    }
    if (++soak_missing <= 20)
        printf("soak: scene %d not as expected: %s\n", scene, failed);
}

/* The game's soak lines, as it logs them: an expectation for the picture to come, a row of the table, the end. */
static void soak_line(const char *line)
{
    const char *at = strstr(line, "DULCAMARA-TEST: soak");
    if (!at)
    {
        if (!strstr(line, "OpenGL work") && (strstr(line, "rror") || strstr(line, "Traceback") || strstr(line, "refused") || strstr(line, "could not") ||
             strstr(line, "no memory")) &&
            ++soak_errors <= 30)
            printf("soak: logged: %s", line);
        soak_traceback = soak_traceback || strstr(line, "Traceback");
        return;
    }
    /* each of the game's lines comes twice, its print and Ren'Py's log of it: the print is enough */
    if (strstr(line, "renpy log: DULCAMARA-TEST"))
        return;
    at += strlen("DULCAMARA-TEST: soak");
    int scene, used = 0;
    if (sscanf(at, " %d expect %n", &scene, &used) == 1 && used)
    {
        if (soak_wait >= 0 && soak_scene >= 0) /* the one before was never seen as expected */
        {
            char failed[256];
            if (!soak_check(soak_expect, failed, sizeof(failed)))
                soak_report_missing(soak_scene, failed, soak_lenient > 0);
            else
                ++soak_ok;
        }
        if (soak_lenient > 0)
            --soak_lenient;
        ++soak_seen;
        soak_scene = scene;
        soak_wait = soak_delay;
        soak_late_frames = 0;
        snprintf(soak_expect, sizeof(soak_expect), "%s", at + used);
        soak_expect[strcspn(soak_expect, "\r\n")] = '\0';
    }
    else if (!strncmp(at, "-stats ", 7))
    {
        long threads, files, resident;
        process_counts(&threads, &files, &resident);
        char stats[1024];
        snprintf(stats, sizeof(stats), "%s", at + 7);
        stats[strcspn(stats, "\r\n")] = '\0';
        printf("soak-row: %s threads=%ld files=%ld rss_kb=%ld seen=%ld ok=%ld late=%ld missing=%ld\n", stats, threads,
               files, resident, soak_seen, soak_ok, soak_late, soak_missing);
    }
    else if (!strncmp(at, "-rollback", 9))
    {
        ++soak_rollbacks;
        soak_lenient = 3; /* this scene, rolled back past, and the one or two after it */
    }
    else if (!strncmp(at, " done", 5))
        soak_done = 1;
}

/* after each frame: the expectation waiting looked at, once its picture has come */
static void soak_after_frame(void)
{
    if (soak_wait < 0)
        return;
    if (soak_wait > 0)
    {
        --soak_wait;
        return;
    }
    char failed[256];
    if (soak_check(soak_expect, failed, sizeof(failed)))
    {
        if (soak_late_frames)
        {
            if (++soak_late <= 20)
                printf("soak: scene %d came %d frames late\n", soak_scene, soak_late_frames);
        }
        else
            ++soak_ok;
        soak_wait = -1;
        return;
    }
    ++soak_late_frames; /* looked at again each frame until the next scene: late, or missing */
}

static bool soak(void)
{
    const char *delay = getenv("RENPY_HEADLESS_SOAK_DELAY");
    soak_delay = delay && atoi(delay) > 0 ? atoi(delay) : 4;
    soaking = true;
    const long most = getenv("RENPY_HEADLESS_SOAK_FRAMES") ? atol(getenv("RENPY_HEADLESS_SOAK_FRAMES")) : 4000000;
    const time_t start = time(NULL);
    long n = 0;
    for (; n < most && !soak_done && !shutdown_asked && !soak_traceback; ++n)
    {
        frame();
        soak_after_frame();
    }
    if (soak_wait >= 0 && soak_scene >= 0)
    {
        char failed[256];
        if (soak_check(soak_expect, failed, sizeof(failed)))
            ++soak_ok;
        else
            soak_report_missing(soak_scene, failed, false);
    }
    printf("soak: %ld scenes, %ld as expected, %ld late, %ld missing, %ld rolled forward through, %ld errors logged, %ld "
           "rollbacks; %ld frames (%.1f hours of the game's time) in %ld s\n",
           soak_seen, soak_ok, soak_late, soak_missing, soak_skipped, soak_errors, soak_rollbacks, n,
           (double)n * (double)(frame_usec ? frame_usec : 16667) / 3.6e9, (long)(time(NULL) - start));
    soaking = false;
    if (soak_traceback)
        printf("FAIL: the soak ended on a Python traceback\n");
    return soak_done && soak_seen > 0 && !soak_missing && !soak_late && !soak_traceback;
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
    if (getenv("RENPY_HEADLESS_FRAME_USEC"))
        frame_usec = (retro_usec_t)atoll(getenv("RENPY_HEADLESS_FRAME_USEC"));
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
    if (ok && !strcmp(mode, "held-stop"))
    {
        assert(probe_runtime.version);
        struct dr_save_layout layout;
        for (int i = 0; i < MAX_FRAMES; ++i)
        {
            retro_run();
            probe_runtime.copy_layout(&layout);
            if (layout.checkpoint)
                break;
        }
        assert(layout.checkpoint && (layout.initial || layout.preinitial));
        probe_stop(false);
        retro_unload_game();
        retro_deinit();
        puts("PASS held checkpoint clean stop");
        return 0;
    }
    if (ok && !strcmp(mode, "refuse"))
    {
        assert(until_logged("DULCAMARA-TEST: refusing"));
        probe_stop(true);
        puts("PASS refusal");
        fflush(NULL);
        _Exit(0);
    }
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
    else if (ok && !strcmp(mode, "sounds"))
    {
        ok = until_logged("DULCAMARA-TEST: started") && until_picture() && until_logged("sounds listed in");
        frames(120);
        const char *switch_off = getenv("RENPY_HEADLESS_SWITCH_OFF"), *base = getenv("renpy_sounds");
        if (ok && switch_off && base)
        {
            char path[1024];
            snprintf(path, sizeof(path), "%s.txt", base);
            FILE *f = fopen(path, "w");
            if (f)
            {
                fprintf(f, "# Dulcamara: switched off as the game runs\noff = %s\n", switch_off);
                fclose(f);
            }
            ok = f && until_logged_in_time("plays nothing for", 10);
            frames(90); /* what Ren'Py queued before plays out */
            sound_frames = sound_loud = 0;
            sound_peak = 0;
            frames(120);
        }
    }
    else if (ok && !strcmp(mode, "soak"))
        ok = until_logged("DULCAMARA-TEST: started") && until_picture() && soak();
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
        /* RENPY_HEADLESS_UNTIL: and on until the log has that (a test game's step that takes a while) */
        const char *until = getenv("RENPY_HEADLESS_UNTIL");
        if (ok && until && *until)
            ok = until_logged(until);
        /* RENPY_HEADLESS_LISTEN=<frames>: then the sound of only that many frames more is reported (whether a game's
         * music still plays, there and then) */
        const char *listen = getenv("RENPY_HEADLESS_LISTEN");
        if (ok && listen && atoi(listen) > 0)
        {
            sound_frames = sound_loud = 0;
            sound_peak = 0;
            frames(atoi(listen));
            printf("listened for %d frames\n", atoi(listen));
        }
    }
    else if (ok)
    {
        printf("FAIL: no mode %s\n", mode);
        ok = false;
    }
    if (geometry_w)
        printf("geometry: %ux%u\n", geometry_w, geometry_h);
    report();
    if (probe_runtime.version)
    {
        assert(probe_initial);
        bool unsafe_threads = getenv("DULCAMARA_RUNTIME_THREADS_UNSAFE") != NULL;
        probe_stop(unsafe_threads);
        if (unsafe_threads)
        {
            puts("PASS retained thread");
            fflush(NULL);
            _Exit(0);
        }
    }
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
