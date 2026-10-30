/*
 * Dulcamara - Ren'Py's core: log lines from any thread (Ren'Py's, its helpers'), held until retro_run hands them to
 * the frontend's log on the frontend's thread.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_LOG_H
#define DR_LOG_H

/* level 1: information, 2: a warning or an error */
void log_queue(int level, const char *fmt, ...);
/* The frontend: every held line, oldest first. */
void log_flush(void (*emit)(int level, const char *line));

#endif
