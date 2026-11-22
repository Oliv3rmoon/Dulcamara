/* Real SDL events through production input and focus handling. */
#include "core/renpy/input.h"
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
    input_detach();
    SDL_Quit();
    puts("PASS real SDL release/repeat reset and background/foreground events");
}
