/*
 * Dulcamara - Ren'Py's core: what Ren'Py's SDL calls do instead (sdl_renames.h renames them in Ren'Py's code; this
 * file calls the real SDL). Ren'Py draws into the core's OpenGL context (gl.c), keeps the core's time and waits
 * through it (handoff.c), mixes into the core's sound (audio.c) and sees only the core's controller (input.c). SDL
 * still runs underneath, with its dummy video and audio drivers, for the window object, events and threads.
 *
 * On a thread other than Ren'Py's (its helpers), SDL_Delay and the waits are SDL's own, in real time.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hooks.h"

#include "audio.h"
#include "frames.h"
#include "gl.h"
#include "handoff.h"
#include "input.h"
#include "log.h"

#include <SDL.h>
#include <pthread.h>
#include <string.h>

/* ---------------------------------------------------------------- state */

#define TIMERS 64

typedef struct
{
    SDL_TimerID id; /* 0: free */
    int64_t due_usec;
    SDL_TimerCallback callback;
    void *param;
    Uint32 interval;
} Timer;

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static Timer timers[TIMERS];
static SDL_TimerID next_timer = 1;
static int gl_attributes[64];
static SDL_Window *gl_window;
static int context_token; /* SDL_GLContext is a pointer: the core's one context is this address */
static int window_w = 1280, window_h = 720;
static Uint32 picture_window; /* the window the picture is drawn for (its id: SDL may have destroyed it since) */

void hooks_reset(void)
{
    pthread_mutex_lock(&timer_lock);
    memset(timers, 0, sizeof(timers));
    next_timer = 1;
    pthread_mutex_unlock(&timer_lock);
    memset(gl_attributes, 0, sizeof(gl_attributes));
    gl_window = NULL;
    picture_window = 0;
    window_w = 1280;
    window_h = 720;
}

/* ---------------------------------------------------------------- time */

static void fire_timers(void)
{
    const int64_t now = handoff_now_usec();
    for (int i = 0; i < TIMERS; ++i)
    {
        pthread_mutex_lock(&timer_lock);
        Timer t = timers[i];
        const bool due = t.id && t.due_usec <= now;
        pthread_mutex_unlock(&timer_lock);
        if (!due)
            continue;
        const Uint32 again = t.callback(t.interval, t.param); /* pygame's: pushes its event */
        pthread_mutex_lock(&timer_lock);
        if (timers[i].id == t.id)
        {
            if (again)
            {
                timers[i].interval = again;
                timers[i].due_usec = now + (int64_t)again * 1000;
            }
            else
                timers[i].id = 0;
        }
        pthread_mutex_unlock(&timer_lock);
    }
}

void hooks_sleep_until(int64_t until_usec)
{
    gl_flush_pending();
    while (handoff_now_usec() < until_usec)
    {
        fire_timers();
        if (!handoff_yield())
            return;
    }
    fire_timers();
}

Uint32 dr_GetTicks(void)
{
    return (Uint32)(handoff_now_usec() / 1000);
}

Uint64 dr_GetTicks64(void)
{
    return (Uint64)(handoff_now_usec() / 1000);
}

void dr_Delay(Uint32 ms)
{
    if (!handoff_on_engine_thread())
    {
        SDL_Delay(ms);
        return;
    }
    hooks_sleep_until(handoff_now_usec() + (int64_t)ms * 1000);
}

SDL_TimerID dr_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param)
{
    pthread_mutex_lock(&timer_lock);
    SDL_TimerID id = 0;
    for (int i = 0; i < TIMERS && !id; ++i)
        if (!timers[i].id)
        {
            id = next_timer++;
            timers[i] = (Timer){id, handoff_now_usec() + (int64_t)interval * 1000, callback, param, interval};
        }
    pthread_mutex_unlock(&timer_lock);
    if (!id)
        SDL_SetError("Dulcamara: no free timer");
    return id;
}

SDL_bool dr_RemoveTimer(SDL_TimerID id)
{
    SDL_bool removed = SDL_FALSE;
    pthread_mutex_lock(&timer_lock);
    for (int i = 0; i < TIMERS; ++i)
        if (id && timers[i].id == id)
        {
            timers[i].id = 0;
            removed = SDL_TRUE;
        }
    pthread_mutex_unlock(&timer_lock);
    return removed;
}

/* What Ren'Py's thread does each time it looks for events: the timers that are due, the pointer's moves. */
static void before_events(void)
{
    fire_timers();
    input_pump(picture_window ? SDL_GetWindowFromID(picture_window) : NULL, window_w, window_h);
}

int dr_PollEvent(SDL_Event *event)
{
    if (handoff_on_engine_thread())
        before_events();
    return SDL_PollEvent(event);
}

int dr_WaitEventTimeout(SDL_Event *event, int timeout)
{
    if (!handoff_on_engine_thread())
        return SDL_WaitEventTimeout(event, timeout);
    const int64_t until = timeout < 0 ? INT64_MAX : handoff_now_usec() + (int64_t)timeout * 1000;
    for (;;)
    {
        before_events();
        if (SDL_PollEvent(event))
            return 1;
        if (handoff_now_usec() >= until)
            return 0;
        gl_flush_pending();
        if (!handoff_yield())
        {
            /* the game is being closed: Ren'Py is asked to quit (python.c); meanwhile its waits end at once */
            if (event)
            {
                memset(event, 0, sizeof(*event));
                event->type = SDL_QUIT;
            }
            return 1;
        }
    }
}

int dr_WaitEvent(SDL_Event *event)
{
    return dr_WaitEventTimeout(event, -1);
}

/* ---------------------------------------------------------------- the controller */

static void controllers_up(Uint32 flags)
{
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER))
        input_attach();
}

int dr_Init(Uint32 flags)
{
    const int r = SDL_Init(flags);
    if (r == 0)
        controllers_up(flags);
    return r;
}

int dr_InitSubSystem(Uint32 flags)
{
    const int r = SDL_InitSubSystem(flags);
    if (r == 0)
        controllers_up(flags);
    return r;
}

SDL_bool dr_IsGameController(int index)
{
    return input_is_ours(index) ? SDL_IsGameController(index) : SDL_FALSE;
}

SDL_Joystick *dr_JoystickOpen(int index)
{
    if (!input_is_ours(index))
    {
        SDL_SetError("Dulcamara: Ren'Py uses its own controller");
        return NULL;
    }
    return SDL_JoystickOpen(index);
}

SDL_GameController *dr_GameControllerOpen(int index)
{
    if (!input_is_ours(index))
    {
        SDL_SetError("Dulcamara: Ren'Py uses its own controller");
        return NULL;
    }
    return SDL_GameControllerOpen(index);
}

/* ---------------------------------------------------------------- the window and the display */

SDL_Window *dr_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    (void)x, (void)y;
    if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) || w <= 0 || h <= 0)
        w = DR_MAX_W, h = DR_MAX_H;
    frames_fit(w, h, DR_MAX_W, DR_MAX_H, &w, &h);
    flags &= ~(Uint32)(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP |
                       SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED);
    SDL_Window *window = SDL_CreateWindow(title, 0, 0, w, h, flags | SDL_WINDOW_HIDDEN);
    if (window)
    {
        picture_window = SDL_GetWindowID(window);
        window_w = w;
        window_h = h;
        log_queue(1, "renpy: the picture is %dx%d", w, h);
    }
    return window;
}

void dr_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    (void)window;
    if (w)
        *w = window_w;
    if (h)
        *h = window_h;
}

void dr_SetWindowSize(SDL_Window *window, int w, int h)
{
    frames_fit(w, h, DR_MAX_W, DR_MAX_H, &w, &h);
    window_w = w;
    window_h = h;
    SDL_SetWindowSize(window, w, h);
    gl_resize(w, h);
}

int dr_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))
        dr_SetWindowSize(window, DR_MAX_W, DR_MAX_H);
    return 0;
}

Uint32 dr_GetWindowFlags(SDL_Window *window)
{
    return (SDL_GetWindowFlags(window) | SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS |
            SDL_WINDOW_MOUSE_FOCUS) &
           ~(Uint32)SDL_WINDOW_HIDDEN;
}

int dr_GetNumVideoDisplays(void)
{
    return 1;
}

int dr_GetDisplayBounds(int index, SDL_Rect *rect)
{
    (void)index;
    *rect = (SDL_Rect){0, 0, DR_DISPLAY_W, DR_DISPLAY_H};
    return 0;
}

int dr_GetDisplayUsableBounds(int index, SDL_Rect *rect)
{
    return dr_GetDisplayBounds(index, rect);
}

int dr_GetNumDisplayModes(int index)
{
    (void)index;
    return 1;
}

int dr_GetDisplayMode(int index, int mode, SDL_DisplayMode *out)
{
    (void)index, (void)mode;
    *out = (SDL_DisplayMode){SDL_PIXELFORMAT_RGB888, DR_DISPLAY_W, DR_DISPLAY_H, 60, NULL};
    return 0;
}

int dr_GetDesktopDisplayMode(int index, SDL_DisplayMode *out)
{
    return dr_GetDisplayMode(index, 0, out);
}

int dr_GetCurrentDisplayMode(int index, SDL_DisplayMode *out)
{
    return dr_GetDisplayMode(index, 0, out);
}

/* ---------------------------------------------------------------- OpenGL */

int dr_GL_LoadLibrary(const char *path)
{
    (void)path;
    return 0;
}

void dr_GL_UnloadLibrary(void)
{
}

void *dr_GL_GetProcAddress(const char *name)
{
    return gl_proc(name);
}

SDL_bool dr_GL_ExtensionSupported(const char *extension)
{
    const unsigned char *(*get_string)(unsigned) = (const unsigned char *(*)(unsigned))gl_proc("glGetString");
    const char *all = get_string ? (const char *)get_string(0x1F03 /* GL_EXTENSIONS */) : NULL;
    if (!all || !extension || !*extension)
        return SDL_FALSE;
    const size_t n = strlen(extension);
    for (const char *at = strstr(all, extension); at; at = strstr(at + n, extension))
        if ((at == all || at[-1] == ' ') && (at[n] == ' ' || at[n] == '\0'))
            return SDL_TRUE;
    return SDL_FALSE;
}

int dr_GL_SetAttribute(SDL_GLattr attribute, int value)
{
    if ((int)attribute >= 0 && (int)attribute < 64)
        gl_attributes[attribute] = value;
    return 0;
}

int dr_GL_GetAttribute(SDL_GLattr attribute, int *value)
{
    switch (attribute)
    {
    case SDL_GL_RED_SIZE:
    case SDL_GL_GREEN_SIZE:
    case SDL_GL_BLUE_SIZE:
    case SDL_GL_ALPHA_SIZE:
        *value = 8;
        return 0;
    case SDL_GL_DOUBLEBUFFER:
        *value = 1;
        return 0;
    default:
        *value = (int)attribute >= 0 && (int)attribute < 64 ? gl_attributes[attribute] : 0;
        return 0;
    }
}

void dr_GL_ResetAttributes(void)
{
    memset(gl_attributes, 0, sizeof(gl_attributes));
}

SDL_GLContext dr_GL_CreateContext(SDL_Window *window)
{
    char error[256];
    if (!gl_create(window_w, window_h, gl_attributes[SDL_GL_DEPTH_SIZE], error, sizeof(error)))
    {
        log_queue(2, "renpy: no OpenGL: %s", error);
        SDL_SetError("%s", error);
        return NULL;
    }
    log_queue(1, "renpy: %s", gl_renderer());
    gl_window = window;
    return &context_token;
}

int dr_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    (void)window;
    return gl_make_current(context != NULL) ? 0 : -1;
}

SDL_Window *dr_GL_GetCurrentWindow(void)
{
    return gl_window;
}

SDL_GLContext dr_GL_GetCurrentContext(void)
{
    return gl_window ? &context_token : NULL;
}

void dr_GL_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
    dr_GetWindowSize(window, w, h);
}

int dr_GL_SetSwapInterval(int interval)
{
    (void)interval;
    return 0;
}

int dr_GL_GetSwapInterval(void)
{
    return 1;
}

void dr_GL_SwapWindow(SDL_Window *window)
{
    (void)window;
    gl_present();
    if (handoff_on_engine_thread())
        handoff_yield(); /* a frame shown: control goes back to the frontend */
}

void dr_GL_DeleteContext(SDL_GLContext context)
{
    (void)context;
    gl_destroy();
    gl_window = NULL;
}

/* ---------------------------------------------------------------- sound */

int dr_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    int freq = desired->freq;
    Uint8 channels = desired->channels;
    audio_open(&freq, &channels, desired->callback, desired->userdata);
    desired->freq = freq;
    desired->channels = channels;
    desired->format = AUDIO_F32SYS;
    desired->size = (Uint32)desired->samples * channels * (Uint32)sizeof(float);
    if (obtained)
        *obtained = *desired;
    if (freq != DR_AUDIO_RATE || channels != 2)
        log_queue(2, "renpy: the mixer is not 48 kHz stereo");
    return 0;
}

void dr_PauseAudio(int pause_on)
{
    audio_pause(pause_on != 0);
}

void dr_LockAudio(void)
{
    audio_lock();
}

void dr_UnlockAudio(void)
{
    audio_unlock();
}

void dr_CloseAudio(void)
{
    audio_close();
}
