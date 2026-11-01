/*
 * Dulcamara - the on-screen keyboard's keys (src/keyboard.c), on the build machine: what is on it with Shift and
 * without, where the highlight goes from each edge and over the wide keys, Shift's three states, what each button,
 * each key and the touchpad do. tests/test_keyboard.py builds and runs it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/keyboard.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
    do                                                                                                             \
    {                                                                                                              \
        ++checks;                                                                                                  \
        if (!(cond))                                                                                               \
        {                                                                                                          \
            ++failures;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static KbKey here(const Keyboard *k)
{
    return kb_key(k, k->row, k->column);
}

/* the highlight moved by one button press, which must type nothing */
static void move(Keyboard *k, int button)
{
    uint32_t character = 1;
    const KbAction a = kb_button(k, button, &character);
    CHECK(a == KB_NOTHING && character == 0, "button %d moves only", button);
}

static void expect_character(const Keyboard *k, uint32_t c, const char *after)
{
    const KbKey key = here(k);
    CHECK(key.kind == KB_CHARACTER && key.character == c, "%s: on '%c', not '%c' (kind %d)", after, (char)c,
          (char)key.character, (int)key.kind);
}

static void expect_kind(const Keyboard *k, KbKind kind, const char *after)
{
    CHECK(here(k).kind == kind, "%s: on kind %d, not %d", after, (int)here(k).kind, (int)kind);
}

static void expect_type(Keyboard *k, int button, uint32_t c, const char *what)
{
    uint32_t character = 0;
    const KbAction a = kb_button(k, button, &character);
    CHECK(a == KB_TYPE && character == c, "%s: typed '%c' (action %d), not '%c'", what, (char)character, (int)a,
          (char)c);
}

static void check_rows(void)
{
    static const char *const LOWER[] = {"1234567890-", "qwertyuiop'", "asdfghjkl!?", "zxcvbnm,.:;"};
    static const char *const UPPER[] = {"!@#$%^&*()_", "QWERTYUIOP\"", "ASDFGHJKL!?", "ZXCVBNM,.:;"};
    Keyboard k;
    kb_reset(&k);
    for (int shift = KB_SHIFT_OFF; shift <= KB_SHIFT_LOCKED; ++shift)
    {
        k.shift = shift;
        const char *const *rows = shift == KB_SHIFT_OFF ? LOWER : UPPER;
        for (int row = 0; row < KB_ROWS - 1; ++row)
        {
            CHECK(strlen(rows[row]) == KB_COLUMNS, "row %d is %d keys", row, KB_COLUMNS);
            for (int column = 0; column < KB_COLUMNS; ++column)
            {
                const KbKey key = kb_key(&k, row, column);
                CHECK(key.kind == KB_CHARACTER && key.column == column && key.span == 1 &&
                          key.character == (unsigned char)rows[row][column],
                      "shift %d, row %d, column %d: '%c'", shift, row, column, (char)key.character);
            }
        }
    }
    /* the bottom row: Shift, Space, Delete and Done, each over the columns it covers */
    static const struct
    {
        KbKind kind;
        int column, span;
    } BOTTOM[] = {{KB_SHIFT, 0, 2}, {KB_SPACE, 2, 5}, {KB_DELETE, 7, 2}, {KB_DONE, 9, 2}};
    k.shift = KB_SHIFT_OFF;
    int covered = 0;
    for (size_t i = 0; i < sizeof(BOTTOM) / sizeof(BOTTOM[0]); ++i)
        for (int column = BOTTOM[i].column; column < BOTTOM[i].column + BOTTOM[i].span; ++column, ++covered)
        {
            const KbKey key = kb_key(&k, KB_ROWS - 1, column);
            CHECK(key.kind == BOTTOM[i].kind && key.column == BOTTOM[i].column && key.span == BOTTOM[i].span,
                  "column %d of the bottom row is key kind %d", column, (int)BOTTOM[i].kind);
            CHECK(key.character == (key.kind == KB_SPACE ? ' ' : 0u), "column %d: the character it types", column);
        }
    CHECK(covered == KB_COLUMNS, "the bottom row covers all %d columns", KB_COLUMNS);
}

static void check_moves(void)
{
    Keyboard k;
    kb_reset(&k);
    expect_character(&k, 'q', "the start");
    CHECK(k.shift == KB_SHIFT_OFF, "Shift starts off");
    /* round a row, both ways */
    move(&k, BTN_LEFT);
    expect_character(&k, '\'', "Left from q");
    move(&k, BTN_RIGHT);
    expect_character(&k, 'q', "Right from the row's end");
    for (int i = 0; i < KB_COLUMNS; ++i)
        move(&k, BTN_RIGHT);
    expect_character(&k, 'q', "Right across the row");
    /* round the rows: up from the top is the bottom row */
    move(&k, BTN_UP);
    expect_character(&k, '1', "Up from q");
    move(&k, BTN_UP);
    expect_kind(&k, KB_SHIFT, "Up from 1");
    move(&k, BTN_UP);
    expect_character(&k, 'z', "Up from Shift");
    move(&k, BTN_DOWN);
    move(&k, BTN_DOWN);
    expect_character(&k, '1', "Down from Shift");
    /* down onto the wide Space and back up to the same column */
    kb_reset(&k);
    for (int i = 0; i < 4; ++i)
        move(&k, BTN_RIGHT);
    expect_character(&k, 't', "four to the right of q");
    move(&k, BTN_DOWN);
    expect_character(&k, 'g', "Down from t");
    move(&k, BTN_DOWN);
    move(&k, BTN_DOWN);
    expect_kind(&k, KB_SPACE, "Down to the bottom row under t");
    move(&k, BTN_UP);
    expect_character(&k, 'b', "Up from Space, where it came down");
    /* across the bottom row a key at a time, and round it */
    move(&k, BTN_DOWN);
    move(&k, BTN_RIGHT);
    expect_kind(&k, KB_DELETE, "Right from Space");
    move(&k, BTN_RIGHT);
    expect_kind(&k, KB_DONE, "Right from Delete");
    move(&k, BTN_RIGHT);
    expect_kind(&k, KB_SHIFT, "Right from Done");
    move(&k, BTN_LEFT);
    expect_kind(&k, KB_DONE, "Left from Shift");
    move(&k, BTN_LEFT);
    move(&k, BTN_LEFT);
    expect_kind(&k, KB_SPACE, "Left twice from Done");
    move(&k, BTN_LEFT);
    expect_kind(&k, KB_SHIFT, "Left from Space");
    move(&k, BTN_UP);
    expect_character(&k, 'x', "Up from Shift's right half");
}

static void check_typing(void)
{
    Keyboard k;
    kb_reset(&k);
    expect_type(&k, BTN_CROSS, 'q', "Cross on q");
    /* Shift: the next character, then off */
    move(&k, BTN_L2);
    CHECK(k.shift == KB_SHIFT_ONCE, "L2: Shift for the next character");
    expect_type(&k, BTN_TRIANGLE, ' ', "Triangle");
    CHECK(k.shift == KB_SHIFT_ONCE, "a space leaves Shift on");
    expect_type(&k, BTN_CROSS, 'Q', "Cross on q with Shift");
    CHECK(k.shift == KB_SHIFT_OFF, "Shift off after a character");
    expect_type(&k, BTN_CROSS, 'q', "Cross on q again");
    /* twice: locked, until a third press */
    move(&k, BTN_L1);
    move(&k, BTN_L1);
    CHECK(k.shift == KB_SHIFT_LOCKED, "L1 twice: Shift locked");
    expect_type(&k, BTN_CROSS, 'Q', "Cross with Shift locked");
    expect_type(&k, BTN_CROSS, 'Q', "Cross again with Shift locked");
    move(&k, BTN_UP);
    move(&k, BTN_RIGHT);
    expect_type(&k, BTN_CROSS, '@', "Cross on 2 with Shift locked");
    move(&k, BTN_L2);
    CHECK(k.shift == KB_SHIFT_OFF, "a third Shift: off");
    expect_type(&k, BTN_CROSS, '2', "Cross on 2");
    /* the Shift key itself, from Cross */
    kb_reset(&k);
    move(&k, BTN_DOWN);
    move(&k, BTN_DOWN);
    move(&k, BTN_DOWN);
    expect_kind(&k, KB_SHIFT, "three down from q");
    move(&k, BTN_CROSS);
    CHECK(k.shift == KB_SHIFT_ONCE, "Cross on Shift");
    /* the other wide keys */
    uint32_t character = 1;
    move(&k, BTN_RIGHT);
    expect_type(&k, BTN_CROSS, ' ', "Cross on Space");
    move(&k, BTN_RIGHT);
    CHECK(kb_button(&k, BTN_CROSS, &character) == KB_BACKSPACE && character == 0, "Cross on Delete");
    move(&k, BTN_RIGHT);
    CHECK(kb_button(&k, BTN_CROSS, &character) == KB_RETURN && character == 0, "Cross on Done");
    /* the buttons of their own, wherever the highlight is */
    const Keyboard before = k;
    CHECK(kb_button(&k, BTN_SQUARE, &character) == KB_BACKSPACE, "Square deletes");
    CHECK(kb_button(&k, BTN_START, &character) == KB_RETURN, "Options is Done");
    CHECK(kb_button(&k, BTN_CIRCLE, &character) == KB_HIDE, "Circle hides the keyboard");
    CHECK(kb_button(&k, BTN_R1, &character) == KB_NOTHING && character == 0, "R1 does nothing");
    CHECK(kb_button(&k, BTN_R3, &character) == KB_NOTHING, "R3 does nothing");
    CHECK(k.row == before.row && k.column == before.column && k.shift == before.shift,
          "those leave the highlight and Shift as they were");
}

static void check_touch(void)
{
    Keyboard k;
    kb_reset(&k);
    CHECK(kb_point(&k, 0.0f, 0.0f), "the top left corner is a key");
    expect_character(&k, '1', "the top left corner");
    CHECK(kb_point(&k, 0.999f, 0.999f), "the bottom right corner is a key");
    expect_kind(&k, KB_DONE, "the bottom right corner");
    CHECK(kb_point(&k, 0.5f, 0.9f), "the middle of the bottom row is a key");
    expect_kind(&k, KB_SPACE, "the middle of the bottom row");
    CHECK(kb_point(&k, 4.5f / KB_COLUMNS, 2.5f / KB_ROWS), "g is a key");
    expect_character(&k, 'g', "the middle of g");
    const Keyboard before = k;
    CHECK(!kb_point(&k, -0.01f, 0.5f) && !kb_point(&k, 0.5f, 1.0f) && !kb_point(&k, 1.0f, 0.2f), "off the keys");
    CHECK(k.row == before.row && k.column == before.column, "a touch off the keys leaves the highlight");
    uint32_t character = 0;
    CHECK(kb_press(&k, &character) == KB_TYPE && character == 'g', "a tap on g types g");
}

int main(void)
{
    check_rows();
    check_moves();
    check_typing();
    check_touch();
    printf("%d checks, %d failed\n", checks, failures);
    printf(failures ? "FAIL\n" : "PASS\n");
    return failures ? 1 : 0;
}
