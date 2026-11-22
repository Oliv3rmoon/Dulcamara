/* Drives the production panel against deterministic worker-boundary fakes. */
#include "app.h"
#include "saves.h"
#include "core/host.h"
#include "ui/panels.h"
#include "ui/story.h"
#include "profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

App app;
static Game fixture;
static SavesStatus service;
static SavesState first_state = SAVES_DONE;
static uint64_t serial;
static unsigned starts[8], notices, clears;
static char rendered_error[256], last_notice[256];
/* Story view: the same panel drawn as save slots (story.c); its drawing is recorded, not done. */
static bool story_mode;
static unsigned story_draws, classic_rows, noted_lists;
static StoryBackups last_view;
static StorySlots last_slots; /* what Story's backups screen shows in place of the slots (story_model.c) */
static const char failure[] = "Fixture native save read failed; check storage.";

void str_copy(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src);
}
bool host_owns_runtime(void)
{
    return false;
}
bool app_save_lifecycle_busy(void)
{
    return false;
}
bool maintenance_ready(void)
{
    return true;
}
int app_nav_pad(void)
{
    return 0;
}
void app_clear_toasts(void)
{
    clears++;
}
void app_toast(const char *s)
{
    notices++;
    str_copy(last_notice, sizeof(last_notice), s);
}
void app_draw_hints(const int *g, const char *const *l, int n, const char *r)
{
    (void)g;
    (void)l;
    (void)n;
    (void)r;
}
void eclipse_status(float x, float y, float size)
{
    (void)x;
    (void)y;
    (void)size;
}
void panel_begin(const char *title, const char *subtitle)
{
    (void)title;
    (void)subtitle;
}
bool story_view(void)
{
    return story_mode;
}
void story_draw_backups(const StoryBackups *b)
{
    story_draws++;
    last_view = *b;
    last_slots = story_slots_state(b->list_ok, b->error);
    assert(b->row >= b->top && b->row - b->top < STORY_SLOT_CELLS);
    if (b->error && !strcmp(b->error, failure))
        str_copy(rendered_error, sizeof(rendered_error), b->error);
}
void story_note_backups(const char *id, const SavesStatus *status)
{
    assert(!strcmp(id, "fixture-game") && status->operation == SAVES_LIST);
    noted_lists++;
}
void shelf_open(void)
{
    app.screen = SCREEN_LIBRARY;
}
void path_join(char *dst, size_t size, const char *a, const char *b)
{
    snprintf(dst, size, "%s/%s", a, b);
}
bool profile_load(GameProfile *profile, const char *path)
{
    (void)path;
    memset(profile, 0, sizeof(*profile));
    return false;
}
void profile_resolve(const Settings *global, const GameProfile *profile, Settings *effective)
{
    (void)profile;
    *effective = *global;
}
void panel_row(int row, const char *label, const char *value, bool selected)
{
    classic_rows++;
    assert(row >= 0 && row < 7);
    (void)label;
    (void)value;
    (void)selected;
}
void panel_note(float y, const char *s)
{
    (void)y;
    if (!strcmp(s, failure))
        str_copy(rendered_error, sizeof(rendered_error), s);
}
bool saves_busy(void)
{
    return service.state == SAVES_WORKING;
}
void saves_cancel(void)
{
    service.state = SAVES_CANCELLED;
}
void saves_poll(SavesStatus *out)
{
    *out = service;
}
bool saves_start(SavesOperation op, const SaveRequest *request, const char *snapshot,
                 bool confirmed)
{
    (void)request;
    assert(op < 8);
    if (op == SAVES_DETAILS || op == SAVES_RESTORE || op == SAVES_DELETE)
        assert(snapshot && !strcmp(snapshot, "fixture-snapshot"));
    if (op == SAVES_RESTORE || op == SAVES_DELETE)
        assert(confirmed);
    starts[op]++;
    service = (SavesStatus){.serial = ++serial, .operation = op, .state = first_state};
    /* The producer has already completed before the UI's very first poll. */
    if (first_state == SAVES_ERROR)
        str_copy(service.result.error, sizeof(service.result.error), failure);
    if (op == SAVES_LIST)
    {
        service.count = 1;
        str_copy(service.list[0].snapshot, sizeof(service.list[0].snapshot), "fixture-snapshot");
        service.list[0].kind = SAVE_MANUAL;
    }
    if (op == SAVES_DETAILS)
    {
        service.detail.kind = SAVE_MANUAL;
        service.layout.count = 1;
        service.layout.entries[0].kind = DR_SAVE_RPG_SLOTS;
        str_copy(service.layout.entries[0].path, sizeof(service.layout.entries[0].path),
                 "/fixture/native");
    }
    return true;
}
static size_t list_total = 1; /* snapshots the fake service holds */
bool saves_start_page(const char *id, size_t offset)
{
    assert(id && !strcmp(id, "fixture-game"));
    assert(offset < list_total || !offset);
    bool ok = saves_start(SAVES_LIST, NULL, NULL, false);
    service.count = list_total;
    service.offset = offset;
    for (size_t i = 0; i < SAVE_LIST_MAX && offset + i < list_total; i++)
    {
        str_copy(service.list[i].snapshot, sizeof(service.list[i].snapshot), "fixture-snapshot");
        service.list[i].kind = SAVE_MANUAL;
    }
    return ok;
}
static void setup(void)
{
    str_copy(fixture.id, sizeof(fixture.id), "fixture-game");
    str_copy(fixture.title, sizeof(fixture.title), "Fixture game");
    app.library.games = &fixture;
    app.library.count = 1;
    backup_panel_open(0);
    backup_panel_screen(0);
}
static void idle(void)
{
    for (int i = 0; i < 4; i++)
        backup_panel_screen(0);
}
static void details(uint32_t action)
{
    backup_panel_screen(story_mode ? BIT(BTN_RIGHT) : BIT(BTN_DOWN)); /* Story's slots run left to right */
    app.pads[0].buttons = action;
    backup_panel_screen(action);
    backup_panel_screen(0);
}
static void check_details(void)
{
    setup();
    details(BIT(BTN_CROSS));
    assert(backup_panel_confirming());
    assert(!story_mode || (last_view.confirmation && !last_view.confirm_armed && !last_view.deleting));
    assert(clears == 1);
    assert(!notices);
    idle();
    assert(clears == 1);
    backup_panel_screen(BIT(BTN_CROSS)); /* still held: not armed */
    assert(!starts[SAVES_RESTORE]);
    assert(!story_mode || !last_view.confirm_armed);
    app.pads[0].buttons = 0;
    backup_panel_screen(0);
    backup_panel_screen(BIT(BTN_CIRCLE));
    idle();
    assert(!backup_panel_confirming());
    assert(starts[SAVES_LIST] == 2);
    details(BIT(BTN_CROSS));
    assert(backup_panel_confirming());
    assert(clears == 2); /* new serial consumed */
}
static void check_mutation(SavesOperation op)
{
    setup();
    if (op == SAVES_CREATE)
        backup_panel_screen(BIT(BTN_CROSS));
    else
    {
        details(op == SAVES_DELETE ? BIT(BTN_SQUARE) : BIT(BTN_CROSS));
        assert(backup_panel_confirming());
        app.pads[0].buttons = 0;
        backup_panel_screen(0);
        if (story_mode)
        {
            /* Story: a Cross in the question's first moments (the press that would skip its text) answers nothing */
            backup_panel_screen(BIT(BTN_CROSS));
            assert(backup_panel_confirming() && !starts[op]);
            for (int i = 0; i < 30; i++)
                backup_panel_screen(0);
        }
        backup_panel_screen(BIT(BTN_CROSS));
    }
    backup_panel_screen(0);
    assert(starts[op] == 1);
    assert(starts[SAVES_LIST] == 2);
    assert(notices == 1);
    assert(!strcmp(last_notice, op == SAVES_CREATE   ? "Manual backup created"
                                : op == SAVES_DELETE ? "Local snapshot deleted"
                                                     : "Restore complete; recovery copy retained"));
    idle();
    assert(starts[SAVES_LIST] == 2 && notices == 1);
    assert(!story_mode || (last_view.list_ok && last_slots == STORY_SLOTS_LISTED));
}
/* A restore that fails: its error, no notice, and in Story no slots drawn as if there were no backups. */
static void check_restore_error(void)
{
    setup();
    assert(!story_mode || (last_view.list_ok && last_slots == STORY_SLOTS_LISTED));
    details(BIT(BTN_CROSS));
    assert(backup_panel_confirming());
    app.pads[0].buttons = 0;
    for (int i = 0; i < 31; i++)
        backup_panel_screen(0);
    first_state = SAVES_ERROR;
    backup_panel_screen(BIT(BTN_CROSS));
    backup_panel_screen(0);
    assert(starts[SAVES_RESTORE] == 1 && !backup_panel_confirming());
    assert(!strcmp(rendered_error, failure) && !notices);
    idle();
    assert(starts[SAVES_LIST] == 1);
    assert(!story_mode || (!last_view.list_ok && last_slots == STORY_SLOTS_FAILED && last_view.listed == 0));
}
/* More than one page: on the second (R1), the second snapshot there is the 130th; the confirmation, whose
 * status is the details read (offset 0), must still be told which page it is on. */
static void check_second_page(void)
{
    list_total = SAVE_LIST_MAX + 2;
    setup();
    backup_panel_screen(BIT(BTN_R1));
    backup_panel_screen(0);
    assert(starts[SAVES_LIST] == 2);
    backup_panel_screen(story_mode ? BIT(BTN_RIGHT) : BIT(BTN_DOWN));
    details(BIT(BTN_CROSS));
    assert(backup_panel_confirming());
    assert(!story_mode || (last_view.confirmation && last_view.status->offset == 0 &&
                           last_view.page_offset + (size_t)last_view.row == SAVE_LIST_MAX + 2));
}
static void check_error(bool listing)
{
    if (listing)
        first_state = SAVES_ERROR;
    setup();
    if (!listing)
    {
        first_state = SAVES_ERROR;
        details(BIT(BTN_CROSS));
    }
    assert(!backup_panel_confirming());
    assert(!strcmp(rendered_error, failure));
    assert(!notices);
    idle();
    assert(starts[SAVES_LIST] == 1 && !notices && !clears);
    assert(!story_mode || (!last_view.list_ok && last_slots == STORY_SLOTS_FAILED));
    /* A later foreign serial must not be consumed or replace the panel's error. */
    service.serial++;
    service.state = SAVES_DONE;
    service.operation = SAVES_CREATE;
    idle();
    assert(starts[SAVES_LIST] == 1 && !notices && !clears);
}
int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    story_mode = argc == 3 && !strcmp(argv[2], "story");
    app.settings.library_view = story_mode ? LIBRARY_VIEW_STORY : LIBRARY_VIEW_SHELF;
    app.dt = 1.0f / 60.0f;
    if (!strcmp(argv[1], "details"))
        check_details();
    else if (!strcmp(argv[1], "create"))
        check_mutation(SAVES_CREATE);
    else if (!strcmp(argv[1], "restore"))
        check_mutation(SAVES_RESTORE);
    else if (!strcmp(argv[1], "delete"))
        check_mutation(SAVES_DELETE);
    else if (!strcmp(argv[1], "second-page"))
        check_second_page();
    else if (!strcmp(argv[1], "restore-error"))
        check_restore_error();
    else if (!strcmp(argv[1], "list-error"))
        check_error(true);
    else if (!strcmp(argv[1], "details-error"))
        check_error(false);
    else
        assert(false);
    /* one presentation or the other, never both; the list result reaches Story's status screen */
    assert(story_mode ? story_draws && !classic_rows : !story_draws && classic_rows);
    assert(!strcmp(argv[1], "list-error") || noted_lists);
    printf("PASS production backup panel (%s view): first-poll terminal %s, matching serial consumed once\n",
           story_mode ? "story" : "shelf", argv[1]);
}
