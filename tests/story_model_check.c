/* Story mode's model: typing never delays input, honest lines from real data, wrapping, tint fallback. */
#include "ui/story_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* every byte one unit wide, as a fixed-width face */
static float measure(const char *text, size_t bytes, void *context)
{
    (void)context;
    float w = 0;
    for (size_t i = 0; i < bytes; ++i)
        w += ((unsigned char)text[i] & 0xc0) != 0x80; /* one per character */
    return w;
}

int main(void)
{
    /* typing: time-based, no queue; finishing is immediate; still shows whole lines */
    StoryLine line = {0};
    story_line_say(&line, "Hello there", false);
    assert(!story_line_done(&line) && story_line_visible_bytes(&line) == 0);
    story_line_tick(&line, 0.05f, false); /* 5.5 characters */
    assert(story_line_visible_bytes(&line) == 5);
    story_line_finish(&line);
    assert(story_line_done(&line) && story_line_visible_bytes(&line) == strlen("Hello there"));
    /* holding Down through a long list: each new game replaces the line, nothing is queued behind it */
    for (int i = 0; i < 1000; ++i)
    {
        char text[64];
        snprintf(text, sizeof(text), "Game %04d. You have not started this one yet.", i);
        story_line_say(&line, text, false);
        story_line_tick(&line, 1.0f / 60, false);
    }
    assert(!strncmp(line.text, "Game 0999", 9) && line.shown < 3.0f);
    story_line_finish(&line);
    assert(story_line_done(&line));
    /* saying the same line again keeps its progress (a redraw is not a new line) */
    story_line_say(&line, line.text, false);
    assert(story_line_done(&line));
    /* reduced motion: whole lines at once */
    story_line_say(&line, "Still line", true);
    assert(story_line_done(&line));
    story_line_say(&line, "Moving line", false);
    story_line_tick(&line, 0.001f, true);
    assert(story_line_done(&line));
    /* UTF-8: typed prefix stays on a character boundary */
    story_line_say(&line, "月の旅 日本語", false);
    story_line_tick(&line, 2.5f / STORY_TYPE_RATE, false);
    assert(story_line_visible_bytes(&line) == 6); /* two 3-byte characters */
    assert(line.total == 7);

    /* choices wrap both ways */
    assert(story_choice_move(0, -1, 4) == 3 && story_choice_move(3, 1, 4) == 0 && story_choice_move(1, 1, 4) == 2);
    assert(story_choice_move(0, 1, 0) == 0);

    char out[640], kicker[120];
    /* levels: hours played, New when never played */
    story_level(out, sizeof(out), 0, 0);
    assert(!strcmp(out, "New"));
    story_level(out, sizeof(out), 0, 1700000000);
    assert(!strcmp(out, "Lv 1"));
    story_level(out, sizeof(out), 40 * 60, 1700000000);
    assert(!strcmp(out, "Lv 1"));
    story_level(out, sizeof(out), 3 * 3600 + 12 * 60, 1700000000);
    assert(!strcmp(out, "Lv 3"));
    story_level(out, sizeof(out), 9 * 3600 + 3 * 60, 1700000000);
    assert(!strcmp(out, "Lv 9"));
    story_duration(out, sizeof(out), 3 * 3600 + 12 * 60 + 59);
    assert(!strcmp(out, "3 h 12 m"));
    story_duration(out, sizeof(out), 40 * 60);
    assert(!strcmp(out, "40 m"));
    story_duration(out, sizeof(out), 20);
    assert(!strcmp(out, "under a minute"));

    /* relative days (TZ=UTC in the harness): 2026-11-10 12:00 UTC */
    const long long now = 1794312000;
    story_when(out, sizeof(out), now - 3600, now);
    assert(!strcmp(out, "today"));
    story_when(out, sizeof(out), now - 86400, now);
    assert(!strcmp(out, "yesterday"));
    story_when(out, sizeof(out), now - 3 * 86400, now);
    assert(!strcmp(out, "3 days ago"));
    story_when(out, sizeof(out), now - 30 * 86400, now);
    assert(!strcmp(out, "on 11 Oct 2026"));

    /* home lines say only what the data holds */
    story_home_line(out, sizeof(out), "RPG Maker 2000 Test suite", 3 * 3600 + 12 * 60, now - 86400, now);
    assert(!strcmp(out, "\"RPG Maker 2000 Test suite.\" You are 3 h 12 m in, and you last played yesterday. "
                        "What will you do?"));
    story_home_line(out, sizeof(out), "Fantasy Opposite", 0, 0, now);
    assert(!strcmp(out, "\"Fantasy Opposite.\" You have not started this one yet. What will you do?"));
    story_home_line(out, sizeof(out), "Old", 0, now - 3 * 86400, now);
    assert(!strcmp(out, "\"Old.\" You last played it 3 days ago. What will you do?"));
    story_home_line(out, sizeof(out), "Counted", 600, 0, now);
    assert(!strcmp(out, "\"Counted.\" You are 10 m in. What will you do?"));

    char where[200];
    story_chapter_lines(kicker, sizeof(kicker), where, sizeof(where), "RPG Maker 2000", 3 * 3600 + 12 * 60,
                        now - 86400, now);
    assert(!strcmp(kicker, "RPG MAKER 2000 \xC2\xB7 3 H 12 M PLAYED"));
    assert(!strcmp(where, "You left off 3 h 12 m in. Last played yesterday."));
    story_chapter_lines(kicker, sizeof(kicker), where, sizeof(where), "Ren'Py 8", 0, 0, now);
    assert(!strcmp(kicker, "REN'PY 8 \xC2\xB7 FIRST TIME"));
    assert(!strcmp(where, "You have not played this one yet."));

    /* not shown: count, first reasons by folder name, and the log */
    LibrarySkip skipped[3] = {
        {"/data/dulcamara/games/Lust Doll Plus",
         "an RPG Maker MV/MZ game: Dulcamara plays RPG Maker 2000/2003 and Ren'Py games"},
        {"/data/dulcamara/games/Series/Deep/Game", "nested too deep: move it up, at most two folders inside games/"},
        {"/data/dulcamara/games/c.zip", "still inside a .zip archive: extract it into a folder"}};
    story_not_shown_line(out, sizeof(out), skipped, 3, "/data/dulcamara/logs/dulcamara.log");
    assert(strstr(out, "3 folders were not shown."));
    assert(strstr(out, "\"Lust Doll Plus\" is an RPG Maker MV/MZ game: Dulcamara plays RPG Maker 2000/2003 and "
                       "Ren'Py games."));
    assert(strstr(out, "\"Game\": nested too deep"));
    assert(strstr(out, "/data/dulcamara/logs/dulcamara.log"));
    story_not_shown_line(out, sizeof(out), skipped, 1, "log");
    assert(strstr(out, "1 folder was not shown."));

    /* wrapping: lines fit, long words cut on characters, overflow reported */
    size_t starts[4], lengths[4];
    bool overflow;
    int n = story_wrap("one two three four five", 9, measure, NULL, starts, lengths, 4, &overflow);
    assert(n == 3 && !overflow);
    assert(lengths[0] == 7 && !strncmp(&"one two three four five"[starts[1]], "three", lengths[1]));
    n = story_wrap("one two three four five", 9, measure, NULL, starts, lengths, 2, &overflow);
    assert(n == 2 && overflow);
    n = story_wrap("日本語日本語日本語", 4, measure, NULL, starts, lengths, 4, &overflow);
    assert(n == 3 && lengths[0] == 12 && !overflow); /* four 3-byte characters a line */
    for (int i = 0; i < n; ++i)
        assert(measure(&"日本語日本語日本語"[starts[i]], lengths[i], NULL) <= 4);
    n = story_wrap("", 10, measure, NULL, starts, lengths, 4, &overflow);
    assert(n == 0 && !overflow);

    /* titles and file names are plain text: every '{' marked, so no title becomes a {Glyph} or cuts the line */
    char plain[64];
    story_plain(plain, sizeof(plain), "Test {beta");
    assert(!strcmp(plain, "Test \x1b{beta"));
    story_plain(plain, sizeof(plain), "Double {{ open } {b} {Cross} back\\slash");
    assert(!strcmp(plain, "Double \x1b{\x1b{ open } \x1b{b} \x1b{Cross} back\\slash"));
    story_plain(plain, sizeof(plain), "an \x1b in data");
    assert(!strcmp(plain, "an ? in data"));
    story_plain(plain, 7, "ab{{{{{");
    assert(!strcmp(plain, "ab\x1b{\x1b{")); /* never half a marked brace at the end */
    /* typing: a cut never stops at a marked brace; it stops before an unfinished markup tag only */
    const char *typed = "\"Test \x1b{beta.\" Press {Square} now";
    for (size_t cut = 0; cut <= strlen(typed); ++cut)
    {
        const size_t at = story_visible_cut(typed, cut);
        const char *tag = strstr(typed, "{Square}");
        const size_t open = (size_t)(tag - typed), close = open + strlen("{Square}");
        assert(at == (cut > open && cut < close ? open : cut));
    }
    assert(story_visible_cut("Test \x1b{beta and more", 9) == 9); /* typed up to "{be" */
    assert(story_visible_cut("Close } brace", 13) == 13);
    assert(story_visible_cut("Open {", 6) == 5); /* our markup half typed: held back */
    /* a long word is never wrapped between the mark and its brace */
    n = story_wrap("\x1b{\x1b{\x1b{\x1b{", 3, measure, NULL, starts, lengths, 4, &overflow);
    for (int i = 0; i < n; ++i)
        assert("\x1b{\x1b{\x1b{\x1b{"[starts[i]] == '\x1b' && lengths[i] % 2 == 0);

    /* backup slots: two taken in the same minute read apart (seconds shown) */
    char first[48], second[48];
    story_slot_stamp(first, sizeof(first), (uint64_t)now * 1000000000u + 5000000u, false);
    story_slot_stamp(second, sizeof(second), (uint64_t)(now + 41) * 1000000000u, false);
    assert(!strcmp(first, "10 Nov \xC2\xB7 12:00:00") && !strcmp(second, "10 Nov \xC2\xB7 12:00:41"));
    story_slot_stamp(first, sizeof(first), (uint64_t)(now - 9 * 86400 + 7) * 1000000000u, true);
    assert(!strcmp(first, "1 Nov 2026 12:00:07"));

    /* the chapter card: only while the game the player chose starts (pre-play protection included); never for a
     * stop, asked for or the game's own exit, its protection afterwards, a failure or an unsafe stop */
    assert(story_chapter_card(true, false, false, false));   /* loading, or protecting before play */
    assert(!story_chapter_card(false, false, false, false)); /* no game */
    assert(!story_chapter_card(true, true, false, false));   /* stopping: asked for, or the host stopping by itself */
    assert(!story_chapter_card(true, false, true, false));   /* the game quit by itself: its post-stop backup */
    assert(!story_chapter_card(true, false, false, true));   /* save protection failed, or an unsafe stop */

    /* the backups screen: empty slots only after a list that succeeded; a failed list, restore or delete is never
     * "no backups" but its error and a way to list again; nothing listed yet and no error: still looking */
    assert(story_slots_state(true, NULL) == STORY_SLOTS_LISTED);
    assert(story_slots_state(true, "") == STORY_SLOTS_LISTED);
    assert(story_slots_state(true, "Wait for the current save operation to finish.") == STORY_SLOTS_LISTED);
    assert(story_slots_state(false, "Recovery snapshots are retained") == STORY_SLOTS_FAILED);
    assert(story_slots_state(false, NULL) == STORY_SLOTS_LOOKING);
    assert(story_slots_state(false, "") == STORY_SLOTS_LOOKING);

    /* tint: saturated bright covers lend their hue; dark or grey ones keep red */
    assert(story_tint(0) == STORY_RED);
    assert(story_tint(0xff101010) == STORY_RED);
    assert(story_tint(0xff808080) == STORY_RED);
    assert(story_tint(0xff202a30) == STORY_RED);
    uint32_t teal = story_tint(0xff60aab0);
    assert(teal != STORY_RED && (teal >> 24) == 0xff && (teal & 0xff) >= (teal >> 16 & 0xff));
    uint32_t blue = story_tint(0xff2050a0);
    assert(blue != STORY_RED && (blue & 0xff) > 180);
    puts("PASS story model: typing never queues or blocks, still shows whole lines, UTF-8 safe; honest lines from "
         "play time/last played; not-shown reasons and log; wrap with overflow; tint falls back to red");
    return 0;
}
