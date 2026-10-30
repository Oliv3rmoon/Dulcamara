/*
 * Dulcamara - Ren'Py's core: OpenGL through CGL, on macOS (the host check): a context with no window, as legacy
 * OpenGL 2.1, the version Ren'Py's gl2 asks for (2.0) is within.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define GL_SILENCE_DEPRECATION
#include "gl_backend.h"

#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <stdio.h>

static CGLContextObj context;
static void *framework;

bool backend_create(char *error, size_t size)
{
    const CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8,
                                                  kCGLPFAAllowOfflineRenderers, 0};
    CGLPixelFormatObj format = NULL;
    GLint count = 0;
    if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError || !format)
    {
        snprintf(error, size, "CGL: no pixel format");
        return false;
    }
    const CGLError made = CGLCreateContext(format, NULL, &context);
    CGLDestroyPixelFormat(format);
    if (made != kCGLNoError || CGLSetCurrentContext(context) != kCGLNoError)
    {
        snprintf(error, size, "CGL: no context (%d)", (int)made);
        return false;
    }
    if (!framework)
        framework = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    return true;
}

void *backend_proc(const char *name)
{
    return framework ? dlsym(framework, name) : NULL;
}

bool backend_make_current(bool on)
{
    return CGLSetCurrentContext(on ? context : NULL) == kCGLNoError;
}

void backend_destroy(void)
{
    CGLSetCurrentContext(NULL);
    if (context)
        CGLDestroyContext(context);
    context = NULL;
}
