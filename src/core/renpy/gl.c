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
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLubyte, GLboolean;
typedef intptr_t GLsizeiptr, GLintptr;

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_STREAM_READ 0x88E1
#define GL_READ_ONLY 0x88B8
#define GL_MAP_READ_BIT 0x0001
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_INVERT_MESA 0x8758
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
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
    GLenum (*GetError)(void);
    /* Ren'Py's own, which the core watches (gl_proc), and what the copies' write-back and checks need: any may be
     * missing, and then what needs it is not done */
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*CopyTexImage2D)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);
    void (*CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
    void (*GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*DrawElements)(GLenum, GLsizei, GLenum, const void *);
} gl;

static bool made;
static GLuint fbo, color, depth, pbo[2];
static int width, height;
static int reading[2][2]; /* the size each pixel-pack buffer holds a frame of; 0 when it holds none */
static size_t allocated[2]; /* each pixel-pack buffer's storage, made once for a picture's size */
static int next_pbo;

/* ps5-opengl reads a picture back on the CPU as glReadPixels is called, whether into a pixel-pack buffer or not: it
 * maps the framebuffer, waits for the GPU and untiles it pixel by pixel (ps5_transfer_map). The pixel-pack buffers
 * then only add work: Mesa converts each row into one, the driver writes it back from the CPU's cache, the next frame
 * maps it (another write-back) and frames_copy_flipped copies it out again. So on the PS5 the picture is read
 * straight into the frame, top row first (GL_PACK_INVERT_MESA), in the framebuffer's own order of channels (Mesa's
 * plain copy), and red and blue swapped after if that order is RGBA. Elsewhere the reads are the GPU's and the
 * pixel-pack buffers keep it from being waited on. DULCAMARA_GL_READ_DIRECT=1 or 0 chooses either anywhere. */
#if defined(__PROSPERO__)
#define ON_PS5 true
#else
#define ON_PS5 false
#endif
static bool direct;
static bool inverted;           /* the rows come top first */
static GLenum read_format = GL_BGRA, read_type = GL_UNSIGNED_INT_8_8_8_8_REV;
static bool swap_red_blue;      /* read as RGBA: the frame is XRGB8888 */
static uint32_t *upside_down;   /* the picture as read, where the rows cannot come top first */
static size_t upside_down_size; /* in pixels */

/* A texture Ren'Py fills by copying (glCopyTexImage2D: each image it loads that is 64x64 or larger, each picture it
 * draws into a texture, each frame of a movie) has its memory written back from the CPU's cache before the copy, on
 * the PS5 (DULCAMARA_GL_WRITE_BACK=1 or 0 chooses anywhere). ps5-opengl zeroes a new texture through the cache
 * (memset in ps5_resource_create_unlocked) and leaves those lines dirty; the copy is drawn by the GPU, which writes
 * memory past the cache (ps5_blit_gpu_color), and when the zeroed lines are written back later (evicted, or flushed
 * by the driver as the texture is first drawn, ps5_prepare_texture) they put zeros over the copy: parts of a picture,
 * or all of a small one, drawn transparent. Writing one texel first makes the driver write the whole texture back
 * while it is still empty (ps5_transfer_unmap); the copy then goes into that same storage (glCopyTexSubImage2D), as
 * glCopyTexImage2D does when the size and format are the ones Ren'Py just gave the texture. */
static bool write_back_copies;
#define COPY_CHECKS 4 /* the first copies of 64x64 or more compared with what they were copied from */
static int copy_checks;

static const char *load(void)
{
#define LOAD(name)                                                                                                 \
    if (!(*(void **)&gl.name = backend_proc("gl" #name)))                                                          \
        return "gl" #name;
    LOAD(GenFramebuffers) LOAD(DeleteFramebuffers) LOAD(BindFramebuffer) LOAD(CheckFramebufferStatus)
    LOAD(FramebufferRenderbuffer) LOAD(GenRenderbuffers) LOAD(DeleteRenderbuffers) LOAD(BindRenderbuffer)
    LOAD(RenderbufferStorage) LOAD(GenBuffers) LOAD(DeleteBuffers) LOAD(BindBuffer) LOAD(BufferData)
    LOAD(UnmapBuffer) LOAD(ReadPixels) LOAD(GetIntegerv) LOAD(PixelStorei) LOAD(Viewport) LOAD(GetString)
    LOAD(GetError)
#undef LOAD
#define OPTIONAL(name) *(void **)&gl.name = backend_proc("gl" #name);
    OPTIONAL(GenTextures) OPTIONAL(DeleteTextures) OPTIONAL(BindTexture) OPTIONAL(TexImage2D) OPTIONAL(TexSubImage2D)
    OPTIONAL(CopyTexImage2D) OPTIONAL(CopyTexSubImage2D) OPTIONAL(GetTexLevelParameteriv)
    OPTIONAL(FramebufferTexture2D) OPTIONAL(DrawElements)
#undef OPTIONAL
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

static bool setting(const char *name, bool otherwise)
{
    const char *value = getenv(name);
    return value && *value ? strcmp(value, "0") != 0 : otherwise;
}

static void clear_errors(void)
{
    for (int i = 0; i < 8 && gl.GetError(); ++i)
    {
    }
}

/* How the picture is read back (direct above), chosen with the framebuffer bound; said once in the log. */
static void choose_reading(void)
{
    direct = setting("DULCAMARA_GL_READ_DIRECT", ON_PS5);
    write_back_copies = setting("DULCAMARA_GL_WRITE_BACK", ON_PS5);
    copy_checks = 0;
    if (!direct)
    {
        log_queue(1, "renpy: pictures are read back through two pixel-pack buffers; copied textures are%s written "
                     "back first",
                  write_back_copies ? "" : " not");
        return;
    }
    /* the framebuffer's own order, which Mesa copies as it is (GL_RGBA unless it is BGRA); OpenGL 2.1 has no such
     * query, and then RGBA it is */
    clear_errors();
    GLint format = 0, type = 0;
    gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
    gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    const bool bgra = format == GL_BGRA && (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV);
    read_format = bgra ? GL_BGRA : GL_RGBA;
    read_type = bgra ? (GLenum)type : GL_UNSIGNED_BYTE;
    swap_red_blue = !bgra;
    clear_errors();
    gl.PixelStorei(GL_PACK_INVERT_MESA, 1);
    inverted = gl.GetError() == 0;
    if (inverted)
        gl.PixelStorei(GL_PACK_INVERT_MESA, 0);
    clear_errors();
    log_queue(1, "renpy: pictures are read back straight into the frame, as %s%s, %s; copied textures are%s "
                 "written back first",
              bgra ? "BGRA" : "RGBA", bgra ? "" : " (red and blue swapped after)",
              inverted ? "top row first" : "turned over after", write_back_copies ? "" : " not");
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
    memset(allocated, 0, sizeof(allocated));
    next_pbo = 0;
    choose_reading();
    made = true; /* the framebuffer stays bound: Ren'Py takes it as the screen's */
    return true;
}

static int64_t now_usec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* ---------------------------------------------------------------- Ren'Py's calls the core watches */

/* What Ren'Py's OpenGL calls cost: this picture's, and the report's so far (timed below) */
typedef struct
{
    int draws, timed_draws, uploads, storages, copies, errors;
    int64_t draw_usec, upload_usec, upload_pixels, storage_usec, copy_usec, write_back_usec;
} Work;

static Work work, reported;
static bool time_draws; /* every 30th picture's draws: a clock read around each of them would cost more than many */

/* OpenGL's errors after an upload or a copy (a texture without memory is drawn black), the first 8 in the log */
static void note_call_errors(const char *call, GLsizei w, GLsizei h)
{
    static int logged;
    for (int i = 0; i < 4; ++i)
    {
        const GLenum error = gl.GetError();
        if (!error)
            return;
        ++work.errors;
        if (logged++ < 8)
            log_queue(2, "renpy: OpenGL reported error 0x%04x after %s, %dx%d", error, call, w, h);
    }
}

static void watched_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    ++work.draws;
    if (!time_draws)
    {
        gl.DrawElements(mode, count, type, indices);
        return;
    }
    const int64_t start = now_usec();
    gl.DrawElements(mode, count, type, indices);
    work.draw_usec += now_usec() - start;
    ++work.timed_draws;
}

static void watched_tex_image_2d(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                                 GLenum format, GLenum type, const void *pixels)
{
    GLint unpack = 0;
    if (!pixels)
        gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    const int64_t start = now_usec();
    gl.TexImage2D(target, level, internal, w, h, border, format, type, pixels);
    const int64_t spent = now_usec() - start;
    if (pixels || unpack)
    {
        ++work.uploads;
        work.upload_usec += spent;
        work.upload_pixels += (int64_t)w * h;
    }
    else
    {
        ++work.storages;
        work.storage_usec += spent;
    }
    note_call_errors(pixels || unpack ? "an upload" : "a texture's storage", w, h);
}

/* The unpacking state a one-texel upload needs (no buffer, nothing skipped), and Ren'Py's back after it: Ren'Py
 * leaves GL_UNPACK_ROW_LENGTH at the last surface's pitch. */
typedef struct
{
    GLint buffer, row_length, skip_rows, skip_pixels, alignment;
} Unpacking;

static Unpacking plain_unpacking(void)
{
    Unpacking u = {0, 0, 0, 0, 4};
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &u.buffer);
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &u.row_length);
    gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &u.skip_rows);
    gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &u.skip_pixels);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &u.alignment);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return u;
}

static void restore_unpacking(Unpacking u)
{
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)u.buffer);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, u.row_length);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, u.skip_rows);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, u.skip_pixels);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, u.alignment);
}

/* The bound texture's level as a w x h texture of `internal`, its memory written back (above): true when the copy
 * can go into it with glCopyTexSubImage2D. Ren'Py has always just made it so (allocate_texture); otherwise it is
 * made here, as glCopyTexImage2D would. */
static bool written_back(GLint level, GLenum internal, GLsizei w, GLsizei h, bool write)
{
    GLint have_w = 0, have_h = 0, have_internal = 0;
    gl.GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &have_w);
    gl.GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &have_h);
    gl.GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_INTERNAL_FORMAT, &have_internal);
    const Unpacking u = plain_unpacking();
    /* RGBA as Ren'Py asks for it, or as a driver that names the sized format it chose answers */
    const bool rgba = (internal == GL_RGBA || internal == GL_RGBA8) &&
                      ((GLenum)have_internal == GL_RGBA || (GLenum)have_internal == GL_RGBA8);
    if (have_w != w || have_h != h || ((GLenum)have_internal != internal && !rgba))
        gl.TexImage2D(GL_TEXTURE_2D, level, (GLint)internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    static const uint32_t texel = 0;
    if (write)
        gl.TexSubImage2D(GL_TEXTURE_2D, level, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &texel);
    restore_unpacking(u);
    return gl.GetError() == 0;
}

/* A copy check's rows: the picture's first and last as the read framebuffer has them, as a texture made just for the
 * check has them when copied without the write-back, and as Ren'Py's own texture has them after its copy. */
typedef struct
{
    uint32_t *rows; /* 6 rows of w */
    GLint pack_buffer, pack_alignment, texture;
    bool fresh_read;
} CopyCheck;

static void read_two_rows(GLint x, GLint y, GLsizei w, GLsizei h, uint32_t *first, uint32_t *last)
{
    gl.ReadPixels(x, y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, first);
    gl.ReadPixels(x, y + h - 1, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, last);
}

/* A texture's first and last rows, read through a framebuffer of its own: as ps5-opengl maps it, it writes the
 * texture's lines back from the cache and then reads them, so a copy the zeroed lines would spoil shows spoilt. */
static bool read_texture_rows(GLuint texture, GLsizei w, GLsizei h, uint32_t *first, uint32_t *last)
{
    GLint read_framebuffer = 0;
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    GLuint framebuffer = 0;
    gl.GenFramebuffers(1, &framebuffer);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool complete = gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete)
        read_two_rows(0, 0, w, h, first, last);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_framebuffer);
    gl.DeleteFramebuffers(1, &framebuffer);
    return complete;
}

static bool check_before(CopyCheck *c, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h)
{
    c->rows = malloc((size_t)w * 6 * sizeof(uint32_t));
    if (!c->rows)
        return false;
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &c->pack_buffer);
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &c->pack_alignment);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &c->texture);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    read_two_rows(x, y, w, h, c->rows, c->rows + w);
    GLuint fresh = 0;
    gl.GenTextures(1, &fresh);
    gl.BindTexture(GL_TEXTURE_2D, fresh);
    c->fresh_read = written_back(0, internal, w, h, false);
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, w, h);
    c->fresh_read = c->fresh_read && read_texture_rows(fresh, w, h, c->rows + 2 * w, c->rows + 3 * w);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)c->texture);
    gl.DeleteTextures(1, &fresh);
    return true;
}

static int differences(const uint32_t *a, const uint32_t *b, GLsizei w, int *zeros)
{
    int n = 0;
    for (GLsizei i = 0; i < 2 * w; ++i)
        if (a[i] != b[i])
        {
            ++n;
            *zeros += b[i] == 0;
        }
    return n;
}

static void check_after(CopyCheck *c, GLsizei w, GLsizei h)
{
    const bool read = read_texture_rows((GLuint)c->texture, w, h, c->rows + 4 * w, c->rows + 5 * w);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)c->pack_buffer);
    gl.PixelStorei(GL_PACK_ALIGNMENT, c->pack_alignment);
    int fresh_zeros = 0, own_zeros = 0;
    const int fresh = c->fresh_read ? differences(c->rows, c->rows + 2 * w, w, &fresh_zeros) : -1;
    const int own = read ? differences(c->rows, c->rows + 4 * w, w, &own_zeros) : -1;
    log_queue(fresh > 0 && !own ? 2 : 1,
              "renpy: copy check %d, %dx%d: of the %d texels in its first and last rows, a new texture copied as "
              "Ren'Py does has %d wrong (%d zero), Ren'Py's own (%s) %d wrong (%d zero)",
              copy_checks, w, h, 2 * w, fresh, fresh_zeros, write_back_copies ? "written back first" : "as it is",
              own, own_zeros);
    free(c->rows);
}

static void watched_copy_tex_image_2d(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w,
                                      GLsizei h, GLint border)
{
    const bool ours = target == GL_TEXTURE_2D && !border && w > 0 && h > 0 &&
                      (internal == GL_RGBA || internal == GL_RGBA8) && gl.TexImage2D && gl.TexSubImage2D &&
                      gl.CopyTexSubImage2D && gl.GetTexLevelParameteriv;
    CopyCheck check = {0};
    const bool checked = ours && !level && copy_checks < COPY_CHECKS && (int64_t)w * h >= 64 * 64 && h > 1 &&
                         gl.GenTextures && gl.DeleteTextures && gl.BindTexture && gl.FramebufferTexture2D &&
                         (++copy_checks, check_before(&check, internal, x, y, w, h));
    const int64_t start = now_usec();
    if (write_back_copies && ours && written_back(level, internal, w, h, true))
    {
        work.write_back_usec += now_usec() - start;
        gl.CopyTexSubImage2D(target, level, 0, 0, x, y, w, h);
    }
    else
        gl.CopyTexImage2D(target, level, internal, x, y, w, h, border);
    ++work.copies;
    work.copy_usec += now_usec() - start;
    note_call_errors("a texture copy", w, h);
    if (checked)
        check_after(&check, w, h);
}

/* Ren'Py's OpenGL functions; three of them watched (above), passing every call on */
void *gl_proc(const char *name)
{
    if (!name)
        return NULL;
    void *real = backend_proc(name);
    if (!real)
        return NULL;
#define WATCH(gl_name, field, watcher)                                                                             \
    if (!strcmp(name, gl_name))                                                                                     \
    {                                                                                                              \
        *(void **)&gl.field = real;                                                                                \
        return (void *)watcher;                                                                                    \
    }
    WATCH("glDrawElements", DrawElements, watched_draw_elements)
    WATCH("glTexImage2D", TexImage2D, watched_tex_image_2d)
    WATCH("glCopyTexImage2D", CopyTexImage2D, watched_copy_tex_image_2d)
#undef WATCH
    return real;
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

/* How much of a picture is not black (one pixel in seven looked at), in the log for the first one read back, the
 * first one that is not black, the 60th, the 300th and every 600th: with Ren'Py running and the screen black, it
 * says whether the pictures it draws are black (its first ones are, as it starts). */
static void note_picture(const uint32_t *pixels, int w, int h)
{
    static int count;
    static bool lit_seen;
    ++count;
    const size_t n = (size_t)w * (size_t)h;
    size_t looked = 0, lit = 0;
    for (size_t i = 0; i < n; i += 7, ++looked)
        lit += (pixels[i] & 0xFFFFFFu) != 0;
    const bool first_lit = lit && !lit_seen;
    lit_seen = lit_seen || lit;
    if (count == 1 || first_lit || count == 60 || count == 300 || count % 600 == 0)
        log_queue(1, "renpy: picture %d, %dx%d: %.1f%% of it not black%s", count, w, h,
                  looked ? 100.0 * (double)lit / (double)looked : 0.0,
                  first_lit ? " (the first that is not)" : lit_seen ? "" : " (none has been yet)");
}

/* OpenGL's errors (Ren'Py's or the read-back's) as the first pictures are read back: it reports them no other way */
static void note_errors(void)
{
    static int checks;
    if (checks >= 3)
        return;
    ++checks;
    for (int i = 0; i < 8; ++i)
    {
        const GLenum error = gl.GetError();
        if (!error)
            return;
        log_queue(2, "renpy: OpenGL reported error 0x%04x as picture %d was read back", error, checks);
    }
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
            note_picture(out, w, h);
            frames_publish();
        }
        gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    else
    {
        static int unmapped;
        if (unmapped++ < 3)
            log_queue(2, "renpy: a picture could not be read back (its buffer would not map)");
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

/* The picture read straight into the frame (direct, above) and published. The time glReadPixels takes is returned. */
static int64_t read_directly(void)
{
    uint32_t *out = frames_back(width, height);
    if (!out)
        return 0;
    const size_t n = (size_t)width * (size_t)height;
    uint32_t *into = out;
    if (!inverted)
    {
        if (upside_down_size < n)
        {
            uint32_t *grown = realloc(upside_down, n * sizeof(uint32_t));
            if (!grown)
                return 0;
            upside_down = grown;
            upside_down_size = n;
        }
        into = upside_down;
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (inverted)
        gl.PixelStorei(GL_PACK_INVERT_MESA, 1);
    const int64_t start = now_usec();
    gl.ReadPixels(0, 0, width, height, read_format, read_type, into);
    const int64_t spent = now_usec() - start;
    if (inverted)
        gl.PixelStorei(GL_PACK_INVERT_MESA, 0); /* Ren'Py's own reads (its screenshots) are not turned over */
    else
        frames_copy_flipped(out, upside_down, width, height);
    if (swap_red_blue)
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t p = out[i];
            out[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
        }
    note_picture(out, width, height);
    frames_publish();
    return spent;
}

/* What reading the pictures back costs (the read, mapping the frame before and copying it out) and what Ren'Py's
 * OpenGL calls cost between them, in the log every 600 frames: the numbers a console's runs are to give. A picture
 * whose OpenGL work (its draws, uploads, copies and read-back) took more than 50 ms is noted at once, the first 16:
 * a freeze the player sees has its cause in the log. */
static void timed(int64_t present_usec, int64_t read_usec, int64_t present_start)
{
    static int64_t total, slowest, read_total, first_start, gl_slowest;
    static int count, slow_pictures, slow_noted;
    total += present_usec;
    read_total += read_usec;
    slowest = present_usec > slowest ? present_usec : slowest;
    if (!count)
        first_start = present_start;
    const int64_t draws_estimate = work.timed_draws ? work.draw_usec * work.draws / work.timed_draws : 0;
    const int64_t gl_usec = present_usec + work.upload_usec + work.storage_usec + work.copy_usec + draws_estimate;
    gl_slowest = gl_usec > gl_slowest ? gl_usec : gl_slowest;
    if (gl_usec > 50000)
    {
        ++slow_pictures;
        if (slow_noted++ < 16)
            log_queue(2,
                      "renpy: a picture's OpenGL work took %.1f ms: %d draws, %d uploads (%.1f ms), %d new textures "
                      "(%.1f ms), %d copies (%.1f ms), the read-back %.1f ms",
                      (double)gl_usec / 1000.0, work.draws, work.uploads, (double)work.upload_usec / 1000.0,
                      work.storages, (double)work.storage_usec / 1000.0, work.copies,
                      (double)work.copy_usec / 1000.0, (double)present_usec / 1000.0);
    }
    reported.draws += work.draws;
    reported.timed_draws += work.timed_draws;
    reported.draw_usec += work.draw_usec;
    reported.uploads += work.uploads;
    reported.upload_usec += work.upload_usec;
    reported.upload_pixels += work.upload_pixels;
    reported.storages += work.storages;
    reported.storage_usec += work.storage_usec;
    reported.copies += work.copies;
    reported.copy_usec += work.copy_usec;
    reported.write_back_usec += work.write_back_usec;
    reported.errors += work.errors;
    memset(&work, 0, sizeof(work));
    if (++count < 600)
    {
        time_draws = count % 30 == 0;
        return;
    }
    time_draws = false;
    const double n = count, ms = 1000.0;
    log_queue(1, "renpy: reading back %dx%d: %.2f ms a frame on average (%.2f of it in glReadPixels), %.2f ms the "
                 "slowest of %d",
              width, height, (double)total / n / ms, (double)read_total / n / ms, (double)slowest / ms, count);
    const Work *r = &reported;
    log_queue(1,
              "renpy: Ren'Py's OpenGL work, %d pictures in %.1f s: %.1f draws a picture (%.3f ms each), %.2f "
              "uploads (%.2f ms each, %.0f kilopixels), %.2f new textures (%.2f ms each), %.2f copies (%.2f ms "
              "each, %.2f of it writing back), %.1f ms the most a picture took, %d over 50 ms, %d OpenGL errors",
              count, (double)(now_usec() - first_start) / 1e6, r->draws / n,
              r->timed_draws ? (double)r->draw_usec / r->timed_draws / ms : 0.0, r->uploads / n,
              r->uploads ? (double)r->upload_usec / r->uploads / ms : 0.0,
              r->uploads ? (double)r->upload_pixels / r->uploads / 1000.0 : 0.0, r->storages / n,
              r->storages ? (double)r->storage_usec / r->storages / ms : 0.0, r->copies / n,
              r->copies ? (double)r->copy_usec / r->copies / ms : 0.0,
              r->copies ? (double)r->write_back_usec / r->copies / ms : 0.0, (double)gl_slowest / ms, slow_pictures,
              r->errors);
    total = slowest = read_total = gl_slowest = 0;
    count = slow_pictures = 0;
    memset(&reported, 0, sizeof(reported));
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
    int64_t read_usec;
    if (direct)
    {
        read_usec = read_directly();
        note_errors();
    }
    else
    {
        const int slot = next_pbo;
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
        /* storage only for a new size: 0.2.5 made it again each frame, so fast-forward's 8 frames a refresh had
         * ps5-opengl allocate and retire 8 of them (3.6 MB each at 1280x720) */
        const size_t bytes = (size_t)width * (size_t)height * 4;
        if (allocated[slot] != bytes)
        {
            gl.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_READ);
            allocated[slot] = bytes;
        }
        const int64_t read_start = now_usec();
        gl.ReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        read_usec = now_usec() - read_start;
        note_errors();
        reading[slot][0] = width;
        reading[slot][1] = height;
        next_pbo = slot ^ 1;
        publish(next_pbo); /* the frame before this one, read a frame ago */
    }
    restore_bindings(b);
    timed(now_usec() - start, read_usec, start);
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
    free(upside_down);
    upside_down = NULL;
    upside_down_size = 0;
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
