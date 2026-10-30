/*
 * Dulcamara - Ren'Py's core: the OpenGL context Ren'Py draws with, made by the core instead of SDL (on the PS5 and
 * Linux through EGL, gl_egl.c; on macOS through CGL, gl_cgl.c), and a framebuffer object of the window's size bound
 * as Ren'Py starts, which Ren'Py's gl2 then takes as the screen's (gl2draw.pyx, init_fbo). Each shown frame is read
 * into a pixel-pack buffer and copied out one frame later, when the read has finished, so the GPU is never waited
 * on; the last frame before Ren'Py waits is copied out when it waits (gl_flush_pending).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_GL_H
#define DR_GL_H

#include <stdbool.h>
#include <stddef.h>

/* A context current on this thread, with the framebuffer bound. False with a reason in error. */
bool gl_create(int w, int h, int depth_bits, char *error, size_t size);
void *gl_proc(const char *name); /* SDL_GL_GetProcAddress */
bool gl_make_current(bool on);
void gl_resize(int w, int h); /* the window's new size: the framebuffer's storage follows, its name stays */
void gl_present(void);        /* SDL_GL_SwapWindow: read this frame, publish the one before */
void gl_flush_pending(void);  /* publish the frame still being read */
void gl_destroy(void);
const char *gl_renderer(void); /* "<GL_RENDERER>, OpenGL <GL_VERSION>" for the log */

#endif
