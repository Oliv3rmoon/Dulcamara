/*
 * Dulcamara - the on-screen keyboard's keys (keyboard.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "keyboard.h"

/* the four rows of characters, without Shift and with it: letters, the digits and the signs a name or a short answer
 * may have */
static const char LOWER[KB_ROWS - 1][KB_COLUMNS + 1] = {"1234567890-", "qwertyuiop'", "asdfghjkl!?", "zxcvbnm,.:;"};
static const char UPPER[KB_ROWS - 1][KB_COLUMNS + 1] = {"!@#$%^&*()_", "QWERTYUIOP\"", "ASDFGHJKL!?", "ZXCVBNM,.:;"};

/* the bottom row: each key and the columns it covers */
static const struct
{
    KbKind kind;
    int column, span;
} BOTTOM[] = {{KB_SHIFT, 0, 2}, {KB_SPACE, 2, 5}, {KB_DELETE, 7, 2}, {KB_DONE, 9, 2}};

void kb_reset(Keyboard *k)
{
    k->row = 1;
    k->column = 0;
    k->shift = KB_SHIFT_OFF;
}

KbKey kb_key(const Keyboard *k, int row, int column)
{
    row = row < 0 ? 0 : row >= KB_ROWS ? KB_ROWS - 1 : row;
    column = column < 0 ? 0 : column >= KB_COLUMNS ? KB_COLUMNS - 1 : column;
    KbKey key = {KB_CHARACTER, column, 1, 0};
    if (row == KB_ROWS - 1)
    {
        for (size_t i = 0; i < sizeof(BOTTOM) / sizeof(BOTTOM[0]); ++i)
            if (column >= BOTTOM[i].column && column < BOTTOM[i].column + BOTTOM[i].span)
            {
                key.kind = BOTTOM[i].kind;
                key.column = BOTTOM[i].column;
                key.span = BOTTOM[i].span;
            }
        key.character = key.kind == KB_SPACE ? ' ' : 0;
        return key;
    }
    key.character = (unsigned char)(k->shift != KB_SHIFT_OFF ? UPPER : LOWER)[row][column];
    return key;
}

static void next_shift(Keyboard *k)
{
    k->shift = (k->shift + 1) % 3; /* off, the next character, locked, off */
}

KbAction kb_press(Keyboard *k, uint32_t *character)
{
    *character = 0;
    const KbKey key = kb_key(k, k->row, k->column);
    switch (key.kind)
    {
    case KB_CHARACTER:
        *character = key.character;
        if (k->shift == KB_SHIFT_ONCE)
            k->shift = KB_SHIFT_OFF;
        return KB_TYPE;
    case KB_SPACE:
        *character = ' ';
        return KB_TYPE;
    case KB_SHIFT:
        next_shift(k);
        return KB_NOTHING;
    case KB_DELETE:
        return KB_BACKSPACE;
    case KB_DONE:
        return KB_RETURN;
    }
    return KB_NOTHING;
}

KbAction kb_button(Keyboard *k, int button, uint32_t *character)
{
    *character = 0;
    /* across a row, from the key the highlight is on: a wide key is left from either side at once */
    const KbKey key = kb_key(k, k->row, k->column);
    switch (button)
    {
    case BTN_UP:
        k->row = (k->row + KB_ROWS - 1) % KB_ROWS;
        return KB_NOTHING;
    case BTN_DOWN:
        k->row = (k->row + 1) % KB_ROWS;
        return KB_NOTHING;
    case BTN_LEFT:
        k->column = key.column > 0 ? key.column - 1 : KB_COLUMNS - 1;
        return KB_NOTHING;
    case BTN_RIGHT:
        k->column = key.column + key.span < KB_COLUMNS ? key.column + key.span : 0;
        return KB_NOTHING;
    case BTN_CROSS:
        return kb_press(k, character);
    case BTN_SQUARE:
        return KB_BACKSPACE;
    case BTN_TRIANGLE:
        *character = ' ';
        return KB_TYPE;
    case BTN_L1:
    case BTN_L2:
        next_shift(k);
        return KB_NOTHING;
    case BTN_START:
        return KB_RETURN;
    case BTN_CIRCLE:
        return KB_HIDE;
    default:
        return KB_NOTHING;
    }
}

bool kb_point(Keyboard *k, float u, float v)
{
    if (!(u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f))
        return false;
    const int row = (int)(v * KB_ROWS), column = (int)(u * KB_COLUMNS);
    k->row = row < KB_ROWS ? row : KB_ROWS - 1;
    k->column = column < KB_COLUMNS ? column : KB_COLUMNS - 1;
    return true;
}
