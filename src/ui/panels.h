/* Controller library overlays. GPL-3.0-or-later */
#ifndef NP_PANELS_H
#define NP_PANELS_H
#include "../app.h"
void panels_search(void);
void panels_filters(void);
void panels_details(int index);
void panels_screen(uint32_t pressed);
/* Shared live layout helpers, safe within the 1080p title area. */
void panel_begin(const char *title, const char *subtitle);
void panel_row(int visible_row, const char *label, const char *value, bool selected);
void panel_note(float y, const char *text);
void backup_panel_open(int index);
/* Opened elsewhere than the details panel: Circle returns to back (SCREEN_LIBRARY: Story's library). */
void backup_panel_open_from(int index, enum Screen back);
bool backup_panel_confirming(void);
/* The backups panel's words for a save location's kind and a snapshot's moment ("Auto / clean stop"). */
const char *backup_scope_kind(uint32_t kind);
void backup_panel_screen(uint32_t pressed);
void preferences_open(int index);
void preferences_screen(uint32_t pressed);
#endif
