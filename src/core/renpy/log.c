/*
 * Dulcamara - Ren'Py's core: the held log lines (log.h). When more arrive than it holds between two frames (a long
 * traceback while the frontend is busy), the oldest are dropped and the drop is logged.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#define LINES 512
#define LINE_SIZE 1024

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char lines[LINES][LINE_SIZE];
static int levels[LINES];
static unsigned first, count, dropped;

void log_queue(int level, const char *fmt, ...)
{
    char line[LINE_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    pthread_mutex_lock(&lock);
    if (count == LINES)
    {
        first = (first + 1) % LINES;
        --count;
        ++dropped;
    }
    const unsigned at = (first + count) % LINES;
    snprintf(lines[at], LINE_SIZE, "%s", line);
    levels[at] = level;
    ++count;
    pthread_mutex_unlock(&lock);
}

void log_flush(void (*emit)(int level, const char *line))
{
    char line[LINE_SIZE];
    for (;;)
    {
        pthread_mutex_lock(&lock);
        if (dropped)
        {
            snprintf(line, sizeof(line), "(%u log lines dropped)", dropped);
            dropped = 0;
            pthread_mutex_unlock(&lock);
            emit(2, line);
            continue;
        }
        if (!count)
        {
            pthread_mutex_unlock(&lock);
            return;
        }
        snprintf(line, sizeof(line), "%s", lines[first]);
        const int level = levels[first];
        first = (first + 1) % LINES;
        --count;
        pthread_mutex_unlock(&lock);
        emit(level, line);
    }
}
