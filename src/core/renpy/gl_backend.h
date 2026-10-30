/*
 * Dulcamara - Ren'Py's core: what gl.c needs from EGL or CGL.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_GL_BACKEND_H
#define DR_GL_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

/* A desktop OpenGL context (2.0 or later, compatibility), current on this thread, with no window. */
bool backend_create(char *error, size_t size);
void *backend_proc(const char *name);
bool backend_make_current(bool on);
void backend_destroy(void);

#endif
