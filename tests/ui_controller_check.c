#include "ui/controller.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    char stamp[48];
    ui_save_timestamp(stamp, sizeof(stamp), UINT64_C(1794153600123456789));
    assert(!strcmp(stamp, "2026-11-08 16:00:00"));
    UiEntry e;
    ui_entry_open(&e, "日本語 abc");
    assert(ui_entry_button(&e, BTN_SQUARE) == UI_EDITING);
    assert(!strcmp(e.value, "日本語 ab"));
    ui_entry_open(&e, "日本語");
    ui_entry_button(&e, BTN_SQUARE);
    assert(!strcmp(e.value, "日本"));
    ui_entry_button(&e, BTN_CROSS);
    assert(!strcmp(e.value, "日本q"));
    assert(ui_entry_button(&e, BTN_CIRCLE) == UI_CANCELLED);
    ui_entry_open(&e, "intact");
    assert(ui_entry_button(&e, BTN_START) == UI_ACCEPTED);
    assert(!strcmp(e.value, "intact"));
    ui_entry_clear(&e);
    assert(!e.value[0]);
    char full[160];
    memset(full, 'a', 159);
    full[159] = 0;
    ui_entry_open(&e, full);
    ui_entry_button(&e, BTN_CROSS);
    assert(strlen(e.value) == 159);
    UiList l = {0};
    ui_list_move(&l, 1, 20, 8);
    assert(l.row == 1 && l.top == 0);
    for (int i = 0; i < 30; i++)
        ui_list_move(&l, 1, 20, 8);
    assert(l.row == 19 && l.top == 12);
    ui_list_move(&l, -1, 0, 8);
    assert(l.row == 0 && l.top == 0);
    assert(!ui_save_allowed(true, false, true));
    assert(!ui_save_allowed(false, true, true));
    assert(!ui_save_allowed(false, false, false));
    assert(ui_save_allowed(false, false, true));
    puts("PASS production text entry: UTF-8 delete, cancel/done, bounded append; list scrolling "
         "and save guard");
}
