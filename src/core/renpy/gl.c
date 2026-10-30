/*
 * Dulcamara - Ren'Py's core: OpenGL (gl.h). GL's names are declared here, the few the core needs, so that no GL
 * header is needed; the functions come from the backend, as Ren'Py's do.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gl.h"

#include "frames.h"
#include "gl_backend.h"
#include "log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLubyte, GLboolean;
typedef intptr_t GLsizeiptr, GLintptr;

#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_STREAM_READ 0x88E1
#define GL_READ_ONLY 0x88B8
#define GL_MAP_READ_BIT 0x0001
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static struct
{
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (*GenRenderbuffers)(GLsizei, GLuint *);
    void (*DeleteRenderbuffers)(GLsizei, const GLuint *);
    void (*BindRenderbuffer)(GLenum, GLuint);
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*GenBuffers)(GLsizei, GLuint *);
    void (*DeleteBuffers)(GLsizei, const GLuint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void *(*MapBuffer)(GLenum, GLenum);
    void *(*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLboolean (*UnmapBuffer)(GLenum);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*PixelStorei)(GLenum, GLint);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    const GLubyte *(*GetString)(GLenum);
} gl;

static bool made;
static GLuint fbo, color, depth, pbo[2];
static int width, height;
static int reading[2][2]; /* the size each pixel-pack buffer holds a frame of; 0 when it holds none */
static int next_pbo;

static const char *load(void)
{
#define LOAD(name)                                                                                                 \
    if (!(*(void **)&gl.name = backend_proc("gl" #name)))                                                          \
        return "gl" #name;
    LOAD(GenFramebuffers) LOAD(DeleteFramebuffers) LOAD(BindFramebuffer) LOAD(CheckFramebufferStatus)
    LOAD(FramebufferRenderbuffer) LOAD(GenRenderbuffers) LOAD(DeleteRenderbuffers) LOAD(BindRenderbuffer)
    LOAD(RenderbufferStorage) LOAD(GenBuffers) LOAD(DeleteBuffers) LOAD(BindBuffer) LOAD(BufferData)
    LOAD(UnmapBuffer) LOAD(ReadPixels) LOAD(GetIntegerv) LOAD(PixelStorei) LOAD(Viewport) LOAD(GetString)
#undef LOAD
    /* glMapBuffer in desktop GL (2.1 has no glMapBufferRange), glMapBufferRange where that is all there is */
    *(void **)&gl.MapBuffer = backend_proc("glMapBuffer");
    *(void **)&gl.MapBufferRange = backend_proc("glMapBufferRange");
    return gl.MapBuffer || gl.MapBufferRange ? NULL : "glMapBuffer";
}

static void storage(void)
{
    gl.BindRenderbuffer(GL_RENDERBUFFER, color);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    if (depth)
    {
        gl.BindRenderbuffer(GL_RENDERBUFFER, depth);
        gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    }
    gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
}

bool gl_create(int w, int h, int depth_bits, char *error, size_t size)
{
    if (made)
        gl_destroy();
    if (!backend_create(error, size))
        return false;
    const char *missing = load();
    if (missing)
    {
        snprintf(error, size, "OpenGL has no %s", missing);
        backend_destroy();
        return false;
    }
    width = w;
    height = h;
    depth = 0;
    gl.GenFramebuffers(1, &fbo);
    gl.GenRenderbuffers(1, &color);
    if (depth_bits > 0)
        gl.GenRenderbuffers(1, &depth);
    storage();
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (depth)
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        snprintf(error, size, "the %dx%d framebuffer is incomplete (0x%04x)", w, h, status);
        made = true;
        gl_destroy();
        return false;
    }
    gl.Viewport(0, 0, w, h);
    gl.GenBuffers(2, pbo);
    memset(reading, 0, sizeof(reading));
    next_pbo = 0;
    made = true; /* the framebuffer stays bound: Ren'Py takes it as the screen's */
    return true;
}

void *gl_proc(const char *name)
{
    return backend_proc(name);
}

bool gl_make_current(bool on)
{
    return backend_make_current(on);
}

void gl_resize(int w, int h)
{
    if (!made || (w == width && h == height) || w <= 0 || h <= 0)
        return;
    width = w;
    height = h;
    storage();
}

/* Copies out the frame pixel-pack buffer `slot` holds, if any, and publishes it. */
static void publish(int slot)
{
    const int w = reading[slot][0], h = reading[slot][1];
    if (!w)
        return;
    reading[slot][0] = reading[slot][1] = 0;
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
    const void *pixels = gl.MapBuffer ? gl.MapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)
                                      : gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)w * h * 4,
                                                          GL_MAP_READ_BIT);
    if (pixels)
    {
        uint32_t *out = frames_back(w, h);
        if (out)
        {
            frames_copy_flipped(out, (const uint32_t *)pixels, w, h);
            frames_publish();
        }
        gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
}

typedef struct
{
    GLint framebuffer, pack_buffer, pack_alignment;
} Bindings;

static Bindings save_bindings(void)
{
    Bindings b = {0, 0, 4};
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &b.framebuffer);
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &b.pack_buffer);
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &b.pack_alignment);
    return b;
}

static void restore_bindings(Bindings b)
{
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)b.pack_buffer);
    gl.PixelStorei(GL_PACK_ALIGNMENT, b.pack_alignment);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)b.framebuffer);
}

static int64_t now_usec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* What reading the pictures back costs (the PBO read, mapping the frame before and copying it out), in the log
 * every 600 frames: the number the console's first runs are to give. */
static void timed(int64_t usec)
{
    static int64_t total, slowest;
    static int count;
    total += usec;
    slowest = usec > slowest ? usec : slowest;
    if (++count < 600)
        return;
    log_queue(1, "renpy: reading back %dx%d: %.2f ms a frame on average, %.2f ms the slowest of %d", width, height,
              (double)total / count / 1000.0, (double)slowest / 1000.0, count);
    total = slowest = 0;
    count = 0;
}

void gl_present(void)
{
    if (!made)
        return;
    static bool first = true;
    if (first)
    {
        log_step("the first picture, %dx%d", width, height);
        first = false;
    }
    const int64_t start = now_usec();
    const Bindings b = save_bindings();
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    const int slot = next_pbo;
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
    gl.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)width * height * 4, NULL, GL_STREAM_READ);
    gl.ReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    reading[slot][0] = width;
    reading[slot][1] = height;
    next_pbo = slot ^ 1;
    publish(next_pbo); /* the frame before this one, read a frame ago */
    restore_bindings(b);
    timed(now_usec() - start);
}

void gl_flush_pending(void)
{
    if (!made || !reading[next_pbo ^ 1][0])
        return;
    const Bindings b = save_bindings();
    publish(next_pbo ^ 1);
    restore_bindings(b);
}

void gl_destroy(void)
{
    if (!made)
        return;
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.DeleteBuffers(2, pbo);
    gl.DeleteFramebuffers(1, &fbo);
    gl.DeleteRenderbuffers(1, &color);
    if (depth)
        gl.DeleteRenderbuffers(1, &depth);
    fbo = color = depth = 0;
    made = false;
    backend_destroy();
}

const char *gl_renderer(void)
{
    static char text[256];
    if (!made)
        return "no OpenGL";
    snprintf(text, sizeof(text), "%s, OpenGL %s", (const char *)gl.GetString(GL_RENDERER),
             (const char *)gl.GetString(GL_VERSION));
    return text;
}
