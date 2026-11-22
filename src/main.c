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
#include "core/vn_controls.h"

#include "config.h"
#include "core/cores.h"
#include "core/host.h"
#include "covers.h"
#include "i18n.h"
#include "platform/platform.h"
#include "platform/ps5_crash.h"
#include "platform/vk/vk_probe.h"
#include "ui/draw.h"
#include "ui/eclipse.h"
#include "ui/panels.h"
#include "ui/watermark.h"
#include "ui/story.h"
#include "ui/preview.h"
#include "ui/icons.h"
#include "ui/sfx.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/update_popup.h"
#include "update.h"
#include "restart.h"
#include "saves.h"
#include "profile.h"
#include "links.h"
#include "ui/links_ui.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

void app_clear_toasts(void)
{
    toast[0] = 0;
    toast_until = 0;
    toast_queued = 0;
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
    eclipse_strip(TH_MARGIN - 14, TH_HINT_Y - 24, plat_width() - 2 * TH_MARGIN + 28, 70);
    float x = TH_MARGIN;
    for (int i = 0; i < count; ++i)
        x += draw_hint(x, TH_HINT_Y, (enum PadGlyph)glyphs[i], tr(labels[i]), count > 5 ? 23 : 26,
                       TH_HINT);
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
static uint32_t previous;
static uint64_t repeat_at;
static int repeats;
static uint64_t options_since, fps_window_start;
static bool options_held, options_taken, r2_was;
static int options_frames, fps_frames;
static void reset_transient(void)
{
    previous = 0;
    repeat_at = 0;
    repeats = 0;
    options_since = fps_window_start = 0;
    options_held = options_taken = r2_was = false;
    options_frames = fps_frames = 0;
    app.fps = 0;
    app.dt = 0;
    app.fast_forward = app.fast_latched = false;
    host_reset_transient();
}
static uint32_t nav_pressed(const PadState *pads)
{
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

const Settings *app_game_settings(void)
{
    return app.game >= 0 ? &app.effective : &app.settings;
}
void app_apply_settings(void)
{
    app.effective = app.settings;
    if (app.game >= 0)
    {
        char name[40], path[NP_PATH_MAX];
        GameProfile profile;
        snprintf(name, sizeof(name), "%s.ini", app.library.games[app.game].id);
        path_join(path, sizeof(path), app.paths.profiles, name);
        profile_load(&profile, path);
        profile_resolve(&app.settings, &profile, &app.effective);
    }
    sfx_configure(app.settings.ui_sound, (app.settings.ui_volume + 1) * 25);
    host_set_volume(app_game_settings()->game_volume);
}

void app_save_settings(void)
{
    if (!app.storage_error[0] && !config_save(&app.settings, app.paths.config))
    {
        np_log("settings: could not write %s", app.paths.config);
        config_defaults(&app.settings);
        config_load(&app.settings, app.paths.config);
        app_apply_settings();
        app_toast("Settings could not be saved; previous defaults restored");
    }
}

void app_open_settings(enum Screen back_to)
{
    app.settings_return = back_to;
    app.screen = SCREEN_SETTINGS;
    settings_opened();
}

bool app_rebuild_library_view(void)
{
    if (!library_view_build(&app.library_view, &app.library, &app.metadata, &app.library_filter,
                            app.settings.sort_mode)) {
        app_toast("Not enough memory to update the library view"); return false;
    }
    shelf_library_changed(); return true;
}

void app_rescan(void)
{
    if (app.sandboxed)
    {
        app_retry_storage();
        return;
    }
    SavesStatus recovery;
    saves_poll(&recovery);
    if (recovery.operation == SAVES_RECOVER && recovery.state == SAVES_ERROR && !host_owns_runtime())
        saves_init(app.paths.root);
    if (!app.scanner) app.scanner = library_scanner_create();
    char cache[NP_PATH_MAX]; path_join(cache, sizeof(cache), app.paths.cache, "library-index.bin");
    if (!library_scanner_request(app.scanner, app.paths.games, cache)) {
        app.scan_status.state = LIBRARY_SCAN_ERROR;
        str_copy(app.scan_status.error, sizeof(app.scan_status.error), "Could not start the library scan");
        app_toast(app.scan_status.error); return;
    }
    library_scanner_poll(app.scanner, NULL, &app.scan_status);
}

static void open_restart_game(void);
/* Main-thread publication. A running game and settings submenus retain their
 * library indexes until returning to the shelf, so defer the pending snapshot. */
static void library_tick(void)
{
    if (!app.scanner) return;
    Library next = {0};
    enum LibraryScanState previous = app.scan_status.state;
    bool ready = library_scanner_poll(
        app.scanner,
        !host_owns_runtime() && app.game < 0 && app.screen == SCREEN_LIBRARY ? &next : NULL,
        &app.scan_status);
    if (ready) {
        library_load_played(&next, app.paths.played);
        LibraryView view = {0};
        if (library_view_build(&view, &next, &app.metadata, &app.library_filter, app.settings.sort_mode)) {
            library_view_free(&app.library_view); library_free(&app.library);
            app.library = next; app.library_view = view;
            text_want_clear();
            for (int i = 0; i < app.library.count; ++i) text_want(app.library.games[i].title);
            covers_start(&app.library); shelf_library_changed();
            np_log("library: %d game(s)%s, %d folder(s) not shown", app.library.count,
                   app.scan_status.cached ? " (cached)" : "", app.library.skipped_count);
        } else { library_free(&next); app_toast("Not enough memory to publish the library scan"); }
    }
    if (previous != app.scan_status.state && app.scan_status.state == LIBRARY_SCAN_ERROR) {
        np_log("library: %s", app.scan_status.error); app_toast(app.scan_status.error);
        if (app.sandboxed)
            np_log("library: sandboxed, so its folders can't be listed (%s)", app.sandbox_reason);
    }
    static bool restart_checked;
    if (!restart_checked && maintenance_ready() && !saves_busy() && app.game < 0 &&
        app.screen == SCREEN_LIBRARY &&
        (app.scan_status.state == LIBRARY_SCAN_DONE || app.scan_status.state == LIBRARY_SCAN_ERROR))
    {
        restart_checked = true;
        open_restart_game();
    }
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

/* LoadExec replaces the process on success. Keep the UI alive if the console declines. */
static void restart_for(const Game *g)
{
    char file[NP_PATH_MAX];
    restart_file(file, sizeof(file));
    if (host_owns_runtime())
    {
        app_message_box("The previous game has not safely stopped.");
        return;
    }
    if (saves_busy() || !maintenance_restart_ready())
    {
        app_message_box("Wait for save protection to finish before restarting.");
        return;
    }
    if (!restart_write(file, g->id))
    {
        app_message_box("Could not record the restart.");
        return;
    }
    np_log("game: starting again for %s", g->title);
    char eboot[NP_PATH_MAX];
    plat_asset_path(eboot, sizeof(eboot), "../eboot.bin");
    app_save_settings();
    fflush(NULL);
    int rc = sceSystemServiceLoadExec(eboot, NULL);
    np_log("game: relaunch returned 0x%x for %s", (unsigned)rc, g->title);
    app_message_box("Close and reopen Dulcamara to start the selected Ren'Py game.");
}
#endif

/* A Ren'Py game's keys (Keys for this game, from its menu or Settings > Ren'Py: ui/settings.c): keys/<its id>.txt in
 * the data folder. */
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

void app_load_keys(int index, int8_t keys[MAP_BUTTONS])
{
    if (index < 0 || index >= app.library.count)
    {
        memset(keys, 0, MAP_BUTTONS);
        return;
    }
    char file[NP_PATH_MAX];
    keys_file(&app.library.games[index], file, sizeof(file));
    config_load_keys(keys, file);
}

bool app_save_keys(int index, const int8_t keys[MAP_BUTTONS])
{
    if (index < 0 || index >= app.library.count || app.storage_error[0])
        return false;
    const Game *g = &app.library.games[index];
    char file[NP_PATH_MAX];
    keys_file(g, file, sizeof(file));
    make_dirs(app.paths.keys);
    if (config_save_keys(keys, file, g->title))
        return true;
    else
        app_message_box(
            "Keys could not be saved; previous keys remain effective. Check data storage.");
    return false;
}

/* A Ren'Py game's sounds (Sounds for this game, from its menu or Settings > Ren'Py: ui/settings.c): sounds/<its id>
 * in the data folder, .list its core's list of them and .txt the ones switched off (config.h). */
static void sounds_base(const Game *g, char *out, size_t size)
{
    path_join(out, size, app.paths.sounds, g->id);
}

bool app_load_sounds(int index, struct SoundList *sounds)
{
    if (index < 0 || index >= app.library.count)
    {
        config_free_sounds(sounds);
        return false;
    }
    char base[NP_PATH_MAX];
    sounds_base(&app.library.games[index], base, sizeof(base));
    return config_load_sounds(sounds, base);
}

bool app_save_sounds(int index, const struct SoundList *sounds)
{
    if (index < 0 || index >= app.library.count || app.storage_error[0])
        return false;
    const Game *g = &app.library.games[index];
    char base[NP_PATH_MAX];
    sounds_base(g, base, sizeof(base));
    make_dirs(app.paths.sounds);
    if (config_save_sounds(sounds, base, g->title))
        return true;
    else
        app_message_box("Sounds could not be saved; previous sound choices remain effective. Check "
                        "data storage.");
    return false;
}

/* At start: the game a restart was for, if any (and the file gone, so it opens once). */
static void open_restart_game(void)
{
    char file[NP_PATH_MAX], id[64] = "";
    restart_file(file, sizeof(file));
    if (!restart_read(file, id, sizeof(id)))
        return;
    for (int i = 0; i < app.library.count; ++i)
        if (!strcmp(app.library.games[i].id, id))
        {
            np_log("game: opening %s, which the app started again for", app.library.games[i].title);
            app_start_game(i);
            if (app.game == i)
                restart_acknowledge(file, id);
            return;
        }
}

/* Stable runtime identity: library snapshots may be replaced after clean stop. */
static Game save_game;
static uint64_t save_session, backup_job, stopped_session;
static uint32_t backup_epoch;
static char early_baseline_id[SAVE_ID_MAX];
static bool pre_backup, post_backup, backup_failed, post_failed, stop_pending, auto_backups;
static int pending_start = -1;
static bool pending_stop;
static double active_seconds;
static bool history_marked;
static void start_game_now(int index);

/* Task5 may offer retry, stop, or an explicit skip after showing the backup error.
 * A plain error dialog dismissal never silently resumes native writes. */
bool app_save_retry(void)
{
    if (!backup_failed || saves_busy())
        return false;
    backup_failed = false;
    if (post_failed) { post_failed = false; stopped_session = 0; }
    return true;
}
bool app_save_lifecycle_busy(void) { return pre_backup || post_backup || backup_failed; }
bool app_save_failure_is_poststop(void) { return post_failed; }
bool app_save_skip_protection(void)
{
    if (!backup_failed || saves_busy()) return false;
    if (post_failed) { post_failed = backup_failed = false; return true; }
    if (stop_pending) return false;
    bool ok = host_release_checkpoint(backup_epoch);
    if (ok)
        backup_failed = false;
    return ok;
}

void app_start_game(int index)
{
    if (index < 0 || index >= app.library.count || pending_start >= 0)
        return;
    pending_start = index; /* host load runs only after a transition frame is presented */
}
static void start_game_now(int index)
{
    if (index < 0 || index >= app.library.count)
        return;
    if (host_owns_runtime())
    {
        app_message_box("The previous game has not safely stopped.");
        return;
    }
    if (app.sandboxed)
    {
        app_message_box(app.storage_error);
        return;
    }
    if (!maintenance_ready() || saves_busy())
    {
        app_message_box("Wait for save recovery or maintenance to finish before playing.");
        return;
    }
    Game copied_game = app.library.games[index];
    Game *g = &copied_game;
    char error[256];
    if (!library_revalidate(g))
    {
        app_message_box("This game is no longer available at its saved location. Look again to "
                        "refresh the library.");
        return;
    }
#if defined(__PROSPERO__)
    if (core_is_renpy(g->core) && app.renpy_ran)
    {
        restart_for(g);
        return; /* never re-enter either Ren'Py runtime in the same process */
    }
#endif
    if (!maintenance_launch())
    {
        app_message_box("Save maintenance is busy. Try again when it finishes.");
        return;
    }
    save_game = *g;
    struct timespec session_clock;
    clock_gettime(CLOCK_REALTIME, &session_clock);
    save_session = (uint64_t)session_clock.tv_sec * 1000000000u + (uint64_t)session_clock.tv_nsec;
    pre_backup = post_backup = backup_failed = post_failed = stop_pending = false;
    early_baseline_id[0] = 0;
    GameProfile profile;
    Settings effective;
    char profile_path[NP_PATH_MAX], profile_name[40];
    snprintf(profile_name, sizeof(profile_name), "%s.ini", g->id);
    path_join(profile_path, sizeof(profile_path), app.paths.profiles, profile_name);
    profile_load(&profile, profile_path);
    profile_resolve(&app.settings, &profile, &effective);
    app.effective = effective;
    auto_backups = effective.automatic_backups;
    active_seconds = 0;
    history_marked = false;
    reset_transient();
    np_log("game: starting %s (%s)", g->title, g->path);
    ps5_crash_step("loading a game");
    host_set_option("renpy_skip_unseen", effective.renpy_skip_unseen ? "on" : "off");
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
        /* its sounds: the core lists them in sounds/<its id>.list and plays nothing for those its .txt has off */
        sounds_base(g, file, sizeof(file));
        make_dirs(app.paths.sounds);
        host_set_option("renpy_sounds", file);
    }
    if (!host_load(g, app.paths.root, error, sizeof(error)))
    {
        maintenance_runtime(false, false, false);
        app_message_box(error);
        plat_audio_open(AUDIO_RATE);
        return;
    }
    host_set_volume(effective.game_volume);
    if (core_is_renpy(g->core))
        app.renpy_ran = true;
    app.game = index;
    app.fast_forward = app.fast_latched = false;
    app.screen = SCREEN_GAME;

    library_select(&app.library, &app.settings, index);
    app_save_settings();
}

void app_stop_game(void)
{
    if (app.game < 0)
        return;
    stop_pending = true;
    if (!post_failed) backup_failed = false;
    if (pre_backup && saves_busy())
        saves_cancel();
    else
        pending_stop = true; /* present stopping before same-thread native shutdown */
    reset_transient();
}

static void runtime_tick(void)
{
    SavesStatus saved;
    saves_poll(&saved);
    static uint64_t reported_recovery;
    if (saved.operation == SAVES_RECOVER && saved.state == SAVES_ERROR &&
        reported_recovery != saved.serial)
    {
        reported_recovery = saved.serial;
        char reason[560];
        snprintf(reason, sizeof(reason), NP_TITLE " " NP_VERSION "\n%s\nPress Square in the library to retry recovery.", saved.result.error);
        app_message_box(reason);
        np_log("saves: %s", saved.result.error);
    }
    /* a finished recovery that left some game's backups as found names them; launches stay unlocked */
    static uint64_t logged_recovery_note;
    if (saved.operation == SAVES_RECOVER && saved.state == SAVES_DONE && saved.result.error[0] &&
        logged_recovery_note != saved.serial)
    {
        logged_recovery_note = saved.serial;
        np_log("saves: %s", saved.result.error);
    }
    if (saved.operation == SAVES_RECOVER && saved.state == SAVES_DONE &&
        reported_recovery != saved.serial && update_state() == UPDATE_INSTALLED)
    {
        reported_recovery = saved.serial;
        app_message_box("Update recovery finished. Close and reopen Dulcamara before playing.");
    }
    if (pre_backup && saved.serial == backup_job && saved.state != SAVES_WORKING)
    {
        pre_backup = false;
        if (stop_pending)
            host_request_stop();
        else if (saved.state == SAVES_DONE)
        {
            str_copy(early_baseline_id, sizeof(early_baseline_id), saved.result.snapshot);
            host_release_checkpoint(backup_epoch);
        }
        else
        {
            backup_failed = true;
            app_message_box(
                "The pre-play backup failed. The game remains paused before native writes. "
                "Stop the game, or use the save controls to retry or explicitly skip protection.");
            np_log("saves: %s", saved.result.error);
        }
    }
    if (post_backup && saved.serial == backup_job && saved.state != SAVES_WORKING)
    {
        post_backup = false;
        if (saved.state != SAVES_DONE) {
            backup_failed = post_failed = true;
            app_message_box("The post-stop backup failed. Saves remain unchanged. Retry or explicitly skip protection in the save controls before launching or installing an update.");
            np_log("saves: %s", saved.result.error);
        }
    }
    enum HostState state = host_poll();
    struct dr_save_layout layout;
    bool eligible = host_snapshot_eligible();
    /* Keep the runtime reservation across the clean-stop -> post-backup handoff.
     * An updater worker cannot overtake either scheduling or worker completion. */
    bool protection_reserved = host_owns_runtime() ||
        (app.game >= 0 && (state == HOST_IDLE || state == HOST_RESTART_REQUIRED));
    maintenance_runtime(protection_reserved, eligible,
                        state == HOST_FAILURE && host_owns_runtime());
    if ((state == HOST_LOADING || state == HOST_RUNNING) && !stop_pending && !pre_backup &&
        !backup_failed && host_copy_save_layout(&layout) && layout.checkpoint)
    {
        if (!eligible)
            host_release_checkpoint(layout.generation); /* later roots: no global quiescence */
        else
        {
            SaveRequest request = {.core = (uint32_t)save_game.core,
                                   .session = save_session,
                                   .kind = layout.preinitial ? SAVE_PRE_INITIAL : SAVE_AUTOMATIC,
                                   .layout = layout};
            str_copy(request.game_id, sizeof(request.game_id), save_game.id);
            str_copy(request.baseline, sizeof(request.baseline), early_baseline_id);
            backup_epoch = layout.generation;
            if (saves_start(auto_backups ? SAVES_CREATE : SAVES_REGISTER, &request, NULL, false))
            {
                pre_backup = true;
                saves_poll(&saved);
                backup_job = saved.serial;
            }
        }
    }
    if (app.game >= 0 && !host_owns_runtime() &&
        (state == HOST_IDLE || state == HOST_RESTART_REQUIRED))
    {
        /* Exactly one post-stop job; never after timeout, never while stop writers run. */
        if (stopped_session != save_session)
        {
            stopped_session = save_session;
            if (eligible && host_copy_save_layout(&layout))
            {
                SaveRequest request = {.core = (uint32_t)save_game.core,
                                       .session = save_session,
                                       .kind = SAVE_AUTOMATIC,
                                       .layout = layout};
                str_copy(request.game_id, sizeof(request.game_id), save_game.id);
                post_backup =
                    saves_start(app.effective.automatic_backups ? SAVES_CREATE : SAVES_REGISTER,
                                &request, NULL, false);
                if (post_backup)
                {
                    saves_poll(&saved);
                    backup_job = saved.serial;
                }
                if (!post_backup) {
                    backup_failed = post_failed = true;
                    app_message_box("Could not start the post-stop backup. Retry or explicitly skip protection in the save controls before playing or installing an update.");
                }
            } else if (auto_backups) {
                app_toast("Native save locations were unavailable; no post-stop backup was made.");
            }
        }
        if (post_backup || saves_busy() || post_failed)
            return;
        maintenance_runtime(false, false, false);
        plat_audio_open(AUDIO_RATE);
        reset_transient();
        np_log("game: back to the library after clean stop");
        if (active_seconds >= 1)
        {
            uint64_t seconds = (uint64_t)active_seconds;
            if (!user_metadata_add_playtime(&app.metadata, save_game.id, seconds) ||
                !user_metadata_save(&app.metadata, app.paths.metadata))
            {
                user_metadata_load(&app.metadata, app.paths.metadata);
                app_toast("Playtime could not be saved; previous history retained");
            }
        }
        active_seconds = 0;
        stop_pending = false;
        pending_stop = false;
        app.game = -1;
        app.effective = app.settings;
        app.screen = SCREEN_LIBRARY;
        app_rebuild_library_view();
        shelf_open();
    }
    static bool unsafe_shown;
    if (state == HOST_FAILURE && host_owns_runtime() && !unsafe_shown)
    {
        unsafe_shown = true;
        app_message_box("The game did not stop. Saves and a new launch remain locked. Close "
                        "Dulcamara to end the process.");
    }
}

static void transition_draw(uint32_t pressed)
{
    bool unsafe = host_state() == HOST_FAILURE && host_owns_runtime();
    bool busy = pending_start >= 0 || stop_pending || host_state() == HOST_LOADING ||
                host_state() == HOST_STOPPING || app_save_lifecycle_busy() || unsafe;
    if (!busy)
        return;
    SavesStatus status;
    saves_poll(&status);
    const char *label = unsafe                          ? "The game did not stop safely"
                        : backup_failed                 ? "Save protection failed"
                        : stop_pending                  ? "Stopping game safely"
                        : pending_start >= 0            ? "Loading game"
                        : status.state == SAVES_WORKING ? "Protecting native saves"
                                                        : "Loading game / native checkpoint";
    /* Story view draws starting a game as a chapter card; only the drawing differs, not the states or their keys */
    const int starting = pending_start >= 0 ? pending_start : app.game;
    if (story_view() && story_chapter_card(starting >= 0, stop_pending || host_state() == HOST_STOPPING, post_backup,
                                           unsafe || backup_failed))
        story_chapter_draw(starting, label);
    else
    {
        draw_rrect(320, 390, 1280, 270, 24, 0xfc101015);
        eclipse_status(365, 430, 70);
        text_draw(470, 440, 34, FONT_BOLD, TH_TEXT, ALIGN_LEFT, label);
        text_draw_fit(375, 520, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, 1170,
                      unsafe ? "Saves and new launches are locked. Close Dulcamara to end the process."
                      : backup_failed
                          ? status.result.error
                          : "Please wait. Native startup/stop may briefly occupy the main thread.");
    }
    if (backup_failed)
    {
        static bool confirm_skip;
        text_draw(375, 581, 24, FONT_REGULAR, TH_TEXT, ALIGN_LEFT,
                  confirm_skip ? "Triangle confirms skip. Circle cancels."
                               : "Cross Retry   Circle Stop   Triangle Skip protection");
        if (pressed & BIT(BTN_TRIANGLE))
        {
            if (confirm_skip)
            {
                confirm_skip = false;
                app_save_skip_protection();
            }
            else
            {
                confirm_skip = true;
                app_message_box("Skipping leaves this checkpoint without a new protection copy. "
                                "Existing snapshots remain. Dismiss this message, then Triangle "
                                "confirms or Circle cancels.");
            }
        }
        if (pressed & BIT(BTN_CIRCLE))
        {
            if (confirm_skip)
                confirm_skip = false;
            else
                app_stop_game();
        }
        if (pressed & BIT(BTN_CROSS))
        {
            confirm_skip = false;
            app_save_retry();
        }
    }
    else if ((pressed & BIT(BTN_CIRCLE)) && app.game >= 0 && !stop_pending)
        app_stop_game();
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
        plat_upload_game(pixels, w, h, pitch, format,
                         app_game_settings()->shader ? 1 : app_game_settings()->upscale,
                         app_game_settings()->upscale_filter);
    if (!pixels)
        return;
    const float aspect = app_game_settings()->aspect == 1 ? 16.0f / 9.0f : host_aspect();
    plat_draw_game(app_game_settings(), aspect, dim);
}

static const int FF_SPEEDS[] = {2, 3, 4, 6, 8};

/* A game's frames that came late (more than 25 ms after the one before, where 60 a second is 16.7): in the log with
 * how long running the game, drawing and presenting took, the first 30 and every 300th, and how many of each 3600
 * there were. A tester saw a Ren'Py game's black bars whenever Show FPS fell below 60, and only then. */
static uint64_t game_run_us, game_draw_us;

static void note_game_frame(uint64_t interval, uint64_t work, uint64_t present)
{
    static int frames, late, noted;
    static uint64_t slowest;
    ++frames;
    if (interval > 25000)
    {
        ++late;
        slowest = interval > slowest ? interval : slowest;
        if (++noted <= 30 || noted % 300 == 0)
            np_log("app: a late frame, %.1f ms after the one before: the game ran %.1f ms, drawing "
                   "took %.1f ms, the "
                   "rest of the frame %.1f ms, presenting %.1f ms",
                   interval / 1000.0, game_run_us / 1000.0, game_draw_us / 1000.0,
                   (work > game_run_us + game_draw_us ? work - game_run_us - game_draw_us : 0) / 1000.0,
                   present / 1000.0);
    }
    if (frames >= 3600)
    {
        np_log("app: %d of the game's last %d frames came late, the latest %.1f ms after the one "
               "before",
               late, frames, slowest / 1000.0);
        frames = late = 0;
        slowest = 0;
    }
}

static void game_screen(PadState *pads, uint32_t pressed)
{
    if (host_state() != HOST_RUNNING || stop_pending || app_save_lifecycle_busy())
    {
        app_draw_game(100);
        return;
    }
    const PadState *p = &pads[app_nav_pad()];
    /* In a Ren'Py game the touchpad is the pointer, its click a click where it points ("let touchpad use the cursor",
     * Ren'Py's first tester): Options held for 0.6 s opens Dulcamara's menu, and a short press of Options is the
     * game's, given to it as Options is let go. */
    const bool own_options = host_core_skips(), options = (p->buttons & BIT(BTN_START)) != 0;
    if (options && !options_held)
    {
        options_since = plat_ticks_us();
        options_taken = false;
    }
    bool open_menu = !own_options && (pressed & BIT(BTN_MENU));
    if (own_options && options && !options_taken && plat_ticks_us() - options_since >= 600000)
        options_taken = open_menu = true;
    const bool options_tapped = own_options && !options && options_held && !options_taken;
    if (options_tapped)
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
    const bool r2 = (p->buttons & BIT(BTN_R2)) || p->r2 > 160;
    if (app_game_settings()->ff_toggle)
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
    /* while the game wants text typed, the on-screen keyboard has the controller (ui/keyboard.c), Options' short
     * press too (its Done): the game's is an idle one */
    if (keyboard_frame(p, pressed, options_tapped, &game_pad))
    {
        options_frames = 0;
        game_pad = (PadState){.connected = p->connected};
    }
    if (own_options)
        vn_one_hand_input(&game_pad, app_game_settings()->vn_hand);
    host_set_input(&game_pad, app_game_settings());

    /* Ren'Py's cores skip at Ren'Py's own pace: 0.2.5 ran one up to 8 times a frame, each a picture drawn on the GPU
     * and read back, and turning fast-forward on and off that way crashed it and then the console */
    const bool skips = host_core_skips();
    const int runs = app.fast_forward && !skips ? FF_SPEEDS[app_game_settings()->ff_speed % 5] : 1;
    /* The game's time: what passed since the last frame. On a 59.94 Hz screen that is a hair more than EasyRPG's
     * 60 Hz step, which every 16 seconds would give one frame two steps and a stutter: within 2 % of a step, the
     * step itself (the game runs 0.1 % slow; its sound plays at its own pace either way). */
    double seconds = app.dt > 0.05f ? 0.05 : app.dt < 0.001f ? 1.0 / 60.0 : app.dt;
    const double step = 1.0 / host_fps();
    if (seconds > step * 0.98 && seconds < step * 1.02)
        seconds = step;
    const uint64_t run_start = plat_ticks_us();
    host_run(runs, seconds);
    host_feed_audio(0.07);
    game_run_us = plat_ticks_us() - run_start;

    /* the game's frames a second, for Show FPS */
    const uint64_t now = plat_ticks_us();
    fps_frames += runs;
    if (now - fps_window_start >= 1000000)
    {
        app.fps = fps_window_start ? fps_frames * 1e6f / (float)(now - fps_window_start) : 0;
        fps_frames = 0;
        fps_window_start = now;
    }

    if (host_shutdown_requested())
    {
        app_stop_game();
        app_toast("The game closed.");
        return;
    }
    const uint64_t draw_start = plat_ticks_us();
    app_draw_game(255);
    game_draw_us = plat_ticks_us() - draw_start;
    keyboard_draw();
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
    if (app_game_settings()->show_fps)
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
}

static void select_storage(void)
{
    char root[NP_PATH_MAX];
    plat_default_root(root, sizeof(root));
    app.sandboxed = !plat_prepare_storage(root, sizeof(root), app.sandbox_reason, sizeof(app.sandbox_reason));
    config_paths(&app.paths, root);
    str_copy(app.storage_error, sizeof(app.storage_error), app.sandboxed ? app.sandbox_reason : "");
    /* The folders, the log and the crash report come first, before the screen, the font and the startup recovery:
     * candidate ecl200-n5 stopped at launch on the console and left nothing at all, so each step now says it has
     * begun, and a crash in any of them is written down. */
    if (!app.sandboxed)
        make_folders();
    char log_path[NP_PATH_MAX];
    path_join(log_path, sizeof(log_path), app.paths.logs, "dulcamara.log");
    if (!app.sandboxed)
        np_log_open(log_path);
    np_log(NP_TITLE " %s starting, data folder %s", NP_VERSION, app.paths.root);
    np_log("storage: %s. %s", app.sandboxed ? "unavailable" : app.paths.root, app.sandbox_reason);
    if (!app.sandboxed)
        ps5_crash_install(log_path);
}

static void start_recovery(void)
{
    maintenance_recovery_begin(MAINT_RECOVER_SAVES | MAINT_RECOVER_UPDATE);
    saves_set_update_recovery(update_recover_startup, NULL);
    const bool update_ready = update_init(NULL);
    ps5_crash_step("startup: save recovery start");
    const bool saves_ready = !app.sandboxed && saves_init(app.paths.root);
    np_log("startup: update recovery %s, save recovery %s", update_ready ? "set up" : "has no usable app folder path",
           saves_ready ? "started" : "could not start");
    if (!saves_ready && !app.sandboxed)
        app_message_box("Save recovery could not start. Game launches remain locked.");
}

static void storage_notice(void)
{
    char message[600];
    if (!app.sandbox_reason[0])
        return;
    if (app.sandboxed)
        snprintf(message, sizeof(message), NP_TITLE " " NP_VERSION "\n%s\nCheck the drive, then press Square to retry storage.", app.storage_error);
    else
        snprintf(message, sizeof(message), NP_TITLE " " NP_VERSION "\n%s\nUsing a separate library in %s. Your original games and saves stay in their folder. Use a marked USB library for larger games.", app.sandbox_reason, app.paths.root);
    app_message_box(message);
}

void app_retry_storage(void)
{
    if (!app.sandboxed || host_owns_runtime() || saves_busy())
        return;
    select_storage();
    if (!app.sandboxed)
    {
        config_defaults(&app.settings);
        config_load(&app.settings, app.paths.config);
        user_metadata_load(&app.metadata, app.paths.metadata);
        app_apply_settings();
        start_recovery();
        links_cleanup(app.paths.games);
        app_rescan();
        app_toast("Storage available. Looking for games.");
    }
    storage_notice();
}

int main(int argc, char **argv)
{
#if defined(NP_PREVIEW)
    uint64_t startup_wall = SDL_GetPerformanceCounter();
#endif
    (void)argc;
    (void)argv;
    select_storage();
    ps5_crash_step("startup: screen");
    np_log("startup: screen");
    setenv("PS5_VIDEOOUT_PARAM_JSON", "/nonexistent", 1);
    plat_want_high_refresh(false);
    plat_want_output(2); /* 4K, 1080p when the TV lacks it */
    if (!plat_init())
    {
        char msg[300];
        snprintf(msg, sizeof(msg), NP_TITLE ": %s", plat_init_error());
        np_log("startup: %s", msg);
        plat_notify(msg);
        for (;;)
            plat_sleep_us(1000000);
    }
    ps5_crash_step("startup: font");
    np_log("startup: font");
    if (!text_init())
        plat_notify(NP_TITLE ": interface font missing from assets/fonts");
    theme_apply(THEME_NIGHTSHADE);
    i18n_set(LANG_EN);

#if defined(__PROSPERO__)
    extern int ps5_klog_capture_stderr(const char *prefix);
    ps5_klog_capture_stderr(""); /* nc <PS5 IP> 3232 shows the log live */
#endif
    ps5_crash_step("startup: update recovery setup");
    start_recovery();
    ps5_crash_step("startup");
    np_log("screen: %s", plat_screen_info());
    for (int i = 0; i < CORE_COUNT; ++i)
    {
        struct retro_system_info info;
        memset(&info, 0, sizeof(info));
        if (!CORES[i].get_system_info)
            continue; /* not in this build */
        CORES[i].get_system_info(&info);
        np_log("core: %s %s for %s", CORES[i].name, info.library_version ? info.library_version : "", CORES[i].runs);
    }
    if (!app.sandboxed)
        vk_probe(app.paths.root);

    config_defaults(&app.settings);
    config_load(&app.settings, app.paths.config);
    app.library_filter.section = -1;
    user_metadata_load(&app.metadata, app.paths.metadata);
    plat_audio_open(AUDIO_RATE);
    sfx_init(AUDIO_RATE);
    app_apply_settings();
    eclipse_init();
    bool update_check_started = false;
    if (!app.storage_error[0])
        links_cleanup(app.paths.games); /* what a download interrupted by a crash left (links.h) */
    if (!app.storage_error[0])
        app_rescan();
    shelf_open();
    storage_notice();

    bool quit = false;
    PadState pads[NP_MAX_PADS];
    uint64_t last = plat_ticks_us(), last_frame_start = last;
    const uint64_t opened = last;
    bool update_offered = false;
    bool in_background = false;
    bool previously_active = false;
    bool privacy_armed = false, privacy_resuming = false;
#if defined(NP_PREVIEW)
    /* Optional host-clock receipt. Buffered writes are outside the measured
     * interval; deterministic preview time never supplies these durations. */
    FILE *metrics = NULL;
    const char *metrics_path = SDL_getenv("NP_METRICS");
    if (metrics_path && metrics_path[0])
    {
        metrics = fopen(metrics_path, "w");
        if (!metrics)
            return 1;
        fprintf(metrics, "frame,screen,count,selected,pressed,frame_ms,input_ms,publish_ms,"
                         "decoded_bytes,texture_bytes,upload_bytes\n");
    }
    unsigned metric_frame = 0;
    np_log("preview real startup_ms=%.3f (platform/font/skin/initial scan scheduling)",
           (SDL_GetPerformanceCounter() - startup_wall) * 1000.0 / SDL_GetPerformanceFrequency());
#endif
    while (!quit)
    {
        if (!update_check_started && maintenance_ready() && !saves_busy())
        {
            update_check_started = true;
            if (app.settings.update_check && !app.storage_error[0])
                update_check();
        }
#if defined(NP_PREVIEW)
        /* The production poll order is kept. A scripted press belongs to this
         * frame, so input-to-present is measured from the frame start: runtime
         * and library publication work before the poll is included. */
        uint64_t wall_start = SDL_GetPerformanceCounter();
#endif
        const uint64_t now = plat_ticks_us();
        app.dt = (float)(now - last) / 1e6f;
        if (app.dt > 0.1f)
            app.dt = 0.1f;
        last = now;

        /* While another app has the screen (the PS5 switched to one, or to its home screen), nothing runs: no game,
         * no drawing, no presenting, no sound. Switching away from a running Ren'Py game, back and away again took the
         * console down, as opening Dulcamara after rest mode once did, with the core's OpenGL, its read-back and the
         * app's Vulkan presents all going on behind the other app. */
        if (plat_in_background())
        {
            if (!in_background)
            {
                in_background = true;
                privacy_armed = privacy_resuming = false;
                previously_active = false;
                np_log("app: another app has the screen: the game, drawing and sound stop");
                reset_transient();
            }
            host_poll(); /* finish stops, but keep startup checkpoints held in the background */
            plat_poll(pads, &quit);
            plat_sleep_us(50000);
            continue;
        }
        if (in_background)
        {
            in_background = false;
            np_log("app: the screen is the app's again");
            last = last_frame_start = plat_ticks_us();
            app.dt = 0;
            reset_transient();
        }

        runtime_tick();
#if defined(NP_PREVIEW)
        uint64_t wall_publish = SDL_GetPerformanceCounter();
#endif
        library_tick();
#if defined(NP_PREVIEW)
        double publish_ms = (SDL_GetPerformanceCounter() - wall_publish) * 1000.0 /
                            SDL_GetPerformanceFrequency();
#endif
        links_ui_tick(); /* after library_tick: a game added from a link is selected once it is published */
        eclipse_tick(app.dt, app.settings.still_background, app.game >= 0);
        if (app.game >= 0 && host_state() == HOST_RUNNING && app.screen == SCREEN_GAME &&
            !stop_pending && !box_open)
        {
            if (!history_marked)
            {
                history_marked = true;
                if (!library_mark_played(&app.library, app.game, app.paths.played))
                    app_toast("Last-played time could not be saved");
            }
            if (previously_active && !app.privacy_active)
                active_seconds += app.dt;
            previously_active = true;
        }
        else
            previously_active = false;

        /* an update found at start is offered in a popup, once, a second after the app opened, on the library and
         * with no message box up (as Nightshade offers its own; until 0.2.12 it installed by itself, with a toast) */
        if (!update_offered && update_state() == UPDATE_AVAILABLE && now - opened >= 1000000 &&
            app.game < 0 && app.screen == SCREEN_LIBRARY && !box_open)
        {
            update_offered = true;
            update_popup_open();
        }

        plat_poll(pads, &quit);
        if (app.quit_requested)
            quit = true;
        for (int p = 0; p < NP_MAX_PADS; ++p)
            if (pads[p].connected != app.pads[p].connected)
                reset_transient();
        memcpy(app.pads, pads, sizeof(app.pads));
        static int previous_screen = -1;
        static uint32_t withheld[NP_MAX_PADS];
        static bool withheld_stick[NP_MAX_PADS], withheld_touch[NP_MAX_PADS];
        if (previous_screen != (int)app.screen)
        {
            reset_transient();
            previous_screen = app.screen;
            for (int p = 0; p < NP_MAX_PADS; p++)
            {
                withheld[p] |= pads[p].buttons;
                withheld_stick[p] = abs(pads[p].lx) > 8000 || abs(pads[p].ly) > 8000;
                withheld_touch[p] = pads[p].touching;
            }
        }
        for (int p = 0; p < NP_MAX_PADS; p++)
        {
            withheld[p] &= pads[p].buttons;
            pads[p].buttons &= ~withheld[p];
            withheld_stick[p] =
                withheld_stick[p] && (abs(pads[p].lx) > 8000 || abs(pads[p].ly) > 8000);
            withheld_touch[p] = withheld_touch[p] && pads[p].touching;
            if (withheld_stick[p])
                pads[p].lx = pads[p].ly = 0;
            if (withheld_touch[p])
                pads[p].touching = false;
        }
        memcpy(app.pads, pads, sizeof(app.pads));
        const PadState *nav = &pads[app_nav_pad()];
        /* Hide before handling dialogs, game input or drawing any game or library overlays. */
        if (!nav->connected)
            privacy_armed = privacy_resuming = false;
        else if (!nav->buttons && nav->l2 < 40 && nav->r2 < 40 &&
                 abs(nav->lx) < 8000 && abs(nav->ly) < 8000 && abs(nav->rx) < 8000 &&
                 abs(nav->ry) < 8000 && !nav->touching)
        {
            if (privacy_resuming)
            {
                reset_transient();
                plat_audio_privacy(false);
                app.privacy_active = privacy_resuming = false;
            }
            privacy_armed = true;
        }
        else if (privacy_armed && (nav->buttons & BIT(BTN_R3)) && app.settings.privacy_shortcut)
        {
            privacy_armed = false;
            if (!app.privacy_active)
            {
                app.privacy_active = true;
                plat_audio_privacy(true);
                reset_transient();
            }
            else if (nav->buttons == BIT(BTN_R3))
                privacy_resuming = true;
        }
        if (app.privacy_active)
        {
            previously_active = false;
            plat_begin_frame(0xff101014u);
            plat_fill_rect(0, 0, plat_width(), plat_height(), 0xff101014u);
            text_draw(plat_width() / 2, plat_height() / 2, 30, FONT_REGULAR, 0xffdddddfu,
                      ALIGN_CENTER, tr("Paused"));
            text_draw(plat_width() / 2, plat_height() / 2 + 48, 22, FONT_REGULAR, 0xffa6a6aau,
                      ALIGN_CENTER, tr("Press R3 to resume"));
            plat_end_frame();
            last_frame_start = now;
            continue;
        }
        uint32_t pressed = nav_pressed(pads);
#if defined(NP_PREVIEW)
        preview_surface_input(&pressed);
#endif
        if (!message_box_input(&pressed))
            update_popup_input(&pressed);
        const bool backdrop = app.screen == SCREEN_LIBRARY || (app.screen == SCREEN_SETTINGS && app.game < 0);
        plat_begin_frame(backdrop ? 0 : 0xff000000u);
        static const char *const names[SCREEN_COUNT] = {"library",  "game",    "menu",
                                                        "settings", "keys",    "sounds",
                                                        "details",  "profile", "backups"};
        ps5_crash_step(names[app.screen]);
        uint32_t transition_pressed = pressed;
        if (pending_start >= 0 || stop_pending || host_state() == HOST_LOADING ||
            host_state() == HOST_STOPPING || app_save_lifecycle_busy())
            pressed = 0;
        switch (app.screen)
        {
        case SCREEN_LIBRARY: shelf_screen(pressed); break;
        case SCREEN_GAME: game_screen(pads, pressed); break;
        case SCREEN_MENU: menu_screen(pressed); break;
        case SCREEN_SETTINGS: settings_screen(pressed); break;
        case SCREEN_KEYS: keys_screen(pressed); break;
        case SCREEN_SOUNDS: sounds_screen(pressed); break;
        case SCREEN_DETAILS:
            panels_screen(pressed);
            break;
        case SCREEN_PROFILE:
            preferences_screen(pressed);
            break;
        case SCREEN_BACKUPS:
            backup_panel_screen(pressed);
            break;
        default: break;
        }
        if (app.game >= 0) covers_update(app.game);
        else {
            int visible[21], count = 0;
            int row = library_view_selection(&app.library_view, &app.library, &app.settings);
            for (int distance = 0; distance <= 10 && row >= 0; ++distance)
                for (int side = -1; side <= 1; side += 2) {
                    int at = row + distance * side;
                    if ((!distance && side > 0) || at < 0 || at >= app.library_view.count) continue;
                    if (count < 21) visible[count++] = app.library_view.indices[at];
                }
            covers_update_visible(visible, count);
        }
        transition_draw(transition_pressed);
        update_popup_draw();
        message_box_draw();
        toast_render();
#if defined(NP_PREVIEW)
        preview_surface_draw();
#endif
        watermark_draw();
        const uint64_t drawn = plat_ticks_us();
        plat_end_frame();
#if defined(NP_PREVIEW)
        uint64_t wall_presented = SDL_GetPerformanceCounter();
#endif
        if (host_owns_runtime())
            host_note_presentation(plat_ticks_us() - drawn);
        if (app.screen == SCREEN_GAME && app.game >= 0)
            note_game_frame(now - last_frame_start, drawn - now, plat_ticks_us() - drawn);
        last_frame_start = now;
        if (pending_start >= 0)
        {
            int start = pending_start;
            pending_start = -1;
            start_game_now(start);
        }
        if (pending_stop)
        {
            pending_stop = false;
            host_request_stop();
        }
#if defined(NP_PREVIEW)
        static double total_wall, input_wall, max_wall;
        static unsigned wall_frames;
        double full =
            (SDL_GetPerformanceCounter() - wall_start) * 1000.0 / SDL_GetPerformanceFrequency();
        if (metrics)
        {
            CoverStats cover = covers_stats();
            fprintf(metrics, "%u,%d,%d,%d,%u,%.6f,%.6f,%.6f,%zu,%zu,%zu\n",
                    metric_frame++, app.screen, app.library.count, app.settings.last_game,
                    transition_pressed, full,
                    (wall_presented - wall_start) * 1000.0 / SDL_GetPerformanceFrequency(),
                    publish_ms, cover.decoded_bytes, cover.texture_bytes, cover.last_upload_bytes);
        }
        total_wall += full;
        if (full > max_wall)
            max_wall = full;
        input_wall += (wall_presented - wall_start) * 1000.0 / SDL_GetPerformanceFrequency();
        if (++wall_frames == 120)
        {
            np_log("preview real main: screen=%d still=%d frame_ms=%.3f max_ms=%.3f "
                   "input_to_present_ms=%.3f (120 frames)",
                   app.screen, app.settings.still_background, total_wall / 120, max_wall,
                   input_wall / 120);
            wall_frames = 0;
            total_wall = input_wall = max_wall = 0;
        }
#endif
    }

#if defined(NP_PREVIEW)
    if (metrics)
        fclose(metrics);
#endif
    /* Only the desktop build gets here; the PS5 shell closes the title. */
    ps5_crash_step("shutdown");
    np_log("shutdown: leaving the main loop");
    if (app.game >= 0)
        app_stop_game();
    while (host_state() == HOST_STOPPING || saves_busy() || pre_backup || post_backup ||
           (stop_pending && host_owns_runtime() && host_state() != HOST_FAILURE))
    {
        if (pending_stop)
        {
            pending_stop = false;
            host_request_stop();
        }
        runtime_tick();
        plat_sleep_us(10000);
    }
    app_save_settings();
#if !defined(__PROSPERO__)
    if (host_owns_runtime())
    {
        fflush(NULL);
        _Exit(1);
    } /* never free live engine resources */
    library_scanner_destroy(app.scanner); app.scanner = NULL;
    covers_stop();
    plat_audio_close();
    icons_shutdown();
    text_shutdown();
    library_view_free(&app.library_view);
    user_metadata_free(&app.metadata);
    library_free(&app.library);
    plat_shutdown();
#else
    /* The system closes the title: exit()/_exit() raise SIGSYS on the console. LoadExec "exit" ends it normally. */
    extern int sceSystemServiceLoadExec(const char *path, const char **arguments);
    if (!host_owns_runtime())
    {
        icons_shutdown();
        text_shutdown();
        plat_release();
    }
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
