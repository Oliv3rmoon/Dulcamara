/* Story mode's words and timing, without drawing: what the text box says about a game, how far its line is typed
 * out, and how the choices move. Pure functions over app data, so the host tests run them as they are.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_UI_STORY_MODEL_H
#define NP_UI_STORY_MODEL_H
#include "../library.h"
#include <stddef.h>
#include <stdint.h>

/* Settings.library_view */
enum
{
    LIBRARY_VIEW_STORY = 0, /* the default */
    LIBRARY_VIEW_SHELF = 1, /* the cover row, kept as the fallback */
    LIBRARY_VIEW_COUNT
};

/* Characters a second the line is typed at. Cosmetic only: input never waits for it. */
#define STORY_TYPE_RATE 110.0f

/* The text box's line. Typing is a function of elapsed time, never a queue: saying a new line replaces the old one
 * at once, and finishing it is immediate. */
typedef struct
{
    char text[640];
    size_t total;  /* characters (not bytes) in text */
    float shown;   /* characters typed so far */
} StoryLine;

/* Starts a new line; with still (reduced motion) it appears whole. Saying the same text again keeps its progress. */
void story_line_say(StoryLine *line, const char *text, bool still);
void story_line_tick(StoryLine *line, float dt, bool still);
void story_line_finish(StoryLine *line);
bool story_line_done(const StoryLine *line);
/* Bytes of text typed so far, on a UTF-8 boundary. */
size_t story_line_visible_bytes(const StoryLine *line);

/* Left/Right through count choices, wrapping. */
int story_choice_move(int current, int delta, int count);

/* "Lv N" from hours played (at least Lv 1 once anything was played), or "New" when never played. */
void story_level(char *out, size_t size, uint64_t playtime_seconds, long long last_played);
/* "3 h 12 m", "40 m", "under a minute". */
void story_duration(char *out, size_t size, uint64_t seconds);
/* When, relative to now in local time: "today", "yesterday", "3 days ago", or "on 2 Nov 2026". */
void story_when(char *out, size_t size, long long when, long long now);
/* The home line for a game: from its play time and when it was last played; never more than the app knows. */
void story_home_line(char *out, size_t size, const char *title, uint64_t playtime_seconds, long long last_played,
                     long long now);
/* The chapter card's two lines: the kicker (engine and how much was played) and where the player left off. */
void story_chapter_lines(char *kicker, size_t kicker_size, char *where, size_t where_size, const char *engine,
                         uint64_t playtime_seconds, long long last_played, long long now);
/* A backup slot's time, in local time, to the second so that slots taken in the same minute read apart:
 * "10 Nov · 20:15:07", or with the year "10 Nov 2026 20:15:07". */
void story_slot_stamp(char *out, size_t size, uint64_t created_ns, bool year);
/* Whether the launch transition is drawn as the chapter card: only while the game the player chose starts (game: a
 * game is starting or in play). Never while it stops (asked for, or the host stopping by itself), during the
 * post-stop backup of a game that quit by itself, or after a protection failure or an unsafe stop: those keep the
 * transition's own frame. */
bool story_chapter_card(bool game, bool stopping, bool after_stop, bool failed);
/* What the "N not shown" row says: how many, the first reasons (folder name: reason), and where the rest are. */
void story_not_shown_line(char *out, size_t size, const LibrarySkip *skipped, int count, const char *log_path);

/* What the backups screen shows in place of its slots: the slots (and "empty" placeholders) only after a list that
 * succeeded; after a failed list, restore, delete or backup, the failure and a way to list again, never what reads
 * as "no backups"; otherwise (nothing listed yet, no error) that it is still looking. */
typedef enum
{
    STORY_SLOTS_LOOKING,
    STORY_SLOTS_LISTED,
    STORY_SLOTS_FAILED
} StorySlots;
StorySlots story_slots_state(bool list_ok, const char *error);

/* Data in a line (a title, a folder name, a path, an error) is plain text, never markup: story_plain copies it with
 * every '{' marked as text (TEXT_LITERAL before it, which the renderer draws as nothing), so no title can become a
 * {Glyph} or cut the line. */
#define TEXT_LITERAL '\x1b'
void story_plain(char *out, size_t size, const char *data);
/* How much of a line to draw when cut bytes of it are typed: never inside a {Glyph} tag (an unmarked '{' not yet
 * closed before cut); a marked '{' is text. */
size_t story_visible_cut(const char *text, size_t cut);

/* Word wrap: up to max_lines line starts into starts[] (byte offsets) and their lengths into lengths[], each line no
 * wider than max_width by measure(). A word wider than a line is cut at a UTF-8 boundary. Returns the number of
 * lines; *overflow is set when text remained after max_lines (the caller ends the last line with an ellipsis). */
typedef float (*StoryMeasure)(const char *text, size_t bytes, void *context);
int story_wrap(const char *text, float max_width, StoryMeasure measure, void *context, size_t *starts,
               size_t *lengths, int max_lines, bool *overflow);

/* The accent for a cover's average colour (covers_color): the cover's hue when it is bright and saturated enough to
 * read as a colour, else Eclipse red. Always opaque. */
uint32_t story_tint(uint32_t average_argb);
#define STORY_RED 0xffe5484du

#endif
