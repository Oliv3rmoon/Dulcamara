/*
 * Dulcamara - adding a game from a link (src/links.c, src/link_fetch.c, src/link_unpack.c) on the host, driven by
 * tests/test_game_links.py against its server on 127.0.0.1:
 *
 *   game-links-check add <games> <url> [option...]       link_add, as the queue runs it
 *   game-links-check unpack <games> <zip> <hint> [option...]  link_unpack alone (the gate held as link_add holds it)
 *   game-links-check cleanup <games>                     links_cleanup, as the app's start runs it
 *   game-links-check scan <games> [folder]               the library's scan; with folder, what the queue's caller
 *                                                        finds for it (links_find_game)
 *   game-links-check pending <links.txt> <done>          links_pending
 *   game-links-check mark <done> <url> <outcome>         links_mark
 *   game-links-check queue <games> <done> <url>...       links_start, polled until it finishes
 *
 * Options: stop=<stage>:<call> raises SIGSTOP at that progress call (the parent kills it there), cancel=<stage>:<call>
 * cancels from then on, fill=<stage>:<call>:<file> fills the volume with <file> there, busy runs a game first
 * (maintenance_launch), so the gate is held by the runtime.
 * Prints "result <LinkError> folder <folder> progress <done>/<total> calls <n> message <text>"; exits 0 on LINK_OK.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "library.h"
#include "links.h"
#include "maintenance.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    int stop_stage, stop_call;
    int cancel_stage, cancel_call;
    int fill_stage, fill_call;
    char fill[1024];
    int calls[8];
    bool cancelled;
    unsigned long long done, total;
    int all_calls;
} Test;

static bool on_cancelled(void *context)
{
    return ((Test *)context)->cancelled;
}

static void fill_volume(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    static char zeros[1 << 16] = {1}; /* not zeros: nothing sparse */
    while (write(fd, zeros, sizeof(zeros)) > 0)
        ;
    for (int i = 0; i < 64; ++i) /* the last scraps a smaller write still finds */
        if (write(fd, zeros, 512) <= 0)
            break;
    fsync(fd);
    close(fd);
}

static void on_progress(void *context, LinkStage stage, uint64_t done, uint64_t total)
{
    Test *t = context;
    const int call = ++t->calls[stage & 7];
    ++t->all_calls;
    if ((int)stage == LINK_STAGE_DOWNLOAD) /* the download's bytes: what the tests compare */
    {
        t->done = done;
        t->total = total;
    }
    if ((int)stage == t->fill_stage && call == t->fill_call)
        fill_volume(t->fill);
    if ((int)stage == t->cancel_stage && call >= t->cancel_call)
        t->cancelled = true;
    if ((int)stage == t->stop_stage && call == t->stop_call)
    {
        printf("stopped %d %d\n", (int)stage, call);
        fflush(stdout);
        raise(SIGSTOP);
    }
}

static void options(Test *t, int argc, char **argv, int from, bool *busy)
{
    for (int i = from; i < argc; ++i)
    {
        if (!strncmp(argv[i], "stop=", 5))
            sscanf(argv[i] + 5, "%d:%d", &t->stop_stage, &t->stop_call);
        else if (!strncmp(argv[i], "cancel=", 7))
            sscanf(argv[i] + 7, "%d:%d", &t->cancel_stage, &t->cancel_call);
        else if (!strncmp(argv[i], "fill=", 5))
        {
            int n = 0;
            sscanf(argv[i] + 5, "%d:%d:%n", &t->fill_stage, &t->fill_call, &n);
            snprintf(t->fill, sizeof(t->fill), "%s", argv[i] + 5 + n);
        }
        else if (!strcmp(argv[i], "busy"))
            *busy = true;
    }
}

static int report(bool ok, const LinkResult *r, const Test *t)
{
    printf("result %d folder %s progress %llu/%llu calls %d message %s\n", (int)r->error, r->folder[0] ? r->folder : "-",
           t->done, t->total, t->all_calls, r->message);
    return ok ? 0 : 1;
}

static void ready(bool busy)
{
    /* startup recovery done, as main.c's start sees it once the save worker reports */
    maintenance_recovery_begin(1);
    maintenance_recovery_finish(1, true);
    if (busy && !maintenance_launch())
    {
        puts("could not hold the runtime");
        exit(3);
    }
}

static int scan(const char *games, const char *folder)
{
    Library lib = {0};
    char error[NP_PATH_MAX + 96];
    if (!library_scan_checked(&lib, games, NULL, NULL, error, sizeof(error)))
    {
        printf("scan failed: %s\n", error);
        return 1;
    }
    for (int i = 0; i < lib.count; ++i)
        printf("game %s\t%s\t%d\n", lib.games[i].location, lib.games[i].title, lib.games[i].core);
    for (int i = 0; i < lib.skipped_count; ++i)
        printf("skip %s\t%s\n", lib.skipped[i].path, lib.skipped[i].reason);
    if (folder)
    {
        char reason[160];
        const int at = links_find_game(&lib, folder, reason, sizeof(reason));
        if (at >= 0)
            printf("found %s\n", lib.games[at].title);
        else
            printf("not shown %s\n", reason);
    }
    library_free(&lib);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 3)
    {
        fprintf(stderr, "usage: see the comment at the top\n");
        return 2;
    }
    Test t = {0};
    LinkObserver observer = {on_cancelled, on_progress, &t};
    LinkResult r;
    memset(&r, 0, sizeof(r));
    bool busy = false;
    const char *command = argv[1];
    if (!strcmp(command, "add") && argc >= 4)
    {
        options(&t, argc, argv, 4, &busy);
        ready(busy);
        return report(link_add(argv[2], argv[3], &observer, &r), &r, &t);
    }
    if (!strcmp(command, "shelf") && argc >= 4) {
        options(&t, argc, argv, 4, &busy);
        ready(busy);
        return report(links_install_shelf(argv[2], argv[3], &r), &r, &t);
    }
    if (!strcmp(command, "unpack") && argc >= 5)
    {
        options(&t, argc, argv, 5, &busy);
        ready(busy);
        if (!maintenance_acquire(MAINT_LINKS))
        {
            puts("gate refused");
            return 3;
        }
        char staging[NP_PATH_MAX];
        snprintf(staging, sizeof(staging), "%s/" LINKS_STAGING, argv[2]);
        mkdir(argv[2], 0755);
        mkdir(staging, 0755);
        const bool ok = link_unpack(argv[2], staging, argv[3], "test", argv[4], &observer, &r);
        maintenance_release(MAINT_LINKS, false);
        return report(ok, &r, &t);
    }
    if (!strcmp(command, "cleanup"))
    {
        const bool ok = links_cleanup(argv[2]);
        printf("cleanup %s\n", ok ? "ok" : "failed");
        return ok ? 0 : 1;
    }
    if (!strcmp(command, "scan"))
        return scan(argv[2], argc > 3 ? argv[3] : NULL);
    if (!strcmp(command, "pending") && argc >= 4)
    {
        static char urls[LINKS_PER_RUN][LINKS_URL_MAX];
        char why[256] = "";
        int invalid = 0;
        const int n = links_pending(argv[2], argv[3], urls, LINKS_PER_RUN, &invalid, why, sizeof(why));
        printf("pending %d invalid %d why %s\n", n, invalid, why);
        for (int i = 0; i < n; ++i)
            printf("url %s\n", urls[i]);
        return n >= 0 ? 0 : 1;
    }
    if (!strcmp(command, "mark") && argc >= 5)
        return links_mark(argv[2], argv[3], argv[4]) ? 0 : 1;
    if (!strcmp(command, "queue") && argc >= 5)
    {
        ready(false);
        static char urls[LINKS_PER_RUN][LINKS_URL_MAX];
        int n = 0;
        for (int i = 4; i < argc && n < LINKS_PER_RUN; ++i)
            snprintf(urls[n++], LINKS_URL_MAX, "%s", argv[i]);
        if (!links_start(argv[2], argv[3], (const char(*)[LINKS_URL_MAX])urls, n))
        {
            puts("could not start");
            return 1;
        }
        if (links_start(argv[2], argv[3], (const char(*)[LINKS_URL_MAX])urls, n))
        {
            puts("a second queue started beside the first");
            return 1;
        }
        LinksStatus s;
        const struct timespec pause = {0, 20 * 1000 * 1000};
        for (;;)
        {
            links_poll(&s);
            if (s.state == LINKS_FINISHED || s.state == LINKS_CANCELLED)
                break;
            nanosleep(&pause, NULL);
        }
        printf("queue state %d added %d failed %d last %s\n", (int)s.state, s.added, s.failed,
               s.last_folder[0] ? s.last_folder : "-");
        for (int i = 0; i < s.count && i < LINKS_RESULTS_MAX; ++i)
            printf("link %d result %d folder %s message %s\n", i, (int)s.results[i].error,
                   s.results[i].folder[0] ? s.results[i].folder : "-", s.results[i].message);
        links_acknowledge();
        links_poll(&s);
        printf("after %d\n", (int)s.state);
        return 0;
    }
    fprintf(stderr, "unknown command %s\n", command);
    return 2;
}
