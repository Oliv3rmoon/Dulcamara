/* Story mode: the library drawn as an RPG menu with a visual-novel text box. It draws the same state the shelf and
 * the panels own (selection, details, backups, the launch transition) and maps the pad onto the same functions.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_UI_STORY_H
#define NP_UI_STORY_H
#include "../saves.h"
#include "story_model.h"

/* Story view is chosen (Settings > Library > View). */
bool story_view(void);

/* SCREEN_LIBRARY in Story view: Up/Down choose a game, Left/Right an action, Cross confirms, Circle goes back;
 * Square, Triangle, Options, L2, R2 and the touchpad as on the shelf. */
void story_screen(uint32_t pressed);

/* What the details panel (panels.c) owns, for the Story status screen. */
typedef struct
{
    const Game *game;
    int index;                  /* the snapshot's library index */
    int row, rows;              /* the panel's chosen action and how many there are */
    const char *const *labels;  /* the panel's actions, in its order */
    bool favorite;
    uint64_t playtime_seconds;
    const char *engine_text;    /* "RPG Maker 2003", "Ren'Py (version unknown)" */
    const char *location;       /* the location from the panel's L1/R1 page */
    bool location_paged;        /* the location is longer than one page */
} StoryStatus;
void story_draw_status(const StoryStatus *status);

/* What the backups panel (backup_panel.c) owns, for the Story save slots. */
typedef struct
{
    const Game *game;
    int index;
    const SavesStatus *status;
    int listed;                 /* snapshots listed on this page */
    int row, top;               /* the panel's list: row 0 is "Create manual backup" */
    bool available;             /* mutations allowed now */
    bool working;
    bool confirmation, deleting, confirm_armed;
    int root_row, root_column;
    const char *error;
    bool automatic_on;          /* this game's effective automatic-backup setting */
    bool list_ok;               /* status is a list that succeeded: only then do no slots mean no backups */
    size_t page_offset;         /* the list page's first snapshot (L1/R1, 128 a page): slot numbers start after it,
                                 * also while status is a details read */
} StoryBackups;
/* How many list cells Story shows at once (the panel's UiList visible count in Story view). */
#define STORY_SLOT_CELLS 6
void story_draw_backups(const StoryBackups *backups);
/* The panel's list result for a game, kept for the status screen's backup count (no listing of its own). */
void story_note_backups(const char *game_id, const SavesStatus *status);
/* What the status screen's Backups row says for a game: the count from the panel's last list, while no save job has
 * run since (play, a backup, a restore or a deletion may have changed it), else that Backups counts them. */
void story_backup_count_text(const char *game_id, char *out, size_t size);

/* What the text box shows of its line now (typed so far, as cut for drawing), its escapes removed: for the preview's
 * read-only probes. */
void story_visible_text(char *out, size_t size);

/* The launch transition's frame in Story view: the chapter card for the game starting (library index), over the
 * whole screen. label is the transition's own state line. */
void story_chapter_draw(int index, const char *label);

/* Shared with the shelf (shelf.c): the same actions in both views. */
bool shelf_invite(void);
void shelf_toggle_sort(void);
void shelf_square(void);
const char *shelf_square_label(void);
/* Shared with the details panel (panels.c): favourite on or off, saved before it is shown; false when it could
 * not be saved (the panel's message is shown and nothing changed). */
bool panels_toggle_favorite(const char *game_id);
/* The backups panel's word for a snapshot's moment ("Auto / clean stop"). */
const char *backup_moment(const SaveInfo *info);
#endif
