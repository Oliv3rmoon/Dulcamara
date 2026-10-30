/*
 * Dulcamara - Ren'Py's core: the controller (input.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "input.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Joystick *pad;
static SDL_JoystickID pad_id = -1;
static uint16_t held;
static bool skip_wanted, skip_held;
static bool detached; /* Ren'Py's thread is closing SDL: the frontend's thread calls it no more */
static int skip_frames; /* frames since Ctrl was last pressed */
static int16_t pointer_x, pointer_y;
static bool pointer_moved, pointer_pressed, button_down;
static int16_t stick_x, stick_y;

/* The RetroPad's buttons as SDL's, by where they sit: the RetroPad's B is the bottom face button, SDL's A. L2 and R2
 * are SDL's triggers (axes), so -1 here. */
static const int BUTTONS[16] = {
    SDL_CONTROLLER_BUTTON_A,            /* B */
    SDL_CONTROLLER_BUTTON_X,            /* Y */
    SDL_CONTROLLER_BUTTON_BACK,         /* SELECT */
    SDL_CONTROLLER_BUTTON_START,        /* START */
    SDL_CONTROLLER_BUTTON_DPAD_UP,      /* UP */
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,    /* DOWN */
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,    /* LEFT */
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,   /* RIGHT */
    SDL_CONTROLLER_BUTTON_B,            /* A */
    SDL_CONTROLLER_BUTTON_Y,            /* X */
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, /* L */
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, /* R */
    -1,                                 /* L2 */
    -1,                                 /* R2 */
    SDL_CONTROLLER_BUTTON_LEFTSTICK,    /* L3 */
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,   /* R3 */
};

static void apply(void)
{
    for (int b = 0; b < 16; ++b)
        if (BUTTONS[b] >= 0)
            SDL_JoystickSetVirtualButton(pad, BUTTONS[b], (Uint8)((held >> b) & 1));
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, stick_x);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTY, stick_y);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, (held >> 12) & 1 ? 32767 : 0);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, (held >> 13) & 1 ? 32767 : 0);
}

void input_attach(void)
{
    pthread_mutex_lock(&lock);
    detached = false;
    if (!pad)
    {
        SDL_VirtualJoystickDesc d;
        SDL_zero(d);
        d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        d.naxes = SDL_CONTROLLER_AXIS_MAX;
        d.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
        d.button_mask = (1u << d.nbuttons) - 1;
        d.axis_mask = (1u << d.naxes) - 1;
        d.name = "Dulcamara controller";
        const int index = SDL_JoystickAttachVirtualEx(&d);
        if (index >= 0 && !SDL_IsGameController(index))
        {
            /* an SDL without a mapping for virtual controllers: the one their button order means */
            char guid[64], mapping[512];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
            snprintf(mapping, sizeof(mapping),
                     "%s,Dulcamara controller,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,"
                     "rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                     "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,",
                     guid);
            SDL_GameControllerAddMapping(mapping);
        }
        pad = index >= 0 ? SDL_JoystickOpen(index) : NULL;
        pad_id = pad ? SDL_JoystickInstanceID(pad) : -1;
        if (pad)
            apply();
    }
    pthread_mutex_unlock(&lock);
}

bool input_is_ours(int device_index)
{
    pthread_mutex_lock(&lock);
    const bool ours = pad && SDL_JoystickGetDeviceInstanceID(device_index) == pad_id;
    pthread_mutex_unlock(&lock);
    return ours;
}

void input_set(uint16_t retropad, int16_t left_x, int16_t left_y)
{
    pthread_mutex_lock(&lock);
    held = retropad;
    stick_x = left_x;
    stick_y = left_y;
    if (pad)
        apply();
    pthread_mutex_unlock(&lock);
}

/* Left Ctrl down or up, into SDL's event queue (which takes events from any thread): Ren'Py's "skip" keymap. Only
 * once SDL's events are up; until then the wish is kept. While held it is pressed again every half second, as a key
 * held on a keyboard is: Ren'Py stops skipping at a menu, or when the game says so, and starts again on a press. */
static void push_skip(void)
{
    const bool again = skip_held && skip_wanted && ++skip_frames >= 30;
    if ((skip_held == skip_wanted && !again) || detached || !SDL_WasInit(SDL_INIT_EVENTS))
        return;
    skip_frames = 0;
    SDL_Event e;
    SDL_zero(e);
    e.type = skip_wanted ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = skip_wanted ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = SDL_SCANCODE_LCTRL;
    e.key.keysym.sym = SDLK_LCTRL;
    e.key.keysym.mod = skip_wanted ? KMOD_LCTRL : KMOD_NONE;
    if (SDL_PushEvent(&e) == 1)
        skip_held = skip_wanted;
}

void input_set_skip(bool on)
{
    pthread_mutex_lock(&lock);
    skip_wanted = on;
    push_skip();
    pthread_mutex_unlock(&lock);
}

void input_set_pointer(int16_t x, int16_t y, bool pressed)
{
    pthread_mutex_lock(&lock);
    if (x != pointer_x || y != pointer_y)
        pointer_moved = true;
    pointer_x = x;
    pointer_y = y;
    pointer_pressed = pressed;
    pthread_mutex_unlock(&lock);
}

void input_pump(SDL_Window *window, int w, int h)
{
    pthread_mutex_lock(&lock);
    const bool moved = pointer_moved, pressed = pointer_pressed;
    const int x = (int)(((long)pointer_x + 32767) * (w - 1) / 65534), y = (int)(((long)pointer_y + 32767) * (h - 1) / 65534);
    pointer_moved = false;
    pthread_mutex_unlock(&lock);
    if (!window || w <= 0 || h <= 0)
        return;
    if (moved || (pressed && !button_down))
        SDL_WarpMouseInWindow(window, x, y); /* SDL's own mouse: its state and its motion event */
    if (pressed != button_down)
    {
        SDL_Event e;
        SDL_zero(e);
        e.type = pressed ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.windowID = SDL_GetWindowID(window);
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = pressed ? SDL_PRESSED : SDL_RELEASED;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
        if (SDL_PushEvent(&e) == 1)
            button_down = pressed;
    }
}

void input_detach(void)
{
    pthread_mutex_lock(&lock);
    if (pad)
    {
        SDL_JoystickClose(pad);
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == pad_id)
                SDL_JoystickDetachVirtual(i);
    }
    pad = NULL;
    pad_id = -1;
    detached = true;
    held = 0;
    skip_wanted = skip_held = false;
    pointer_x = pointer_y = 0;
    pointer_moved = pointer_pressed = button_down = false;
    stick_x = stick_y = 0;
    pthread_mutex_unlock(&lock);
}
