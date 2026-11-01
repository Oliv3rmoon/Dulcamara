/*
 * Dulcamara - RPG Maker 2000 / 2003 games on the PS5, on EasyRPG Player's libretro core.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dulcamara, a Nightshade app. The app: start-up, the main loop, the game screen, and what the screens share
 * (app.h). Its platform layer, its Vulkan screen and shaders, its interface primitives and its update check come
 * from Mandrake, formerly Nightshade Station (darkxex's SwanStationPS5 on BlackBearReloaded's
 * ps5-native-app-boilerplate).
 */
#include "app.h"

#include "config.h"
#include "core/cores.h"
#include "core/host.h"
#include "covers.h"
#include "i18n.h"
#include "platform/platform.h"
#include "platform/ps5_crash.h"
#include "platform/vk/vk_probe.h"
#include "ui/draw.h"
#include "ui/icons.h"
#include "ui/sfx.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "update.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_RATE 48000 /* EasyRPG's, and the interface sounds' */

App app = {.game = -1, .dt = 1.0f / 60.0f};

/* ---------------------------------------------------------------- notices */

static char toast[200];
static uint64_t toast_until, toast_length = 2500000;
static char toast_queue[4][200];
static uint64_t toast_queue_us[4];
static int toast_queued;

static void show_toast(const char *text, uint64_t length_us)
{
    str_copy(toast, sizeof(toast), text);
    toast_length = length_us;
    toast_until = plat_ticks_us() + length_us;
}

void app_toast(const char *message)
{
    app_toast_for(message, 2.5f);
}

void app_toast_for(const char *message, float seconds)
{
    np_log("notice: %s", message);
    const uint64_t length = (uint64_t)(seconds * 1e6f);
    if (toast[0] && plat_ticks_us() < toast_until)
    {
        if (!strcmp(toast, message) || toast_queued == 4)
            return;
        toast_queue_us[toast_queued] = length;
        str_copy(toast_queue[toast_queued++], sizeof(toast_queue[0]), message);
        return;
    }
    show_toast(tr(message), length);
}

static void toast_render(void)
{
    const uint64_t now = plat_ticks_us();
    if (toast_queued && (!toast[0] || now > toast_until))
    {
        show_toast(toast_queue[0], toast_queue_us[0]);
        memmove(toast_queue[0], toast_queue[1], sizeof(toast_queue[0]) * 3);
        memmove(toast_queue_us, toast_queue_us + 1, sizeof(toast_queue_us[0]) * 3);
        --toast_queued;
    }
    if (!toast[0] || now > toast_until)
        return;
    const float shown = (float)(toast_length - (toast_until - now)) / 1e6f, left = (float)(toast_until - now) / 1e6f;
    const float a = left < 0.3f ? left / 0.3f : 1.0f, rise = shown < 0.15f ? (1.0f - shown / 0.15f) * 24.0f : 0.0f;
    const float size = 26, w = text_width(size, FONT_REGULAR, toast) + 110, h = 64;
    const float x = (plat_width() - w) * 0.5f, y = plat_height() - 190 + rise;
    draw_rrect(x, y, w, h, 32, argb_alpha(TH_PILL_A(0xf0), a));
    icon_draw(ICON_INFO_CIRCLE, x + 22, y + 18, 28, argb_alpha(TH_FOCUS, a));
    text_draw(x + 62, y + 17, size, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_LEFT, toast);
}

/* ---------------------------------------------------------------- the message box */

static char box_text[600];
static bool box_open;
static float box_t;

void app_message_box(const char *message)
{
    str_copy(box_text, sizeof(box_text), tr(message));
    box_open = true;
    box_t = 0.0f;
}

static bool message_box_input(uint32_t *pressed)
{
    if (!box_open)
        return false;
    if (*pressed & (BIT(BTN_CROSS) | BIT(BTN_CIRCLE)))
    {
        box_open = false;
        sfx_play(SFX_BACK);
    }
    *pressed = 0;
    return true;
}

/* Word-wrapped text, centred, line by line; returns the height used. */
static float draw_wrapped(const char *m, float cx, float y, float width, float size, uint32_t argb, float bottom)
{
    char line[256];
    const float top = y;
    while (*m && y < bottom)
    {
        size_t best = 0;
        for (size_t i = 0;;)
        {
            size_t j = i;
            while (m[j] && m[j] != ' ' && m[j] != '\n')
                ++j;
            const size_t n = j < sizeof(line) - 1 ? j : sizeof(line) - 1;
            memcpy(line, m, n);
            line[n] = '\0';
            if (best > 0 && text_width(size, FONT_REGULAR, line) > width)
                break;
            best = n;
            if (!m[j] || m[j] == '\n')
                break;
            i = j + 1;
        }
        memcpy(line, m, best);
        line[best] = '\0';
        text_draw(cx, y, size, FONT_REGULAR, argb, ALIGN_CENTER, line);
        y += size * 1.45f;
        m += best;
        while (*m == ' ')
            ++m;
        if (*m == '\n')
            ++m;
    }
    return y - top;
}

static void message_box_draw(void)
{
    if (!box_open)
        return;
    box_t = fminf(box_t + app.dt * 8.0f, 1.0f);
    const float w = 860, h = 520, pad = 52, size = 28;
    const float x = (plat_width() - w) * 0.5f, y = (plat_height() - h) * 0.5f + (1.0f - box_t) * 24.0f;
    draw_rect(0, 0, (float)plat_width(), (float)plat_height(), argb_alpha(0xb0000000u, box_t));
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(0xf8), box_t));
    draw_rect(x, y, w, 4, argb_alpha(TH_FOCUS, box_t));
    icon_draw(ICON_INFO_CIRCLE, x + (w - 56) * 0.5f, y + pad, 56, argb_alpha(TH_FOCUS, box_t));
    draw_wrapped(box_text, x + w * 0.5f, y + pad + 84, w - 2 * pad, size, argb_alpha(TH_TEXT, box_t), y + h - 110);
    const float hs = 26, gw = pad_glyph_width(GLYPH_CROSS, hs);
    const char *close = tr("Close");
    const float hw = gw + hs * 0.35f + text_width(hs, FONT_REGULAR, close);
    const float hx = x + (w - hw) * 0.5f, hy = y + h - pad - hs;
    draw_pad_glyph(GLYPH_CROSS, hx + gw * 0.5f, hy + hs * 0.6f, hs);
    text_draw(hx + gw + hs * 0.35f, hy, hs, FONT_REGULAR, argb_alpha(TH_TEXT, box_t), ALIGN_LEFT, close);
}

/* ---------------------------------------------------------------- shared helpers */

void app_draw_hints(const int *glyphs, const char *const *labels, int count, const char *right)
{
    draw_rect(TH_MARGIN, TH_HINT_Y - 22, plat_width() - 2 * TH_MARGIN, 2, TH_DIVIDER);
    float x = TH_MARGIN;
    for (int i = 0; i < count; ++i)
        x += draw_hint(x, TH_HINT_Y, (enum PadGlyph)glyphs[i], tr(labels[i]), 26, TH_HINT);
    if (right)
        text_draw(plat_width() - TH_MARGIN, TH_HINT_Y, 26, FONT_REGULAR, TH_HINT, ALIGN_RIGHT, tr(right));
}

int app_nav_pad(void)
{
    for (int i = 0; i < NP_MAX_PADS; ++i)
        if (app.pads[i].connected)
            return i;
    return 0;
}

/* Button presses for the menus, with auto-repeat on directions and shoulders; the navigating controller's. */
static uint32_t nav_pressed(const PadState *pads)
{
    static uint32_t previous;
    static uint64_t repeat_at;
    static int repeats;
    const PadState *p = &pads[app_nav_pad()];
    uint32_t held = p->buttons;
    if (p->ly < -20000)
        held |= BIT(BTN_UP);
    if (p->ly > 20000)
        held |= BIT(BTN_DOWN);
    if (p->lx < -20000)
        held |= BIT(BTN_LEFT);
    if (p->lx > 20000)
        held |= BIT(BTN_RIGHT);
    uint32_t pressed = held & ~previous;
    const uint32_t dirs =
        BIT(BTN_UP) | BIT(BTN_DOWN) | BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_L1) | BIT(BTN_R1);
    const uint64_t now = plat_ticks_us();
    if (pressed & dirs)
    {
        repeat_at = now + 260000;
        repeats = 0;
    }
    else if ((held & dirs) && now >= repeat_at)
    {
        pressed |= held & dirs;
        repeat_at = now + (repeats < 6 ? 70000 : 40000);
        ++repeats;
    }
    previous = held;
    return pressed;
}

void app_apply_settings(void)
{
    sfx_configure(app.settings.ui_sound, (app.settings.ui_volume + 1) * 25);
    host_set_volume(app.settings.game_volume);
}

void app_save_settings(void)
{
    if (!app.storage_error[0] && !config_save(&app.settings, app.paths.config))
        np_log("settings: could not write %s", app.paths.config);
}

void app_open_settings(enum Screen back_to)
{
    app.settings_return = back_to;
    app.screen = SCREEN_SETTINGS;
    settings_opened();
}

void app_rescan(void)
{
    library_scan(&app.library, app.paths.games);
    library_load_played(&app.library, app.paths.played);
    library_sort(&app.library, app.settings.sort_mode);
    text_want_clear();
    for (int i = 0; i < app.library.count; ++i)
        text_want(app.library.games[i].title); /* Japanese titles: their characters into the atlases */
    covers_start(&app.library);
    np_log("library: %d game(s) in %s", app.library.count, app.paths.games);
    shelf_library_changed();
}

/* ---------------------------------------------------------------- the game */

/* Ren'Py runs one game each time the app starts (src/core/renpy/python.c): for another, the app starts again and opens
 * it then. Its id waits in this file, which the next start reads once. */
static void restart_file(char *out, size_t size)
{
    path_join(out, size, app.paths.root, "restart-game.txt");
}

#if defined(__PROSPERO__)
extern int sceSystemServiceLoadExec(const char *path, const char **arguments);

/* Starts the app again for g. The console's shell may refuse to start a title from itself: then the app closes, as
 * its own exit does, and the game opens at the next start. Does not return. */
static void restart_for(const Game *g)
{
    char file[NP_PATH_MAX], eboot[NP_PATH_MAX];
    restart_file(file, sizeof(file));
    FILE *f = fopen(file, "w");
    if (f)
    {
        fprintf(f, "%s\n", g->id);
        fclose(f);
    }
    plat_asset_path(eboot, sizeof(eboot), "../eboot.bin");
    np_log("game: starting again for %s (%s)", g->title, eboot);
    app_save_settings();
    icons_shutdown();
    text_shutdown();
    plat_release();
    fflush(NULL);
    int rc = sceSystemServiceLoadExec(eboot, NULL);
    np_log("game: LoadExec %s refused rc=0x%x; closing, the game opens at the next start", eboot, (unsigned)rc);
    fflush(NULL);
    for (;;)
    {
        rc = sceSystemServiceLoadExec("exit", NULL);
        plat_sleep_us(1000000);
    }
}
#endif

/* A Ren'Py game's keys (Keys for this game in its menu, ui/settings.c): keys/<its id>.txt in the data folder. */
static void keys_file(const Game *g, char *out, size_t size)
{
    char name[40];
    snprintf(name, sizeof(name), "%s.txt", g->id);
    path_join(out, size, app.paths.keys, name);
}

void app_apply_keys(void)
{
    for (int pad = 0; pad < MAP_BUTTONS; ++pad)
        if (config_key_option(pad))
            host_set_option(config_key_option(pad), KEY_ACTIONS[app.keys[pad]].value);
}

void app_save_keys(void)
{
    if (app.game < 0 || app.storage_error[0])
        return;
    const Game *g = &app.library.games[app.game];
    char file[NP_PATH_MAX];
    keys_file(g, file, sizeof(file));
    make_dirs(app.paths.keys);
    if (config_save_keys(app.keys, file, g->title))
        np_log("keys: %s for %s", file, g->title);
    else
        np_log("keys: could not write %s", file);
}

/* At start: the game a restart was for, if any (and the file gone, so it opens once). */
static void open_restart_game(void)
{
    char file[NP_PATH_MAX], id[64] = "";
    restart_file(file, sizeof(file));
    FILE *f = fopen(file, "r");
    if (!f)
        return;
    if (!fgets(id, sizeof(id), f))
        id[0] = '\0';
    fclose(f);
    remove(file);
    id[strcspn(id, "\r\n")] = '\0';
    for (int i = 0; i < app.library.count; ++i)
        if (!strcmp(app.library.games[i].id, id))
        {
            np_log("game: opening %s, which the app started again for", app.library.games[i].title);
            app_start_game(i);
            return;
        }
}

void app_start_game(int index)
{
    if (index < 0 || index >= app.library.count)
        return;
    Game *g = &app.library.games[index];
    char error[256];
#if defined(__PROSPERO__)
    if (core_is_renpy(g->core) && app.renpy_ran)
        restart_for(g); /* either Ren'Py's: each brings its own SDL and OpenGL driver, set up once in a process */
#endif
    np_log("game: starting %s (%s)", g->title, g->path);
    ps5_crash_step("loading a game");
    host_set_option("renpy_skip_unseen", app.settings.renpy_skip_unseen ? "on" : "off");
    if (core_is_renpy(g->core))
    {
        /* every button's key, the game's own or the core's: none stays from the game before */
        char file[NP_PATH_MAX];
        keys_file(g, file, sizeof(file));
        config_load_keys(app.keys, file);
        app_apply_keys();
        int own = 0;
        for (int pad = 0; pad < MAP_BUTTONS; ++pad)
            own += app.keys[pad] != 0;
        if (own)
            np_log("keys: %d of the buttons press %s's own keys (%s)", own, g->title, file);
    }
    if (!host_load(g, app.paths.root, error, sizeof(error)))
    {
        app_message_box(error);
        plat_audio_open(AUDIO_RATE);
        return;
    }
    host_set_volume(app.settings.game_volume);
    if (core_is_renpy(g->core))
        app.renpy_ran = true;
    app.game = index;
    app.fast_forward = app.fast_latched = false;
    app.screen = SCREEN_GAME;
    library_mark_played(&app.library, index, app.paths.played);
    app.settings.last_game = index;
    app_save_settings();
}

void app_stop_game(void)
{
    if (app.game < 0)
        return;
    host_unload();
    plat_audio_open(AUDIO_RATE);
    plat_audio_clear();
    np_log("game: back to the library");
    app.game = -1;
    app.fast_forward = app.fast_latched = false;
    app.screen = SCREEN_LIBRARY;
    shelf_open();
}

void app_draw_game(uint8_t dim)
{
    if (app.game < 0)
        return;
    int w, h, format;
    size_t pitch;
    bool fresh;
    const void *pixels = host_frame(&w, &h, &pitch, &format, &fresh);
    /* a shader reads the game's own pixels: no upscaling before it */
    if (pixels && fresh)
        plat_upload_game(pixels, w, h, pitch, format, app.settings.shader ? 1 : app.settings.upscale,
                         app.settings.upscale_filter);
    if (!pixels)
        return;
    const float aspect = app.settings.aspect == 1 ? 16.0f / 9.0f : host_aspect();
    plat_draw_game(&app.settings, aspect, dim);
}

static const int FF_SPEEDS[] = {2, 3, 4, 6, 8};

static void game_screen(PadState *pads, uint32_t pressed)
{
    const PadState *p = &pads[app_nav_pad()];
    /* In a Ren'Py game the touchpad is the pointer, its click a click where it points ("let touchpad use the cursor",
     * Ren'Py's first tester): Options held for 0.6 s opens Dulcamara's menu, and a short press of Options is the
     * game's, given to it as Options is let go. */
    static uint64_t options_since;
    static bool options_held, options_taken;
    static int options_frames;
    const bool own_options = host_core_skips(), options = (p->buttons & BIT(BTN_START)) != 0;
    if (options && !options_held)
    {
        options_since = plat_ticks_us();
        options_taken = false;
    }
    bool open_menu = !own_options && (pressed & BIT(BTN_MENU));
    if (own_options && options && !options_taken && plat_ticks_us() - options_since >= 600000)
        options_taken = open_menu = true;
    if (own_options && !options && options_held && !options_taken)
        options_frames = 6;
    options_held = options;
    if (open_menu)
    {
        options_frames = 0;
        menu_open();
        app_draw_game(255);
        return;
    }
    /* R2: fast-forward, while held or turned on and off */
    static bool r2_was;
    const bool r2 = (p->buttons & BIT(BTN_R2)) || p->r2 > 160;
    if (app.settings.ff_toggle)
    {
        if (r2 && !r2_was)
            app.fast_forward = app.fast_latched = !app.fast_forward;
    }
    else
        app.fast_forward = r2 || app.fast_latched;
    r2_was = r2;
    host_set_fast_forward(app.fast_forward);

    PadState game_pad = *p;
    game_pad.buttons &= ~(BIT(BTN_MENU) | BIT(BTN_R2));
    if (own_options)
    {
        game_pad.buttons &= ~BIT(BTN_START);
        if (options_frames > 0)
        {
            game_pad.buttons |= BIT(BTN_START);
            --options_frames;
        }
        game_pad.buttons |= p->buttons & BIT(BTN_MENU); /* the touchpad's click, to the pointer (core/host.c) */
    }
    host_set_input(&game_pad, &app.settings);

    /* Ren'Py's cores skip at Ren'Py's own pace: 0.2.5 ran one up to 8 times a frame, each a picture drawn on the GPU
     * and read back, and turning fast-forward on and off that way crashed it and then the console */
    const bool skips = host_core_skips();
    const int runs = app.fast_forward && !skips ? FF_SPEEDS[app.settings.ff_speed % 5] : 1;
    /* The game's time: what passed since the last frame. On a 59.94 Hz screen that is a hair more than EasyRPG's
     * 60 Hz step, which every 16 seconds would give one frame two steps and a stutter: within 2 % of a step, the
     * step itself (the game runs 0.1 % slow; its sound plays at its own pace either way). */
    double seconds = app.dt > 0.05f ? 0.05 : app.dt < 0.001f ? 1.0 / 60.0 : app.dt;
    const double step = 1.0 / host_fps();
    if (seconds > step * 0.98 && seconds < step * 1.02)
        seconds = step;
    host_run(runs, seconds);
    host_feed_audio(0.07);

    /* the game's frames a second, for Show FPS */
    static uint64_t window_start;
    static int frames;
    const uint64_t now = plat_ticks_us();
    frames += runs;
    if (now - window_start >= 1000000)
    {
        app.fps = window_start ? frames * 1e6f / (float)(now - window_start) : 0;
        frames = 0;
        window_start = now;
    }

    if (host_shutdown_requested())
    {
        app_stop_game();
        app_toast("The game closed.");
        return;
    }
    app_draw_game(255);
    if (app.fast_forward)
    {
        char speed[16];
        if (skips)
            snprintf(speed, sizeof(speed), "%s", tr("Skip"));
        else
            snprintf(speed, sizeof(speed), "x%d", runs);
        const float w = 150;
        draw_rrect(plat_width() - w - 30, 30, w, 60, 30, 0xb0000000u);
        icon_draw(ICON_PLAYER_TRACK_NEXT, plat_width() - w - 12, 38, 44, TH_FOCUS);
        text_draw(plat_width() - 60, 42, 30, FONT_BOLD, TH_TEXT, ALIGN_RIGHT, speed);
    }
    if (app.settings.show_fps)
    {
        char f[32];
        snprintf(f, sizeof(f), "%.1f FPS", app.fps);
        const float w = text_width(26, FONT_BOLD, f) + 40;
        draw_rrect(24, 24, w, 52, 26, 0xb0000000u);
        text_draw(44, 36, 26, FONT_BOLD, TH_GOOD, ALIGN_LEFT, f);
    }
}

/* ---------------------------------------------------------------- start-up */

static void make_folders(void)
{
    char rtp2000[NP_PATH_MAX], rtp2003[NP_PATH_MAX];
    path_join(rtp2000, sizeof(rtp2000), app.paths.rtp, "2000");
    path_join(rtp2003, sizeof(rtp2003), app.paths.rtp, "2003");
    const char *dirs[] = {app.paths.root, app.paths.games, app.paths.rtp, rtp2000, rtp2003, app.paths.logs,
                          app.paths.cache};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        make_dirs(dirs[i]);
    char probe[NP_PATH_MAX];
    path_join(probe, sizeof(probe), app.paths.root, ".write-test");
    FILE *f = fopen(probe, "w");
    if (f)
    {
        fclose(f);
        remove(probe);
    }
    else
        snprintf(app.storage_error, sizeof(app.storage_error), "%s isn't writable. Is the HEN running? (%s)",
                 app.paths.root, app.sandbox_reason[0] ? app.sandbox_reason : "no reason given");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char root[NP_PATH_MAX];
    plat_default_root(root, sizeof(root));
    config_paths(&app.paths, root);
    /* Unlock before SDL starts any thread: the HEN changes this process's credentials. */
    app.sandboxed = !plat_prepare_storage(app.sandbox_reason, sizeof(app.sandbox_reason));
    setenv("PS5_VIDEOOUT_PARAM_JSON", "/nonexistent", 1);
    plat_want_high_refresh(false);
    plat_want_output(2); /* 4K, 1080p when the TV lacks it */
    if (!plat_init())
    {
        char msg[300];
        snprintf(msg, sizeof(msg), NP_TITLE ": %s", plat_init_error());
        plat_notify(msg);
        for (;;)
            plat_sleep_us(1000000);
    }
    if (!text_init())
        plat_notify(NP_TITLE ": interface font missing from assets/fonts");
    theme_apply(THEME_NIGHTSHADE);
    i18n_set(LANG_EN);

    make_folders();
    char log_path[NP_PATH_MAX];
    path_join(log_path, sizeof(log_path), app.paths.logs, "dulcamara.log");
    np_log_open(log_path);
    ps5_crash_install(log_path);
#if defined(__PROSPERO__)
    extern int ps5_klog_capture_stderr(const char *prefix);
    ps5_klog_capture_stderr(""); /* nc <PS5 IP> 3232 shows the log live */
#endif
    np_log(NP_TITLE " %s starting, data folder %s", NP_VERSION, app.paths.root);
    np_log("screen: %s", plat_screen_info());
    np_log(app.sandboxed ? "storage: sandboxed (%s)" : "storage: unlocked%s", app.sandboxed ? app.sandbox_reason : "");
    for (int i = 0; i < CORE_COUNT; ++i)
    {
        struct retro_system_info info;
        memset(&info, 0, sizeof(info));
        if (!CORES[i].get_system_info)
            continue; /* not in this build */
        CORES[i].get_system_info(&info);
        np_log("core: %s %s for %s", CORES[i].name, info.library_version ? info.library_version : "", CORES[i].runs);
    }
    vk_probe(app.paths.root);

    config_defaults(&app.settings);
    config_load(&app.settings, app.paths.config);
    plat_audio_open(AUDIO_RATE);
    sfx_init(AUDIO_RATE);
    app_apply_settings();
    if (app.settings.update_check && !app.storage_error[0])
        update_check();
    if (!app.storage_error[0])
        app_rescan();
    shelf_open();
    if (app.storage_error[0])
        app_message_box(app.storage_error);
    else
        open_restart_game();

    bool quit = false;
    PadState pads[NP_MAX_PADS];
    uint64_t last = plat_ticks_us();
    const uint64_t opened = last;
    bool update_offered = false;
    while (!quit)
    {
        const uint64_t now = plat_ticks_us();
        app.dt = (float)(now - last) / 1e6f;
        if (app.dt > 0.1f)
            app.dt = 0.1f;
        last = now;

        /* an update found at start installs by itself, once, a second after the app opened (as Mandrake) */
        if (!update_offered && update_state() == UPDATE_AVAILABLE && now - opened >= 1000000 && app.game < 0)
        {
            update_offered = true;
            app_toast_for("An update is available and will be installed on your next restart.", 4.0f);
            update_install();
        }

        plat_poll(pads, &quit);
        if (app.quit_requested)
            quit = true;
        memcpy(app.pads, pads, sizeof(app.pads));
        uint32_t pressed = nav_pressed(pads);
        message_box_input(&pressed);
        const bool backdrop = app.screen == SCREEN_LIBRARY || (app.screen == SCREEN_SETTINGS && app.game < 0);
        plat_begin_frame(backdrop ? 0 : 0xff000000u);
        static const char *const names[SCREEN_COUNT] = {"library", "game", "menu", "settings", "keys"};
        ps5_crash_step(names[app.screen]);
        switch (app.screen)
        {
        case SCREEN_LIBRARY: shelf_screen(pressed); break;
        case SCREEN_GAME: game_screen(pads, pressed); break;
        case SCREEN_MENU: menu_screen(pressed); break;
        case SCREEN_SETTINGS: settings_screen(pressed); break;
        case SCREEN_KEYS: keys_screen(pressed); break;
        default: break;
        }
        covers_update(app.game >= 0 ? app.game : app.settings.last_game);
        message_box_draw();
        toast_render();
        plat_end_frame();
    }

    /* Only the desktop build gets here; the PS5 shell closes the title. */
    ps5_crash_step("shutdown");
    np_log("shutdown: leaving the main loop");
    if (app.game >= 0)
        app_stop_game();
    app_save_settings();
#if !defined(__PROSPERO__)
    covers_stop();
    plat_audio_close();
    icons_shutdown();
    text_shutdown();
    library_free(&app.library);
    plat_shutdown();
#else
    /* The system closes the title: exit()/_exit() raise SIGSYS on the console. LoadExec "exit" ends it normally. */
    extern int sceSystemServiceLoadExec(const char *path, const char **arguments);
    icons_shutdown();
    text_shutdown();
    plat_release();
    fflush(NULL);
    for (;;)
    {
        int rc = sceSystemServiceLoadExec("exit", NULL);
        np_log("shutdown: LoadExec exit refused rc=0x%x", (unsigned)rc);
        plat_sleep_us(1000000);
    }
#endif
    return 0;
}
