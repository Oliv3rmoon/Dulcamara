/* Real SDL events through production input and focus handling. */
#include "core/renpy/input.h"
#include "core/vn_controls.h"
#include "platform/focus.h"
#include <assert.h>
#include <stdio.h>
bool python_text_field(float *a, float *b)
{
    *a = *b = 0;
    return false;
}
bool python_type_text(const char *s)
{
    (void)s;
    return true;
}
void log_queue(int level, const char *format, ...)
{
    (void)level;
    (void)format;
}
static void expect_hand_key(PadState pad, int layout, uint32_t buttons, SDL_Keycode key)
{
    vn_one_hand_input(&pad, layout);
    assert(pad.buttons == buttons);
    /* The default Ren'Py button map uses the same position for these physical and RetroPad bits. */
    input_set((uint16_t)pad.buttons, 0, 0);
    SDL_Event event;
    bool matched = false;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == key)
            matched = true;
    assert(matched);
    input_set(0, 0, 0);
    while (SDL_PollEvent(&event)) {}
}

int main(void)
{
    assert(SDL_Init(SDL_INIT_EVENTS) == 0);
    input_attach();
    assert(input_bind(0, "return"));
    input_set(1, 0, 0);
    SDL_Event e;
    bool down = false, up = false;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN)
            down = true;
    assert(down);
    input_reset_transient();
    while (SDL_PollEvent(&e))
        if (e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_RETURN)
            up = true;
    assert(up);
    for (int i = 0; i < 40; i++)
        input_set(0, 0, 0);
    while (SDL_PollEvent(&e))
        assert(e.type != SDL_KEYDOWN);
    bool background = false;
    SDL_zero(e);
    e.type = SDL_WINDOWEVENT;
    e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    assert(SDL_PushEvent(&e) == 1);
    while (SDL_PollEvent(&e))
        background = plat_focus_event(background, &e);
    assert(background);
    e.type = SDL_APP_DIDENTERFOREGROUND;
    assert(SDL_PushEvent(&e) == 1);
    while (SDL_PollEvent(&e))
        background = plat_focus_event(background, &e);
    assert(!background);
    assert(input_bind(PAD_B, "return"));
    assert(input_bind(PAD_A, "escape"));
    assert(input_bind(PAD_DOWN, "down"));
    assert(input_bind(PAD_LEFT, "left"));
    expect_hand_key((PadState){.buttons=BIT(BTN_L1)}, VN_LEFT_HAND, BIT(BTN_CROSS), SDLK_RETURN);
    expect_hand_key((PadState){.l2=200}, VN_LEFT_HAND, BIT(BTN_CIRCLE), SDLK_ESCAPE);
    expect_hand_key((PadState){.buttons=BIT(BTN_DOWN)}, VN_LEFT_HAND, BIT(BTN_DOWN), SDLK_DOWN);
    expect_hand_key((PadState){.buttons=BIT(BTN_R1)}, VN_RIGHT_HAND, BIT(BTN_CROSS), SDLK_RETURN);
    expect_hand_key((PadState){.rx=-22000}, VN_RIGHT_HAND, BIT(BTN_LEFT), SDLK_LEFT);
    expect_hand_key((PadState){.buttons=BIT(BTN_CIRCLE)}, VN_RIGHT_HAND, BIT(BTN_CIRCLE), SDLK_ESCAPE);
    PadState unchanged = {.buttons=BIT(BTN_R1), .ry=18000};
    vn_one_hand_input(&unchanged, VN_STANDARD);
    assert(unchanged.buttons == BIT(BTN_R1) && unchanged.ry == 18000);
    input_detach();
    SDL_Quit();
    puts("PASS real SDL release/repeat reset and background/foreground events");
}
