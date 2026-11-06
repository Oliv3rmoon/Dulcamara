/*
 * Dulcamara - the in-app update with an insider build, on the host: src/update.c itself (its check_thread and
 * install_thread, through SDL's threads), src/insider.c, net.c on the host's libcurl, built with INSIDER_URL the
 * address of the server tests/test_insider.py runs:
 *   update-insider-check <data folder> <installs>
 * stores the insider code as Settings does, checks for an update with Test builds on, then installs the update
 * up to <installs> times (a failed install is tried again, as "Try again" does), printing after each what update.c says
 * and what is left in the cache; with 0, turns Test builds off instead. The ZIP the server sends has no PPSA99010 folder, so an install whose download passed its
 * checks ends in "Couldn't write the new files" and nothing is written outside the data folder.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app.h"
#include "insider.h"
#include "platform/platform.h"
#include "update.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

App app;

uint64_t plat_ticks_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void plat_sleep_us(uint32_t us)
{
    struct timespec t = {(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L};
    nanosleep(&t, NULL);
}

static const char *state_name(int s)
{
    static const char *const names[] = {"idle", "checking", "none", "available", "installing", "installed", "failed"};
    return s >= 0 && s < (int)(sizeof(names) / sizeof(names[0])) ? names[s] : "?";
}

/* update_state() once it is no longer `while_state`, at most `seconds` from now */
static int wait_while(int while_state, int seconds)
{
    const uint64_t until = plat_ticks_us() + (uint64_t)seconds * 1000000u;
    int s;
    while ((s = update_state()) == while_state && plat_ticks_us() < until)
        plat_sleep_us(20000);
    return s;
}

static void print_cache(void)
{
    printf("cache");
    DIR *d = opendir(app.paths.cache);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            printf(" %s", e->d_name);
    if (d)
        closedir(d);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <data folder> <installs>\n", argv[0]);
        return 2;
    }
    str_copy(app.paths.root, sizeof(app.paths.root), argv[1]);
    path_join(app.paths.cache, sizeof(app.paths.cache), argv[1], "cache");
    path_join(app.paths.logs, sizeof(app.paths.logs), argv[1], "logs");
    make_dirs(app.paths.logs);
    char log[NP_PATH_MAX];
    path_join(log, sizeof(log), app.paths.logs, "dulcamara.log");
    np_log_open(log);
    app.game = -1;

    /* Settings > System: Test builds on, and the code typed on Dulcamara's keyboard */
    app.settings.update_test_builds = true;
    printf("enabled %d hint before %d\n", insider_enabled(), insider_hint());
    printf("code %d has %d hint %d\n", (int)insider_set_code("abcd-efgh"), insider_has_code(), insider_hint());

    update_check();
    const int checked = wait_while(UPDATE_CHECKING, 40);
    uint64_t done, total;
    printf("check %s version %s size %llu test_build %d\n", state_name(checked), update_version(),
           (unsigned long long)update_size(), update_test_build());
    if (checked != UPDATE_AVAILABLE)
        return 0;
    const int installs = atoi(argv[2]);
    if (installs == 0)
    {
        /* Test builds off: the insider build, a test build, is no longer offered */
        app.settings.update_test_builds = false;
        printf("after test builds off %s\n", state_name(update_state()));
    }
    for (int i = 1; i <= installs; ++i)
    {
        update_install();
        const int s = wait_while(UPDATE_INSTALLING, 60);
        update_progress(&done, &total);
        printf("install %d %s retry %d message %s\n", i, state_name(s), update_can_retry(), update_message());
        print_cache();
        if (s != UPDATE_FAILED || !update_can_retry())
            break;
    }
    np_log_open(log); /* closes it, kept as dulcamara.old.log */
    return 0;
}
