/* Controller state shared by the real panels and host tests. GPL-3.0-or-later */
#ifndef NP_UI_CONTROLLER_H
#define NP_UI_CONTROLLER_H
#include "../keyboard.h"
typedef enum
{
    UI_EDITING,
    UI_ACCEPTED,
    UI_CANCELLED
} UiEditResult;
typedef struct
{
    Keyboard keys;
    char value[160];
} UiEntry;
void ui_entry_open(UiEntry *entry, const char *value);
UiEditResult ui_entry_button(UiEntry *entry, int button);
void ui_entry_clear(UiEntry *entry);
typedef struct
{
    int row, top;
} UiList;
void ui_list_move(UiList *list, int delta, int count, int visible);
void ui_save_timestamp(char *out, size_t size, uint64_t realtime_ns);
bool ui_save_allowed(bool runtime, bool lifecycle, bool maintenance_ready);
#endif
