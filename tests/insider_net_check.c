/*
 * Dulcamara - the insider request over the network on the host (src/insider_check.c's insider_decide with src/net.c
 * and the host's libcurl), against the server tests/test_insider.py runs:
 *   insider-net-check <base> <code> <seconds>
 * asks <base>/v1/updates as the update check does (insider.c's request: net_request_within with the limit <seconds>)
 * and prints "next <github|offer> status <s> http <n> answer <a> seconds <t>": github is the update check going on to
 * GitHub's releases exactly as before.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "insider_check.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long seconds_limit;

/* insider.c's request */
static int post(void *user, const char *url, const char *json, char **body, size_t *length)
{
    (void)user;
    return net_request_within(url, json, "application/json", NP_NAME "/" NP_VERSION, seconds_limit, body, length);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <base> <code> <seconds>\n", argv[0]);
        return 2;
    }
    seconds_limit = atol(argv[3]);
    InsiderBuild build;
    InsiderResult result;
    const double started = now();
    const InsiderNext next = insider_decide(argv[1], true, argv[2], NP_VERSION, post, NULL, &build, &result);
    printf("next %s status %s http %d answer %s seconds %.1f version %s\n",
           next == INSIDER_NEXT_OFFER ? "offer" : "github", insider_status_text(result.status), result.http,
           result.answer[0] ? result.answer : "-", now() - started, build.version[0] ? build.version : "-");
    return 0;
}
