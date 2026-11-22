/*
 * Dulcamara - adding games from links in the interface (links_ui.h): starts links.c's queue from links.txt or a typed
 * link, says how it goes on Settings > Library, and when it ends looks at the library again and selects the new game,
 * or says why the library does not show it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "links_ui.h"

#include "../app.h"
#include "../core/host.h"
#include "../i18n.h"
#include "../links.h"
#include "../maintenance.h"
#include "../platform/platform.h"

#include <stdio.h>
#include <string.h>

static bool selecting;                    /* waiting for the scan that lists the new game */
static uint64_t select_generation;        /* that scan's request */
static char select_folder[NP_PATH_MAX];   /* the game installed last */

static void rescan_and_select(const char *folder)
{
    str_copy(select_folder, sizeof(select_folder), folder);
    app_rescan();
    select_generation = app.scan_status.generation;
    selecting = true;
}

static bool can_start(void)
{
    if (app.game >= 0 || host_owns_runtime())
    {
        app_message_box("Close the game first: nothing is downloaded while a game runs.");
        return false;
    }
    if (links_ui_busy())
    {
        app_toast("Links are already being added.");
        return false;
    }
    if (!maintenance_ready())
    {
        app_message_box("Wait for save recovery, a backup, a restore or an update to finish, then try again.");
        return false;
    }
    return true;
}

void links_ui_install_shelf(void)
{
    if (!can_start())
        return;
    char asset[NP_PATH_MAX];
    plat_asset_path(asset, sizeof(asset), "itch-shelf.zip");
    LinkResult result;
    if (links_install_shelf(app.paths.games, asset, &result))
        rescan_and_select(result.folder);
    else
        app_message_box(result.message);
}

static void start(const char *done, const char (*urls)[LINKS_URL_MAX], int count)
{
    if (!links_start(app.paths.games, done, urls, count))
    {
        app_toast("Couldn't start adding the games.");
        return;
    }
    selecting = false;
    np_log("links: adding %d link(s)%s", count, done ? " from links.txt" : "");
}

void links_ui_from_file(void)
{
    if (!can_start())
        return;
    char file[NP_PATH_MAX], done[NP_PATH_MAX], why[128], text[600];
    path_join(file, sizeof(file), app.paths.root, LINKS_FILE);
    path_join(done, sizeof(done), app.paths.root, LINKS_DONE_FILE);
    static char urls[LINKS_PER_RUN][LINKS_URL_MAX];
    int invalid = 0;
    const int count = links_pending(file, done, urls, LINKS_PER_RUN, &invalid, why, sizeof(why));
    if (count < 0)
    {
        snprintf(text, sizeof(text),
                 tr("%s. Put one direct link to a game's .zip a line (starting with https://) in " NP_DATA
                    "/links.txt, over FTP, then choose this again."),
                 why);
        app_message_box(text);
        return;
    }
    if (count == 0)
    {
        if (invalid)
            snprintf(text, sizeof(text),
                     tr("No new links in links.txt. %d line(s) are not links starting with https:// or http://."),
                     invalid);
        else
            snprintf(text, sizeof(text),
                     tr("No new links in links.txt: each one is already in links-done.txt. Delete a line there to "
                        "try that link again."));
        app_message_box(text);
        return;
    }
    start(done, (const char(*)[LINKS_URL_MAX])urls, count);
}

void links_ui_from_url(const char *url)
{
    if (!link_url_valid(url))
    {
        app_message_box("That isn't a link Dulcamara can download: it starts with https:// or http://.");
        return;
    }
    if (!can_start())
        return;
    static char one[1][LINKS_URL_MAX];
    str_copy(one[0], sizeof(one[0]), url);
    start(NULL, (const char(*)[LINKS_URL_MAX])one, 1);
}

bool links_ui_busy(void)
{
    LinksStatus s;
    links_poll(&s);
    return s.state == LINKS_RUNNING;
}

void links_ui_cancel(void)
{
    links_cancel();
    app_toast("Cancelling...");
}

const char *links_ui_status(void)
{
    static char text[160];
    static LinksStatus s;
    links_poll(&s);
    if (s.state != LINKS_RUNNING)
        return selecting ? tr("Added: looking for it in the library") : "";
    char of[24] = "";
    if (s.count > 1)
        snprintf(of, sizeof(of), "%d/%d: ", s.index + 1, s.count);
    switch (s.stage)
    {
    case LINK_STAGE_DOWNLOAD:
        if (s.total)
            snprintf(text, sizeof(text), tr("%sDownloading %.1f of %.1f MB (Cross cancels)"), of,
                     (double)s.done / 1048576.0, (double)s.total / 1048576.0);
        else
            snprintf(text, sizeof(text), tr("%sDownloading %.1f MB (Cross cancels)"), of, (double)s.done / 1048576.0);
        break;
    case LINK_STAGE_CHECK: snprintf(text, sizeof(text), tr("%sChecking the ZIP (Cross cancels)"), of); break;
    case LINK_STAGE_UNPACK:
        snprintf(text, sizeof(text), tr("%sUnpacking %d%% (Cross cancels)"), of,
                 s.total ? (int)(s.done * 100 / s.total) : 0);
        break;
    case LINK_STAGE_INSTALL: snprintf(text, sizeof(text), tr("%sAdding it to the games"), of); break;
    }
    return text;
}

/* the end of a queue: what happened, and the library looked at again */
static void finished(const LinksStatus *s)
{
    char text[600];
    size_t n = 0;
    if (s->state == LINKS_CANCELLED)
        n += (size_t)snprintf(text, sizeof(text), tr("Cancelled: nothing was kept from the link being added."));
    else if (s->failed == 0)
    {
        if (s->added == 1)
            app_toast(tr("Game added."));
        else
        {
            snprintf(text, sizeof(text), tr("%d games added."), s->added);
            app_toast(text);
        }
    }
    else if (s->count == 1)
        n += (size_t)snprintf(text, sizeof(text), "%s", s->results[0].message);
    else
    {
        n += (size_t)snprintf(text, sizeof(text), tr("%d added, %d not added:"), s->added, s->failed);
        for (int i = 0; i < s->count && i < LINKS_RESULTS_MAX && n < sizeof(text) - 1; ++i)
            if (s->results[i].error != LINK_OK && s->results[i].error != LINK_CANCELLED)
                n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%d: %.150s", i + 1, s->results[i].message);
    }
    if (n)
        app_message_box(text);
    if (s->added > 0)
        rescan_and_select(s->last_folder);
}

void links_ui_tick(void)
{
    LinksStatus s;
    links_poll(&s);
    if (s.state == LINKS_FINISHED || s.state == LINKS_CANCELLED)
    {
        links_acknowledge();
        finished(&s);
    }
    if (!selecting)
        return;
    const LibraryScanStatus *scan = &app.scan_status;
    if (scan->generation != select_generation && scan->state == LIBRARY_SCAN_RUNNING)
        select_generation = scan->generation; /* a newer look (Square): it finds the game too */
    if (scan->generation != select_generation || scan->state == LIBRARY_SCAN_RUNNING)
        return;
    if (scan->state != LIBRARY_SCAN_DONE)
    {
        selecting = false; /* the scan failed or was cancelled: it said why */
        return;
    }
    /* library_tick published the finished scan this frame only on the shelf, with no game */
    if (app.screen != SCREEN_LIBRARY || app.game >= 0 || host_owns_runtime())
        return;
    selecting = false;
    char reason[160], text[600];
    const int at = links_find_game(&app.library, select_folder, reason, sizeof(reason));
    if (at >= 0)
    {
        library_select(&app.library, &app.settings, at);
        app_save_settings();
        snprintf(text, sizeof(text), tr("Added: %s"), app.library.games[at].title);
        app_toast(text);
        return;
    }
    const char *name = strrchr(select_folder, '/');
    snprintf(text, sizeof(text),
             tr("\"%s\" was added to the games folder, but the library doesn't show it: %s. Delete it over FTP if "
                "you don't want it."),
             name ? name + 1 : select_folder, reason);
    app_message_box(text);
}
