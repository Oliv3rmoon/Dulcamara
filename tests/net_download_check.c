/*
 * Dulcamara - net_download (src/net.c) on the host, against the server tests/test_net_download.py runs:
 *   net-download-check <url> <dest>
 * prints "result <NetResult> seconds <s> progress <done>/<total> calls <n> why <reason>" and exits 0 when the
 * download succeeded.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void np_log(const char *fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);
    fputs("log: ", stdout);
    vprintf(fmt, arguments);
    fputc('\n', stdout);
    va_end(arguments);
}

static unsigned long long last_done, last_total;
static int calls;

static void on_progress(void *user, uint64_t done, uint64_t total)
{
    (void)user;
    last_done = done;
    last_total = total;
    ++calls;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <url> <dest>\n", argv[0]);
        return 2;
    }
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    char why[128] = "unset";
    const NetResult result = net_download(argv[1], argv[2], on_progress, NULL, why, sizeof(why));
    clock_gettime(CLOCK_MONOTONIC, &end);
    const double seconds = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    printf("result %d seconds %.1f progress %llu/%llu calls %d why %s\n", (int)result, seconds, last_done, last_total,
           calls, why);
    return result == NET_OK ? 0 : 1;
}
