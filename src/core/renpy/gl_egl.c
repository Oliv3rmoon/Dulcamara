/*
 * Dulcamara - Ren'Py's core: OpenGL through EGL. On the PS5, ps5-opengl's display with a small pbuffer (milestone 2);
 * on Linux (the host check in CI), Mesa's surfaceless platform. A compatibility context: Ren'Py's gl2 asks for
 * OpenGL 2.0 without a profile.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gl_backend.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static EGLSurface surface = EGL_NO_SURFACE;

bool backend_create(char *error, size_t size)
{
#if defined(__PROSPERO__)
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    display = platform_display ? platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                               : eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
    EGLint major = 0, minor = 0, count = 0;
    EGLConfig config = NULL;
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_NONE};
    const EGLint context_attributes[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                         EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE};
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const char *step = "eglGetDisplay";
    if (display == EGL_NO_DISPLAY)
        goto failed;
    step = "eglInitialize";
    if (!eglInitialize(display, &major, &minor))
        goto failed;
    step = "eglBindAPI";
    if (!eglBindAPI(EGL_OPENGL_API))
        goto failed;
    step = "eglChooseConfig";
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        goto failed;
    step = "eglCreatePbufferSurface";
    surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    if (surface == EGL_NO_SURFACE)
        goto failed;
    step = "eglCreateContext";
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT)
        goto failed;
    step = "eglMakeCurrent";
    if (!eglMakeCurrent(display, surface, surface, context))
        goto failed;
    return true;
failed:
    snprintf(error, size, "EGL: %s failed (0x%04x)", step, (unsigned)eglGetError());
    backend_destroy();
    return false;
}

void *backend_proc(const char *name)
{
    return (void *)eglGetProcAddress(name);
}

bool backend_make_current(bool on)
{
    return on ? eglMakeCurrent(display, surface, surface, context)
              : eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void backend_destroy(void)
{
    if (display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context != EGL_NO_CONTEXT)
        eglDestroyContext(display, context);
    if (surface != EGL_NO_SURFACE)
        eglDestroySurface(display, surface);
    eglTerminate(display);
    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
}
