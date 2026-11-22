/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise Dulcamara's real GL wrapper and real CGL/EGL backend. No fake GL.
 * A bound unpack buffer makes even a nonzero data pointer a byte offset.
 */
#include "gl.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned E, U;
typedef int I;
typedef intptr_t Z;
#define TEX 0x0de1u
#define RGBA 0x1908u
#define RGBA8 0x8058u
#define BYTE 0x1401u
#define UNPACK 0x88ecu
#define STATIC_DRAW 0x88e4u
#define SRC_ALPHA 0x0302u
#define ZERO 0u
#define ONE 1u

int main(int argc, char **argv)
{
    const size_t offset = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 16;
    const char *mode = argc > 2 ? argv[2] : "offset";
    const int client = !strcmp(mode, "client-reuse");
    const int aligned = !strcmp(mode, "aligned-rows");
    const int changed = !strcmp(mode, "buffer-subdata");
    if (offset > 48)
        return 2;
    setenv("DULCAMARA_GL_CPU_LOAD", "1", 1);
    setenv("DULCAMARA_GL_READ_DIRECT", "0", 1);
    setenv("DULCAMARA_GL_WRITE_BACK", "0", 1);
    char error[256] = {0};
    if (!gl_create(2, 2, 0, error, sizeof(error)))
    {
        fprintf(stderr, "No real GL context: %s\n", error);
        return 77;
    }
    void (*gen_buffers)(I, U *) = gl_proc("glGenBuffers");
    void (*bind_buffer)(E, U) = gl_proc("glBindBuffer");
    void (*buffer_data)(E, Z, const void *, E) = gl_proc("glBufferData");
    void (*buffer_subdata)(E, Z, Z, const void *) = gl_proc("glBufferSubData");
    void (*pixel_store)(E, I) = gl_proc("glPixelStorei");
    void (*delete_buffers)(I, const U *) = gl_proc("glDeleteBuffers");
    void (*gen_textures)(I, U *) = gl_proc("glGenTextures");
    void (*bind_texture)(E, U) = gl_proc("glBindTexture");
    void (*tex_image)(E, I, I, I, I, I, E, E, const void *) = gl_proc("glTexImage2D");
    void (*blend)(E, E, E, E) = gl_proc("glBlendFuncSeparate");
    void (*draw)(E, I, E, const void *) = gl_proc("glDrawElements");
    void (*copy_image)(E, I, E, I, I, I, I, I) = gl_proc("glCopyTexImage2D");
    void (*get_image)(E, I, E, E, void *) = gl_proc("glGetTexImage");
    void (*delete_textures)(I, const U *) = gl_proc("glDeleteTextures");
    E (*get_error)(void) = gl_proc("glGetError");
    if (!gen_buffers || !bind_buffer || !buffer_data || !gen_textures || !bind_texture ||
        !tex_image || !blend || !draw || !copy_image || !get_image || !get_error ||
        !delete_buffers || !delete_textures || !buffer_subdata || !pixel_store)
        return 78;
    const uint8_t pixels[16] = {200, 100, 50, 128, 100, 200, 0,  64,
                                10,  20,  30, 255, 255, 128, 64, 0};
    const uint8_t want[16] = {100, 50, 25, 128, 25, 50, 0, 64, 10, 20, 30, 255, 0, 0, 0, 0};
    uint8_t storage[64], actual[16] = {0};
    memset(storage, 0xee, sizeof(storage));
    memcpy(storage + offset, pixels, sizeof(pixels));
    if (aligned)
    {
        memcpy(storage + offset, pixels, 8);
        memcpy(storage + offset + 16, pixels + 8, 8);
    }
    if (changed)
        memset(storage, 0, sizeof(storage));
    U textures[2] = {0}, buffer = 0;
    gen_textures(2, textures);
    bind_texture(TEX, textures[1]);
    tex_image(TEX, 0, RGBA8, 2, 2, 0, RGBA, BYTE, NULL);
    gen_buffers(1, &buffer);
    bind_buffer(UNPACK, buffer);
    buffer_data(UNPACK, sizeof(storage), storage, STATIC_DRAW);
    if (changed)
        buffer_subdata(UNPACK, (Z)offset, sizeof(pixels), pixels);
    if (!strcmp(mode, "buffer-reuse"))
        memset(storage, 0, sizeof(storage));
    if (client)
        bind_buffer(UNPACK, 0);
    if (aligned)
    {
        pixel_store(0x0cf2u, 3); /* three RGBA texels plus GL's eight-byte row alignment */
        pixel_store(0x0cf5u, 8);
    }
    bind_texture(TEX, textures[0]);
    tex_image(TEX, 0, RGBA8, 2, 2, 0, RGBA, BYTE,
              client ? storage + offset : (const void *)(uintptr_t)offset);
    if (get_error())
        return 79;
    if (client)
        memset(storage, 0, sizeof(storage));
    bind_buffer(UNPACK, 0);
    /* The real texture must still contain the original pixels even after the
     * caller reused its memory. This separates the wrapper bug from the driver. */
    get_image(TEX, 0, RGBA, BYTE, actual);
    if (get_error() || memcmp(actual, pixels, sizeof(pixels)))
    {
        fprintf(stderr, "real source texture differs before CPU shortcut, mode %s\n", mode);
        return 3;
    }
    blend(SRC_ALPHA, ZERO, ONE, ZERO);
    draw(0x0004u, 0, 0x1403u, NULL);
    bind_texture(TEX, textures[1]);
    copy_image(TEX, 0, RGBA, 0, 0, 2, 2, 0);
    get_image(TEX, 0, RGBA, BYTE, actual);
    if (get_error() || memcmp(actual, want, sizeof(want)))
    {
        fprintf(stderr, "wrong premultiplied pixels at unpack offset %zu, mode %s\n", offset, mode);
        for (size_t n = 0; n < sizeof(actual); ++n)
            fprintf(stderr, "%u ", actual[n]);
        fputc('\n', stderr);
        return 1;
    }
    delete_buffers(1, &buffer);
    delete_textures(2, textures);
    gl_destroy();
    printf("PASS: real GL upload and premultiplied copy at unpack offset %zu\n", offset);
    return 0;
}
