/* Stable owned identity while a controller overlay holds library publication. GPL-3.0-or-later */
#include "panels.h"
#include "controller.h"
#include "eclipse.h"
#include "draw.h"
#include "text.h"
#include "theme.h"
#include "story.h"
#include "../app.h"
#include "../config.h"
#include "../core/cores.h"
#include "../covers.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static enum
{
    DETAILS,
    FILTERS,
    SEARCH
} page;
static Game game;
static int game_index;
static enum Screen details_return;
static UiList list;
static UiEntry entry;
static float reveal, rise;
void panel_begin(const char *title, const char *subtitle)
{
    if (app.game >= 0)
        app_draw_game(90);
    else
        shelf_backdrop();
    static int last_screen = -1;
    if (last_screen != (int)app.screen)
    {
        reveal = 0;
        last_screen = app.screen;
    }
    anim_approach(&reveal, 1, app.dt, 14);
    rise = (1 - reveal) * 18;
    draw_rrect(160, 130 + rise, 1600, 800, 28, 0xfa101015);
    text_draw_fit(215, 177 + rise, 38, FONT_BOLD, TH_TEXT, ALIGN_LEFT, 1490, title);
    text_draw_fit(215, 233 + rise, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, 1490, subtitle);
    eclipse_strip(215, 279 + rise, 1490, 10);
}
void panel_row(int row, const char *label, const char *value, bool selected)
{
    float y = 311 + row * 64;
    if (selected)
        draw_rrect(207, y - 5, 1505, 60, 12, TH_ROW_SELECTED);
    text_draw_fit(235, y + 9, 26, selected ? FONT_BOLD : FONT_REGULAR, TH_TEXT, ALIGN_LEFT,
                  value ? 800 : 1435, label);
    if (value)
        text_draw_fit(1680, y + 11, 24, FONT_REGULAR, selected ? TH_TEXT : TH_TEXT_DIM, ALIGN_RIGHT,
                      590, value);
}
void panel_note(float y, const char *s)
{
    text_draw_fit(235, y, 23, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, 1450, s);
}
static void opened(void)
{
    list = (UiList){0};
    reveal = 0;
    app.screen = SCREEN_DETAILS;
}
void panels_search(void)
{
    opened();
    page = SEARCH;
    ui_entry_open(&entry, app.library_filter.search);
}
void panels_filters(void)
{
    opened();
    page = FILTERS;
}
void panels_details(int i)
{
    if (i < 0 || i >= app.library.count)
        return;
    opened();
    page = DETAILS;
    game_index = i;
    game = app.library.games[i];
    details_return = app.game >= 0 ? SCREEN_MENU : SCREEN_LIBRARY;
}
/* Favourite on or off for a game, written before it is shown; on failure nothing changes and the reason is said.
 * The details panel and Story mode's Favorite both come here. */
bool panels_toggle_favorite(const char *id)
{
    const GameUserData *data = user_metadata_find(&app.metadata, id);
    const bool favorite = data && data->favorite;
    if (!user_metadata_set_favorite(&app.metadata, id, !favorite) ||
        !user_metadata_save(&app.metadata, app.paths.metadata))
    {
        user_metadata_set_favorite(&app.metadata, id, favorite);
        app_message_box("Favorite could not be saved. Check the data drive's free space "
                        "and permissions.");
        return false;
    }
    app_rebuild_library_view();
    return true;
}
static void move(uint32_t p, int count, int visible)
{
    ui_list_move(&list, (p & BIT(BTN_DOWN)) ? 1 : (p & BIT(BTN_UP)) ? -1 : 0, count, visible);
}
static void search_screen(uint32_t p)
{
    if (p & BIT(BTN_R1))
        ui_entry_clear(&entry);
    for (int b = 0; b < 32; b++)
        if (p & BIT(b))
        {
            UiEditResult r = ui_entry_button(&entry, b);
            if (r == UI_ACCEPTED)
            {
                str_copy(app.library_filter.search, sizeof(app.library_filter.search), entry.value);
                app_rebuild_library_view();
                shelf_open();
                return;
            }
            if (r == UI_CANCELLED)
            {
                shelf_open();
                return;
            }
        }
    panel_begin("Search library",
                "Search by game title or folder. Options finishes; Circle cancels.");
    draw_rrect(235, 309, 1450, 70, 12, 0xff15151b);
    text_draw_fit(265, 325, 30, FONT_REGULAR, TH_TEXT, ALIGN_LEFT, 1390,
                  entry.value[0] ? entry.value : "Type a title...");
    for (int r = 0; r < KB_ROWS; r++)
        for (int col = 0; col < KB_COLUMNS;)
        {
            KbKey k = kb_key(&entry.keys, r, col);
            col = k.column + k.span;
            float x = 235 + k.column * 132, y = 415 + r * 76, w = k.span * 132 - 8;
            KbKey selected = kb_key(&entry.keys, entry.keys.row, entry.keys.column);
            bool on = r == entry.keys.row && k.column == selected.column;
            draw_rrect(x, y, w, 66, 12, on ? TH_FOCUS : 0xff18181d);
            char c[2] = {(char)k.character, 0};
            const char *label = k.kind == KB_SHIFT    ? (entry.keys.shift ? "CAPS" : "Shift")
                                : k.kind == KB_DELETE ? "Delete"
                                : k.kind == KB_DONE   ? "Done"
                                : k.kind == KB_SPACE  ? "Space"
                                                      : c;
            text_draw(x + w / 2, y + 16, 27, FONT_BOLD, TH_TEXT, ALIGN_CENTER, label);
        }
    panel_note(840, "R1 Clear   Square Delete   Triangle Space   L1/L2 Shift");
    int g[] = {GLYPH_CROSS, GLYPH_START, GLYPH_CIRCLE};
    const char *l[] = {"Type", "Done", "Cancel"};
    app_draw_hints(g, l, 3, NULL);
}
static void filters_screen(uint32_t p)
{
    if (p & BIT(BTN_CIRCLE))
    {
        shelf_open();
        return;
    }
    move(p, 5, 5);
    if (p & (BIT(BTN_CROSS) | BIT(BTN_LEFT) | BIT(BTN_RIGHT)))
    {
        if (list.row == 0)
            app.library_filter.favorites_only = !app.library_filter.favorites_only;
        if (list.row == 1)
            app.library_filter.recent_only = !app.library_filter.recent_only;
        if (list.row == 2)
            app.library_filter.section =
                (app.library_filter.section + ((p & BIT(BTN_LEFT)) ? 3 : 2)) % 3 - 1;
        if (list.row == 3)
            app.library_filter.search[0] = 0;
        if (list.row == 4)
        {
            memset(&app.library_filter, 0, sizeof(app.library_filter));
            app.library_filter.section = -1;
        }
        app_rebuild_library_view();
    }
    panel_begin("Library filters",
                "Filters retain the selected game ID, including when it is hidden.");
    const char *labels[] = {"Favorites only", "Recently played only", "Engine family",
                            "Clear search", "Reset all filters"};
    const char *values[] = {app.library_filter.favorites_only ? "On" : "Off",
                            app.library_filter.recent_only ? "On" : "Off",
                            app.library_filter.section < 0    ? "All engines"
                            : app.library_filter.section == 0 ? "RPG Maker"
                                                              : "Ren'Py",
                            app.library_filter.search, NULL};
    for (int r = 0; r < 5; r++)
        panel_row(r, labels[r], values[r], r == list.row);
    char results[96];
    snprintf(results, sizeof(results), "%d matching games. Circle returns to the shelf.",
             app.library_view.count);
    panel_note(800, results);
    int g[] = {GLYPH_CROSS, GLYPH_CIRCLE};
    const char *l[] = {"Change", "Back"};
    app_draw_hints(g, l, 2, NULL);
}
static void details_screen(uint32_t p)
{
    if (p & BIT(BTN_CIRCLE))
    {
        if (details_return == SCREEN_MENU)
            menu_open();
        else
            shelf_open();
        return;
    }
    bool renpy = core_is_renpy(game.core);
    if (story_view())
    {
        /* Story's status screen offers these actions as choices: Left/Right move through them as Up/Down do */
        if (p & BIT(BTN_RIGHT))
            p |= BIT(BTN_DOWN);
        if (p & BIT(BTN_LEFT))
            p |= BIT(BTN_UP);
    }
    move(p, renpy ? 6 : 4, 6);
    const GameUserData *data = user_metadata_find(&app.metadata, game.id);
    bool favorite = data && data->favorite;
    if (p & BIT(BTN_CROSS))
    {
        switch (list.row)
        {
        case 0:
            if (app.game == game_index)
                app.screen = SCREEN_GAME;
            else
                app_start_game(game_index);
            return;
        case 1:
            if (panels_toggle_favorite(game.id))
                favorite = !favorite;
            break;
        case 2:
            preferences_open(game_index);
            return;
        case 3:
            backup_panel_open(game_index);
            return;
        case 4:
            keys_open(game_index, SCREEN_DETAILS);
            return;
        case 5:
            sounds_open(game_index, SCREEN_DETAILS);
            return;
        }
    }
    int version = game.engine ? game.engine : covers_engine(game_index);
    char meta[256], when[64] = "Never";
    if (game.played)
    {
        time_t t = (time_t)game.played;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    }
    data = user_metadata_find(&app.metadata, game.id);
    char version_text[48];
    if (version)
        snprintf(version_text, sizeof(version_text), "%s %d", renpy ? "Ren'Py" : "RPG Maker",
                 version);
    else
        snprintf(version_text, sizeof(version_text), "%s (version unknown)",
                 renpy ? "Ren'Py" : "RPG Maker");
    snprintf(meta, sizeof(meta), "%s  |  Last played: %s  |  Active playtime: %llu min",
             version_text, when, (unsigned long long)(data ? data->playtime_seconds / 60 : 0));
    /* Long native paths are horizontally scrolled by L1/R1, at UTF-8 boundaries. */
    static int path_page;
    if (p & BIT(BTN_R1))
        path_page++;
    if (p & BIT(BTN_L1))
        path_page--;
    int len = (int)strlen(game.location), offset = path_page * 100;
    if (offset >= len || offset < 0)
    {
        path_page = 0;
        offset = 0;
    }
    while (offset > 0 && ((unsigned char)game.location[offset] & 0xc0) == 0x80)
        offset--;
    if (story_view())
    {
        static const char *const story_labels[] = {"Play", "Favorite", "Preferences", "Backups", "Keys", "Sounds"};
        const char *labels[6];
        memcpy(labels, story_labels, sizeof(labels));
        if (app.game == game_index)
            labels[0] = "Resume";
        if (favorite)
            labels[1] = "Unfavorite";
        const StoryStatus view = {&game,   game_index, list.row, renpy ? 6 : 4,
                                  labels,  favorite,   data ? data->playtime_seconds : 0,
                                  version_text, game.location + offset, len > 100};
        story_draw_status(&view);
        return;
    }
    panel_begin(game.title, meta);
    const char *labels[] = {app.game == game_index ? "Resume" : "Play",
                            favorite ? "Remove favorite" : "Add favorite",
                            "Game preferences",
                            "Local backups",
                            "Keys for this game",
                            "Sounds for this game"};
    for (int r = 0; r < (renpy ? 6 : 4); r++)
    {
        float y = 311 + r * 64;
        if (r == list.row)
            draw_rrect(207, y - 5, 755, 60, 12, TH_ROW_SELECTED);
        text_draw_fit(235, y + 9, 26, r == list.row ? FONT_BOLD : FONT_REGULAR, TH_TEXT, ALIGN_LEFT,
                      695, labels[r]);
    }
    PlatTexture *cover = covers_get(game_index);
    if (cover)
        plat_draw_texture(cover, 1100, 320, 540, 405, 0xffffffff, true);
    else
        eclipse_cover(1100, 320, 540, 405, 1, true);
    eclipse_cover(1100, 320, 540, 405, 1, false);
    panel_note(765, "Location (L1 / R1 scroll):");
    panel_note(803, game.location + offset);
    int g[] = {GLYPH_CROSS, GLYPH_CIRCLE};
    const char *l[] = {"Select", "Back"};
    app_draw_hints(g, l, 2, NULL);
}
void panels_screen(uint32_t p)
{
    if (page == SEARCH)
        search_screen(p);
    else if (page == FILTERS)
        filters_screen(p);
    else
        details_screen(p);
}
