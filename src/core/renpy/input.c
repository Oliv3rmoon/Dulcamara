/*
 * Dulcamara - Ren'Py's core: the controller as Ren'Py's keyboard and mouse (input.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 0.2.5 gave Ren'Py an SDL virtual controller, which played on the build machine and did nothing on a console. Keys
 * and clicks go into SDL's event queue instead, as the skip key always has: Ren'Py's keymap knows them in every game,
 * and a game's own keys (its inventory's, say) are keys too.
 */
#include "input.h"

#include "log.h"
#include "python.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef enum
{
    DO_NOTHING,
    DO_KEY,
    DO_CURSOR,
    DO_CLICK,
    DO_RIGHT_CLICK,
} Kind;

typedef struct
{
    Kind kind;
    SDL_Keycode sym;
    SDL_Scancode code;
    char text; /* the text a key types, for Ren'Py's bindings by character ("h"); 0 for none */
} Action;

static const struct
{
    const char *name;
    SDL_Keycode sym;
    SDL_Scancode code;
    char text;
} NAMED_KEYS[] = {
    {"return", SDLK_RETURN, SDL_SCANCODE_RETURN, 0},       {"escape", SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0},
    {"space", SDLK_SPACE, SDL_SCANCODE_SPACE, ' '},        {"tab", SDLK_TAB, SDL_SCANCODE_TAB, 0},
    {"backspace", SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, 0}, {"pageup", SDLK_PAGEUP, SDL_SCANCODE_PAGEUP, 0},
    {"pagedown", SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN, 0}, {"up", SDLK_UP, SDL_SCANCODE_UP, 0},
    {"down", SDLK_DOWN, SDL_SCANCODE_DOWN, 0},             {"left", SDLK_LEFT, SDL_SCANCODE_LEFT, 0},
    {"right", SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0},          {"home", SDLK_HOME, SDL_SCANCODE_HOME, 0},
    {"end", SDLK_END, SDL_SCANCODE_END, 0},                {"delete", SDLK_DELETE, SDL_SCANCODE_DELETE, 0},
    {"shift", SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, 0},        {"ctrl", SDLK_LCTRL, SDL_SCANCODE_LCTRL, 0},
    {"alt", SDLK_LALT, SDL_SCANCODE_LALT, 0},
};

/* each RetroPad button's own action (RETRO_DEVICE_ID_JOYPAD_* order: B Y Select Start Up Down Left Right A X L R L2 R2
 * L3 R3): Cross Return, Square H (Ren'Py hides its windows), Options Escape (the menu), the D-pad the arrows, Circle
 * Escape (the menu, and back out of it), Triangle Tab (skip on and off), L1 and R1 Page Up and Down (roll back and
 * forward), L3 the cursor; Create, L2 and R3 are free for a game's own keys */
static const char *const DEFAULTS[16] = {"return", "h",      "none",  "escape", "up",   "down",   "left", "right",
                                         "escape", "tab",    "pageup", "pagedown", "none", "none", "cursor", "none"};

#define REPEAT_AFTER 24 /* frames a key is held before it repeats, as a keyboard's does (0.4 s), */
#define REPEAT_EVERY 4  /* and then each 4 frames */
#define TOUCH_SHOWN 120 /* frames the cursor stays shown after the touchpad moved it */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Action actions[16];
static bool actions_set;
static bool detached; /* Ren'Py's thread is closing SDL: no more events go to it */
static uint16_t held;
static int held_frames[16];
static bool skip_wanted, skip_held;
static int skip_frames; /* frames since Ctrl was last pressed */
/* the pointer: where it is (0 to 1 across the picture), whether it moved since Ren'Py's thread last looked, and the
 * buttons wanted down (the touchpad's tap, the cursor's Cross, a click or right click button) */
static float pointer_fx = 0.5f, pointer_fy = 0.5f;
static int16_t touch_x, touch_y;
static bool pointer_moved, touch_pressed, click_held, right_click_held;
static bool button_down, right_button_down;
static bool cursor_on;
static int touch_shown;
static int picture_w = 1280, picture_h = 720;
/* text: whether Ren'Py wants text typed and where its input is, as its thread last looked; the frontend's frames
 * (input_set), so that it looks once in each */
static bool text_wanted;
static float text_top, text_bottom;
static unsigned text_frame, text_looked;
/* what the frontend typed, in its order, until Ren'Py's thread gives it to Ren'Py (input_pump): a character (UTF-8),
 * which goes through pygame (python_type_text), or a key, which goes into SDL's events as the controller's do */
typedef struct
{
    char text[8]; /* "" for a key */
    Action key;
    bool down;
} Typed;
static Typed typed[64];
static int typed_count;

static bool action_of(const char *value, Action *out)
{
    memset(out, 0, sizeof(*out));
    if (!value || !strcmp(value, "none"))
        return value != NULL;
    if (!strcmp(value, "cursor"))
        return out->kind = DO_CURSOR, true;
    if (!strcmp(value, "click"))
        return out->kind = DO_CLICK, true;
    if (!strcmp(value, "rightclick"))
        return out->kind = DO_RIGHT_CLICK, true;
    out->kind = DO_KEY;
    for (size_t i = 0; i < sizeof(NAMED_KEYS) / sizeof(NAMED_KEYS[0]); ++i)
        if (!strcmp(value, NAMED_KEYS[i].name))
        {
            out->sym = NAMED_KEYS[i].sym;
            out->code = NAMED_KEYS[i].code;
            out->text = NAMED_KEYS[i].text;
            return true;
        }
    if (value[0] == 'f' && value[1] >= '1' && value[1] <= '9' && (!value[2] || (value[2] >= '0' && !value[3])))
    {
        const int n = value[2] ? (value[1] - '0') * 10 + (value[2] - '0') : value[1] - '0';
        if (n >= 1 && n <= 12)
        {
            out->sym = SDLK_F1 + (n - 1);
            out->code = (SDL_Scancode)(SDL_SCANCODE_F1 + (n - 1));
            return true;
        }
    }
    if (value[0] >= 'a' && value[0] <= 'z' && !value[1])
    {
        out->sym = (SDL_Keycode)value[0];
        out->code = (SDL_Scancode)(SDL_SCANCODE_A + (value[0] - 'a'));
        out->text = value[0];
        return true;
    }
    if (value[0] >= '0' && value[0] <= '9' && !value[1])
    {
        out->sym = (SDL_Keycode)value[0];
        out->code = value[0] == '0' ? SDL_SCANCODE_0 : (SDL_Scancode)(SDL_SCANCODE_1 + (value[0] - '1'));
        out->text = value[0];
        return true;
    }
    memset(out, 0, sizeof(*out));
    return false;
}

static void set_defaults(void)
{
    for (int b = 0; b < 16; ++b)
        action_of(DEFAULTS[b], &actions[b]);
    actions_set = true;
}

bool input_bind(unsigned button, const char *value)
{
    if (button >= 16)
        return false;
    Action a;
    const bool given = value && *value && strcmp(value, "default"), known = given && action_of(value, &a);
    if (!known)
        action_of(DEFAULTS[button], &a);
    pthread_mutex_lock(&lock);
    if (!actions_set)
        set_defaults();
    actions[button] = a;
    pthread_mutex_unlock(&lock);
    return known || !given;
}

static bool events_up(void)
{
    return !detached && SDL_WasInit(SDL_INIT_EVENTS);
}

/* a key down (with the text it types, which pygame takes from the event right after) or up: both in one go, so that
 * Ren'Py's thread never sees the one without the other */
static void push_key(const Action *a, bool down, bool repeat)
{
    if (!events_up())
        return;
    SDL_Event e[2];
    SDL_zero(e);
    e[0].type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e[0].key.timestamp = SDL_GetTicks();
    e[0].key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e[0].key.repeat = repeat;
    e[0].key.keysym.scancode = a->code;
    e[0].key.keysym.sym = a->sym;
    e[0].key.keysym.mod = KMOD_NONE;
    int n = 1;
    if (down && a->text)
    {
        e[1].type = SDL_TEXTINPUT;
        e[1].text.timestamp = e[0].key.timestamp;
        e[1].text.text[0] = a->text;
        n = 2;
    }
    SDL_PeepEvents(e, n, SDL_ADDEVENT, 0, 0);
}

static void set_cursor(bool on)
{
    cursor_on = on;
    pointer_moved = true;
    log_queue(1, on ? "renpy: the cursor is on (the left stick moves it, Cross clicks)" : "renpy: the cursor is off");
}

/* a button's action, as it goes down or up (or is held, for a key's repeat) */
static void act(int b, bool down, bool repeat)
{
    const Action *a = &actions[b];
    if (cursor_on && b == 0) /* Cross, in cursor mode: a click */
    {
        click_held = down;
        return;
    }
    switch (a->kind)
    {
    case DO_KEY:
        push_key(a, down, repeat);
        break;
    case DO_CURSOR:
        if (down && !repeat)
            set_cursor(!cursor_on);
        break;
    case DO_CLICK:
        click_held = down;
        break;
    case DO_RIGHT_CLICK:
        right_click_held = down;
        break;
    default:
        break;
    }
}

void input_attach(void)
{
    pthread_mutex_lock(&lock);
    detached = false;
    if (!actions_set)
        set_defaults();
    pthread_mutex_unlock(&lock);
}

bool input_is_ours(int device_index)
{
    (void)device_index;
    return false;
}

/* the left stick from -1 to 1, with a dead zone */
static float stick(int16_t v)
{
    const float f = (float)v / 32767.0f, dead = 0.2f;
    return f > dead ? (f - dead) / (1 - dead) : f < -dead ? (f + dead) / (1 - dead) : 0;
}

void input_set(uint16_t retropad, int16_t left_x, int16_t left_y)
{
    pthread_mutex_lock(&lock);
    if (!actions_set)
        set_defaults();
    const float sx = stick(left_x), sy = stick(left_y);
    if (cursor_on)
    {
        /* a picture's width in about a second at full tilt, slower near the middle for aiming */
        const float speed = 1.0f / 60.0f;
        if (sx || sy)
        {
            const float dx = sx * (sx < 0 ? -sx : sx) * speed, dy = sy * (sy < 0 ? -sy : sy) * speed;
            pointer_fx = pointer_fx + dx < 0 ? 0 : pointer_fx + dx > 1 ? 1 : pointer_fx + dx;
            const float aspect = picture_h > 0 ? (float)picture_w / (float)picture_h : 16.0f / 9.0f;
            pointer_fy = pointer_fy + dy * aspect < 0 ? 0 : pointer_fy + dy * aspect > 1 ? 1 : pointer_fy + dy * aspect;
            pointer_moved = true;
        }
    }
    else
    {
        /* the stick as the D-pad, as a keyboard's arrows */
        if (sy < -0.5f)
            retropad |= 1u << 4;
        if (sy > 0.5f)
            retropad |= 1u << 5;
        if (sx < -0.5f)
            retropad |= 1u << 6;
        if (sx > 0.5f)
            retropad |= 1u << 7;
    }
    for (int b = 0; b < 16; ++b)
    {
        const bool now = (retropad >> b) & 1, was = (held >> b) & 1;
        if (now && !was)
        {
            held_frames[b] = 0;
            act(b, true, false);
        }
        else if (!now && was)
            act(b, false, false);
        else if (now && actions[b].kind == DO_KEY && !(cursor_on && b == 0) && ++held_frames[b] >= REPEAT_AFTER &&
                 (held_frames[b] - REPEAT_AFTER) % REPEAT_EVERY == 0)
            act(b, true, true);
    }
    held = retropad;
    if (touch_shown)
        --touch_shown;
    ++text_frame;
    pthread_mutex_unlock(&lock);
}

/* Left Ctrl down or up, into SDL's event queue (which takes events from any thread): Ren'Py's "skip" keymap. Only
 * once SDL's events are up; until then the wish is kept. While held it is pressed again every half second, as a key
 * held on a keyboard is: Ren'Py stops skipping at a menu, or when the game says so, and starts again on a press. */
static void push_skip(void)
{
    const bool again = skip_held && skip_wanted && ++skip_frames >= 30;
    if ((skip_held == skip_wanted && !again) || !events_up())
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
    if (x != touch_x || y != touch_y)
    {
        pointer_fx = ((float)x + 32767.0f) / 65534.0f;
        pointer_fy = ((float)y + 32767.0f) / 65534.0f;
        pointer_moved = true;
        touch_shown = TOUCH_SHOWN;
    }
    touch_x = x;
    touch_y = y;
    touch_pressed = pressed;
    pthread_mutex_unlock(&lock);
}

static void push_button(SDL_Window *window, int x, int y, Uint8 button, bool down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.windowID = SDL_GetWindowID(window);
    e.button.button = button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

/* Whether Ren'Py wants text now: Python is asked without the lock held, as it may wait for the GIL, and the
 * frontend's thread never waits on Ren'Py's. */
static void look_for_text(void)
{
    float top, bottom;
    const bool wanted = python_text_field(&top, &bottom);
    pthread_mutex_lock(&lock);
    const bool changed = wanted != text_wanted;
    text_wanted = wanted;
    text_top = top;
    text_bottom = bottom;
    pthread_mutex_unlock(&lock);
    if (changed && wanted)
        log_queue(1, "renpy: Ren'Py asks for text, its input %d-%d %% down the picture: the frontend's keyboard opens",
                  (int)(top * 100 + 0.5f), (int)(bottom * 100 + 0.5f));
    else if (changed)
        log_queue(1, "renpy: Ren'Py has its text: the frontend's keyboard closes");
}

/* What the frontend typed, to Ren'Py in its order; none once Ren'Py has stopped wanting text. */
static void give_typed(void)
{
    Typed out[sizeof(typed) / sizeof(typed[0])];
    pthread_mutex_lock(&lock);
    const int n = text_wanted ? typed_count : 0;
    memcpy(out, typed, (size_t)n * sizeof(Typed));
    typed_count = 0;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; ++i)
        if (out[i].text[0])
        {
            static bool told;
            if (!python_type_text(out[i].text) && !told)
            {
                told = true;
                log_queue(2, "renpy: the text typed could not be given to Ren'Py");
            }
        }
        else
        {
            pthread_mutex_lock(&lock);
            push_key(&out[i].key, out[i].down, false);
            pthread_mutex_unlock(&lock);
        }
}

void input_pump(SDL_Window *window, int w, int h)
{
    pthread_mutex_lock(&lock);
    if (w > 0 && h > 0)
        picture_w = w, picture_h = h;
    const bool moved = pointer_moved, left = touch_pressed || click_held, right = right_click_held;
    const int x = (int)(pointer_fx * (float)(w - 1) + 0.5f), y = (int)(pointer_fy * (float)(h - 1) + 0.5f);
    pointer_moved = false;
    const bool look = text_frame != text_looked;
    text_looked = text_frame;
    pthread_mutex_unlock(&lock);
    if (look)
        look_for_text();
    give_typed();
    if (!window || w <= 0 || h <= 0)
        return;
    if (moved || (left && !button_down) || (right && !right_button_down))
        SDL_WarpMouseInWindow(window, x, y); /* SDL's own mouse: its state and its motion event */
    if (left != button_down)
    {
        push_button(window, x, y, SDL_BUTTON_LEFT, left);
        button_down = left;
    }
    if (right != right_button_down)
    {
        push_button(window, x, y, SDL_BUTTON_RIGHT, right);
        right_button_down = right;
    }
}

bool input_cursor(int w, int h, int *x, int *y)
{
    pthread_mutex_lock(&lock);
    const bool shown = cursor_on || touch_shown > 0;
    *x = (int)(pointer_fx * (float)(w - 1) + 0.5f);
    *y = (int)(pointer_fy * (float)(h - 1) + 0.5f);
    pthread_mutex_unlock(&lock);
    return shown;
}

bool input_text_wanted(float *top, float *bottom)
{
    pthread_mutex_lock(&lock);
    const bool wanted = text_wanted;
    *top = text_top;
    *bottom = text_bottom;
    pthread_mutex_unlock(&lock);
    return wanted;
}

/* one thing typed, after the others, while Ren'Py wants text */
static void add_typed(const Typed *t)
{
    pthread_mutex_lock(&lock);
    if (text_wanted && typed_count < (int)(sizeof(typed) / sizeof(typed[0])))
        typed[typed_count++] = *t;
    pthread_mutex_unlock(&lock);
}

void input_type_text(uint32_t c)
{
    Typed t;
    memset(&t, 0, sizeof(t));
    unsigned char *u = (unsigned char *)t.text; /* UTF-8 */
    if (!c || (c >= 0xD800 && c < 0xE000) || c >= 0x110000)
        return;
    if (c < 0x80)
        u[0] = (unsigned char)c;
    else if (c < 0x800)
        u[0] = (unsigned char)(0xC0 | c >> 6), u[1] = (unsigned char)(0x80 | (c & 0x3F));
    else if (c < 0x10000)
        u[0] = (unsigned char)(0xE0 | c >> 12), u[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)),
        u[2] = (unsigned char)(0x80 | (c & 0x3F));
    else
        u[0] = (unsigned char)(0xF0 | c >> 18), u[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F)),
        u[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)), u[3] = (unsigned char)(0x80 | (c & 0x3F));
    add_typed(&t);
}

void input_type_key(const char *name, bool down)
{
    Typed t;
    memset(&t, 0, sizeof(t));
    if (!action_of(name, &t.key) || t.key.kind != DO_KEY)
        return;
    t.key.text = 0; /* the key alone: what is typed comes as text */
    t.down = down;
    add_typed(&t);
}

void input_detach(void)
{
    pthread_mutex_lock(&lock);
    detached = true;
    held = 0;
    memset(held_frames, 0, sizeof(held_frames));
    skip_wanted = skip_held = false;
    touch_x = touch_y = 0;
    pointer_fx = pointer_fy = 0.5f;
    pointer_moved = touch_pressed = click_held = right_click_held = false;
    button_down = right_button_down = false;
    cursor_on = false;
    touch_shown = 0;
    text_wanted = false;
    text_top = text_bottom = 0;
    text_frame = text_looked = 0;
    typed_count = 0;
    pthread_mutex_unlock(&lock);
}
