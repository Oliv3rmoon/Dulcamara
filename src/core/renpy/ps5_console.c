/*
 * Dulcamara - Ren'Py's cores on the PS5: what the core's libraries print (ps5-opengl's reports on its output and
 * its draws, Python's and FFmpeg's errors) goes to dulcamara.log as well as to the console's stdout and stderr,
 * which no tester sees. Each core's relocatable link sends printf, puts, fprintf, fputs and fwrite here (--wrap,
 * tools/renpy/ps5.sh and tools/renpy7/ps5.sh); writes to any other file go straight on. The first 300 lines are
 * logged, so a library that prints each frame cannot fill the log, and each is written at once: ps5-opengl prints
 * why just before it ends the app (_Exit) when the GPU stops answering, and a line held for the next frame is lost.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include "log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOGGED_LINES 300

int __real_puts(const char *text);
int __real_fputs(const char *restrict text, FILE *restrict stream);
size_t __real_fwrite(const void *restrict data, size_t size, size_t count, FILE *restrict stream);

static atomic_int logged;

/* each line of text (the last one even without its newline), as "renpy: console: ..." */
static void log_text(const char *text, size_t length)
{
    while (length)
    {
        const char *end = memchr(text, '\n', length);
        const size_t line = end ? (size_t)(end - text) : length;
        if (line)
        {
            const int n = atomic_fetch_add(&logged, 1);
            if (n < LOGGED_LINES)
                log_now(1, "renpy: console: %.*s", (int)(line < 900 ? line : 900), text);
            else if (n == LOGGED_LINES)
                log_now(1, "renpy: console: (more is not logged)");
        }
        const size_t taken = end ? line + 1 : line;
        text += taken;
        length -= taken;
    }
}

static bool console(FILE *stream)
{
    return stream == stdout || stream == stderr;
}

static void log_formatted(const char *format, va_list args)
{
    char text[1024];
    const int n = vsnprintf(text, sizeof(text), format, args);
    if (n > 0)
        log_text(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
}

int __wrap_printf(const char *restrict format, ...)
{
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    log_formatted(format, copy);
    va_end(copy);
    const int n = vprintf(format, args);
    va_end(args);
    return n;
}

int __wrap_fprintf(FILE *restrict stream, const char *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    if (console(stream))
    {
        va_list copy;
        va_copy(copy, args);
        log_formatted(format, copy);
        va_end(copy);
    }
    const int n = vfprintf(stream, format, args);
    va_end(args);
    return n;
}

int __wrap_puts(const char *text)
{
    log_text(text, strlen(text));
    return __real_puts(text);
}

int __wrap_fputs(const char *restrict text, FILE *restrict stream)
{
    if (console(stream))
        log_text(text, strlen(text));
    return __real_fputs(text, stream);
}

size_t __wrap_fwrite(const void *restrict data, size_t size, size_t count, FILE *restrict stream)
{
    if (console(stream) && size && count <= 4096 / size)
        log_text((const char *)data, size * count);
    return __real_fwrite(data, size, count, stream);
}

#endif
