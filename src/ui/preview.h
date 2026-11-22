/* Offline visual fixture controls; never part of production/PS5 builds. */
#ifndef NP_UI_PREVIEW_H
#define NP_UI_PREVIEW_H
#if defined(NP_PREVIEW)
#include <stdint.h>
void preview_surface_open(const char *name);
void preview_surface_input(uint32_t *pressed);
void preview_surface_draw(void);
void menu_preview_open(void);
void menu_preview_draw(uint32_t pressed);
void keyboard_preview_open(void);
void keyboard_preview_input(uint32_t pressed);
void keyboard_preview_draw(void);
void insider_prompt_fixture(void);
void update_popup_fixture(int state);
#endif
#endif
