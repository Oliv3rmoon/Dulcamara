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
/* A step of starting Ren'Py ("step: Python starting"), written to the frontend's log at once from whichever thread
 * (log_set_direct's emitter), not held: a crash before the next frame would lose a held line, and the log is to say
 * how far a start got. */
void log_set_direct(void (*emit)(int level, const char *line));
void log_step(const char *fmt, ...);
/* Any line written at once in the same way: what a library prints just before it ends the app (ps5_console.c). */
void log_now(int level, const char *fmt, ...);

#endif
