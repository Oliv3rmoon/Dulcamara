/* Nonblocking save job consumer: never substitutes for a lifecycle-owned job. GPL-3.0-or-later */
#include "panels.h"
#include "controller.h"
#include "eclipse.h"
#include "draw.h"
#include "story.h"
#include "../profile.h"
#include "../app.h"
#include "../saves.h"
#include "../core/host.h"
#include "../core/cores.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static Game game;
static UiList list;
static SavesStatus status;
static uint64_t job, handled_job;
static size_t offset;
static bool confirmation, deleting, detail_pending, confirm_armed;
static int root_row, root_column;
static enum Screen back_to = SCREEN_DETAILS; /* where Circle returns: the details panel, or Story's library */
static bool automatic_on;                    /* this game's effective automatic backups, for Story's words */
/* Story view: how long the restore/delete question has been on screen. A Cross in its first moments is the
 * visual-novel habit of pressing to see the text, not an answer, so it does nothing there. */
#define STORY_QUESTION_SETTLE 0.5f
static float question_age;
bool backup_panel_confirming(void)
{
    return confirmation;
}
static char error[256], selected[SAVE_ID_MAX];
static SaveRequest request;
static bool available(void)
{
    return ui_save_allowed(host_owns_runtime(), app_save_lifecycle_busy(), maintenance_ready());
}
static bool start(SavesOperation op, const char *snapshot, bool confirmed)
{
    if (app_save_lifecycle_busy() || saves_busy())
    {
        snprintf(error, sizeof(error), "Wait for the current save operation to finish.");
        return false;
    }
    if (op != SAVES_LIST && op != SAVES_DETAILS && !available())
    {
        snprintf(error, sizeof(error),
                 "Stop the game safely and finish recovery before modifying backups.");
        return false;
    }
    if (!saves_start(op, &request, snapshot, confirmed))
    {
        snprintf(error, sizeof(error),
                 "Could not start save operation. Finish maintenance, then try again.");
        return false;
    }
    saves_poll(&status);
    job = status.serial;
    error[0] = 0;
    return true;
}
static void refresh(void)
{
    if (app_save_lifecycle_busy() || saves_busy())
    {
        snprintf(error, sizeof(error),
                 "Save protection is busy. Press Triangle to refresh when it finishes.");
        return;
    }
    if (saves_start_page(game.id, offset))
    {
        saves_poll(&status);
        job = status.serial;
        error[0] = 0;
    }
}
void backup_panel_open(int i)
{
    backup_panel_open_from(i, SCREEN_DETAILS);
}
void backup_panel_open_from(int i, enum Screen back)
{
    back_to = back;
    game = app.library.games[i];
    {
        char name[40], path[NP_PATH_MAX];
        GameProfile profile;
        Settings effective;
        snprintf(name, sizeof(name), "%s.ini", game.id);
        path_join(path, sizeof(path), app.paths.profiles, name);
        profile_load(&profile, path);
        profile_resolve(&app.settings, &profile, &effective);
        automatic_on = effective.automatic_backups;
    }
    request = (SaveRequest){0};
    str_copy(request.game_id, sizeof(request.game_id), game.id);
    request.core = game.core;
    request.kind = SAVE_MANUAL;
    list = (UiList){0};
    offset = 0;
    job = handled_job = 0;
    status = (SavesStatus){0};
    confirmation = detail_pending = false;
    error[0] = 0;
    app.screen = SCREEN_BACKUPS;
    refresh();
}
const char *backup_scope_kind(uint32_t kind)
{
    switch (kind)
    {
    case DR_SAVE_FILE:
        return "exact file";
    case DR_SAVE_RPG_SLOTS:
        return "RPG Maker save slots";
    case DR_SAVE_RENPY_SLOTS:
        return "Ren'Py native slots";
    case DR_SAVE_RENPY_FILE:
        return "nested Ren'Py slot";
    default:
        return "declared save directory";
    }
}
const char *backup_moment(const SaveInfo *s)
{
    if (s->kind == SAVE_MANUAL)
        return "Manual";
    if (s->kind == SAVE_RECOVERY)
        return "Recovery";
    return s->moment == SAVE_MOMENT_EARLY      ? "Auto / early"
           : s->moment == SAVE_MOMENT_POSTSTOP ? "Auto / clean stop"
                                               : "Auto / pre-play";
}
static bool listed_ok(void)
{
    return status.operation == SAVES_LIST && status.state == SAVES_DONE;
}
static int listed_count(void)
{
    if (status.operation != SAVES_LIST || status.state != SAVES_DONE ||
        status.offset >= status.count)
        return 0;
    size_t count = status.count - status.offset;
    return (int)(count > SAVE_LIST_MAX ? SAVE_LIST_MAX : count);
}
void backup_panel_screen(uint32_t p)
{
    SavesStatus observed;
    saves_poll(&observed);
    if (job && observed.serial == job)
    {
        /* A small worker may finish before start()/refresh() gets its first poll.
         * Consume the terminal serial once, independently of observed transitions. */
        bool terminal = observed.state == SAVES_DONE || observed.state == SAVES_ERROR ||
                        observed.state == SAVES_CANCELLED;
        bool finished = terminal && handled_job != job;
        status = observed;
        if (finished)
        {
            handled_job = job; /* mark before a completion handler can start another job */
            if (status.state == SAVES_ERROR || status.state == SAVES_CANCELLED)
            {
                str_copy(error, sizeof(error),
                         status.state == SAVES_ERROR ? status.result.error
                                                     : "Operation cancelled safely.");
                detail_pending = false;
            }
            else if (status.operation == SAVES_DETAILS && detail_pending)
            {
                detail_pending = false;
                confirmation = true;
                confirm_armed = false;
                question_age = 0;
                p = 0; /* A completed details read must be displayed before any confirmation. */
                app_clear_toasts();
                root_row = root_column = 0;
            }
            else if (status.operation == SAVES_DETAILS && !detail_pending && !confirmation)
                refresh();
            else if (status.operation == SAVES_LIST && status.state == SAVES_DONE)
                story_note_backups(game.id, &status);
            else if (status.operation == SAVES_CREATE || status.operation == SAVES_DELETE ||
                     status.operation == SAVES_RESTORE)
            {
                app_toast(status.operation == SAVES_RESTORE
                              ? "Restore complete; recovery copy retained"
                          : status.operation == SAVES_DELETE ? "Local snapshot deleted"
                                                             : "Manual backup created");
                refresh();
            }
        }
    }
    if (p & BIT(BTN_CIRCLE))
    {
        if (confirmation || detail_pending)
        {
            confirmation = detail_pending = false;
            refresh();
            return;
        }
        if (back_to == SCREEN_LIBRARY)
            shelf_open();
        else
            app.screen = SCREEN_DETAILS;
        return; /* active job remains owned by service; no blocking joins */
    }
    const bool story = story_view();
    if (story)
    {
        /* Story's slots run left to right: Left/Right move through them as Up/Down do */
        if (p & BIT(BTN_RIGHT))
            p |= BIT(BTN_DOWN);
        if (p & BIT(BTN_LEFT))
            p |= BIT(BTN_UP);
    }
    if (confirmation)
    {
        if (p & BIT(BTN_DOWN))
            root_row++;
        if (p & BIT(BTN_UP))
            root_row--;
        if (p & BIT(BTN_R1))
            root_column = (root_column + 1) % 11;
        if (p & BIT(BTN_L1))
            root_column = (root_column + 10) % 11;
        int count = (int)status.layout.count;
        if (root_row < 0)
            root_row = 0;
        if (root_row >= count)
            root_row = count ? count - 1 : 0;
        uint32_t held = app.pads[app_nav_pad()].buttons;
        if (!(held & (BIT(BTN_CROSS) | BIT(BTN_SQUARE))))
            confirm_armed = true;
        question_age += app.dt;
        if (story && question_age < STORY_QUESTION_SETTLE)
            p &= ~BIT(BTN_CROSS); /* the press that would skip the question's text: no answer yet */
        if (confirm_armed && (p & BIT(BTN_CROSS)))
        {
            if (start(deleting ? SAVES_DELETE : SAVES_RESTORE, selected, true))
            {
                confirmation = false;
                p = 0;
                goto job_view;
            }
        }
        if (story)
        {
            const StoryBackups view = {&game, -1, &status, listed_count(), list.row, list.top, available(), false,
                                       true, deleting, confirm_armed, root_row, root_column, error, automatic_on,
                                       listed_ok(), offset};
            story_draw_backups(&view);
            return;
        }
        char stamp[48], subtitle[160];
        ui_save_timestamp(stamp, sizeof(stamp), status.detail.created);
        snprintf(subtitle, sizeof(subtitle), "%s  |  %s  |  %llu bytes", stamp,
                 backup_moment(&status.detail), (unsigned long long)status.detail.bytes);
        panel_begin(deleting ? "Delete this local snapshot?" : "Restore native saves?", game.title);
        panel_note(310, subtitle);
        panel_note(355, deleting
                            ? "This deletes this snapshot. The game's current saves are unchanged."
                            : "A verified recovery copy of the current native saves is created "
                              "before replacement.");
        panel_note(399, "Shared save locations may affect other games. Other games can also use an "
                        "associated location.");
        panel_note(443,
                   core_is_renpy(game.core)
                       ? "Only listed native locations are covered; custom Python file writes are "
                         "outside this backup."
                       : "Only the listed native save locations are covered by this snapshot.");
        for (int i = root_row; i < count && i < root_row + 3; i++)
        {
            char label[100];
            snprintf(label, sizeof(label), "Save location %d / %d: %s, %s", i + 1, count,
                     status.layout.entries[i].scope == DR_SAVE_SHARED ? "SHARED"
                                                                      : "used by this game",
                     backup_scope_kind(status.layout.entries[i].kind));
            panel_note(498 + (i - root_row) * 80, label);
            int start = root_column * 100;
            const char *path = status.layout.entries[i].path;
            if (start >= (int)strlen(path))
                start = 0;
            while (start > 0 && ((unsigned char)path[start] & 0xc0) == 0x80)
                start--;
            panel_note(532 + (i - root_row) * 80, path + start);
        }
        panel_note(805, "Up/Down scroll locations. L1/R1 scroll long paths.");
        panel_note(
            850, error[0] ? error
                 : confirm_armed
                     ? "Cross confirms. Circle cancels without changing saves."
                     : "Release Cross/Square, then press Cross again to confirm. Circle cancels.");
        int g[] = {GLYPH_CROSS, GLYPH_CIRCLE};
        const char *l[] = {confirm_armed ? "Confirm" : "Release buttons", "Cancel"};
        app_draw_hints(g, l, 2, NULL);
        return;
    }
    if (p & BIT(BTN_TRIANGLE))
        refresh();
    if (p & BIT(BTN_L1))
    {
        offset = offset >= SAVE_LIST_MAX ? offset - SAVE_LIST_MAX : 0;
        list = (UiList){0};
        refresh();
    }
    if ((p & BIT(BTN_R1)) && offset + SAVE_LIST_MAX < status.count)
    {
        offset += SAVE_LIST_MAX;
        list = (UiList){0};
        refresh();
    }
    int count = listed_count();
    if (status.operation == SAVES_LIST && status.state == SAVES_DONE)
        ui_list_move(&list, (p & BIT(BTN_DOWN)) ? 1 : (p & BIT(BTN_UP)) ? -1 : 0, count + 1,
                     story ? STORY_SLOT_CELLS : 7);
    if (status.state == SAVES_WORKING)
        p &= BIT(BTN_START);
    if ((p & BIT(BTN_CROSS)) && list.row == 0)
        start(SAVES_CREATE, NULL, false);
    if (list.row > 0 && list.row <= count && (p & (BIT(BTN_CROSS) | BIT(BTN_SQUARE))))
    {
        str_copy(selected, sizeof(selected), status.list[list.row - 1].snapshot);
        deleting = (p & BIT(BTN_SQUARE)) != 0;
        if (!available())
            str_copy(error, sizeof(error),
                     "Stop the game safely and finish maintenance before restore or deletion.");
        else if (start(SAVES_DETAILS, selected, false))
            detail_pending = true;
    }
job_view:
    /* Confirm may jump here with a job already DONE/ERROR, not only WORKING.
     * Do not render stale list rows or read the skipped local initializer. */
    count = listed_count();
    if (story)
    {
        if (status.state == SAVES_WORKING && (p & BIT(BTN_START)))
            saves_cancel();
        const StoryBackups view = {&game, -1, &status, count, list.row, list.top, available(),
                                   status.state == SAVES_WORKING, false, deleting, false, root_row, root_column,
                                   error, automatic_on, listed_ok(), offset};
        story_draw_backups(&view);
        return;
    }
    panel_begin(
        game.title,
        "Local backups  |  Automatic: five successful snapshots; manual/recovery copies retained");
    if (status.state == SAVES_WORKING)
    {
        eclipse_status(240, 320, 56);
        char progress[180];
        snprintf(progress, sizeof(progress), "%s: %llu / %llu",
                 status.phase == MT_APPLYING   ? "Applying"
                 : status.phase == MT_ROLLBACK ? "Recovering originals"
                                               : "Preparing",
                 (unsigned long long)status.done, (unsigned long long)status.total);
        panel_note(390, progress);
        panel_note(
            450, "Circle returns while the worker completes. Options requests safe cancellation.");
        if (p & BIT(BTN_START))
            saves_cancel();
    }
    else
    {
        for (int row = list.top; row < count + 1 && row < list.top + 7; row++)
        {
            if (row == 0)
            {
                panel_row(row - list.top, "Create manual backup",
                          available() ? "Stopped native layout" : "Unavailable: stop safely first",
                          list.row == 0);
                continue;
            }
            SaveInfo *info = &status.list[row - 1];
            char label[120], v[160], stamp[48];
            ui_save_timestamp(stamp, sizeof(stamp), info->created);
            snprintf(label, sizeof(label), "%s  |  %s", stamp, backup_moment(info));
            snprintf(v, sizeof(v), "%llu bytes  |  %s", (unsigned long long)info->bytes,
                     info->shared ? "Shared scope" : "Used by this game");
            panel_row(row - list.top, label, v, list.row == row);
        }
        if (!count)
            panel_note(430, "No snapshots on this page. Play and stop safely to discover native "
                            "save locations.");
    }
    panel_note(800, error[0] ? error
                             : "Later-discovered native paths are protected at clean stop, not "
                               "retroactively before play.");
    char page[120];
    snprintf(page, sizeof(page), "%zu total  |  L1 / R1 pages  |  Triangle refresh",
             status.operation == SAVES_LIST ? status.count : 0);
    panel_note(850, page);
    int g[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_CIRCLE};
    const char *l[] = {"Create / restore", "Delete snapshot", "Back"};
    app_draw_hints(g, l, 3, NULL);
}
