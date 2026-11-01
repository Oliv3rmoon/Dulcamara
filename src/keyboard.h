/*
 * Dulcamara - the on-screen keyboard's keys: what is on it, where its highlight goes and what a press types. The game
 * screen shows it while a game wants text typed (a Ren'Py game's name entry), as the console's own keyboard cannot be
 * used; ui/keyboard.c draws it and gives the game what it types. Nothing here draws: tests/keyboard_check.c runs it
 * on the build machine.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   1 2 3 4 5 6 7 8 9 0 -        Shift: ! @ # $ % ^ & * ( ) _
 *   q w e r t y u i o p '               Q W E R T Y U I O P "
 *   a s d f g h j k l ! ?               A S D F G H J K L ! ?
 *   z x c v b n m , . : ;               Z X C V B N M , . : ;
 *   [Shift] [  Space  ] [Delete] [Done]
 */
#ifndef NP_KEYBOARD_H
#define NP_KEYBOARD_H

#include "nightshade.h"

#define KB_ROWS 5
#define KB_COLUMNS 11

typedef enum
{
    KB_CHARACTER, /* a letter, a digit or a sign */
    KB_SHIFT,
    KB_SPACE,
    KB_DELETE,
    KB_DONE,
} KbKind;

typedef struct
{
    KbKind kind;
    int column, span;   /* the columns it covers: the bottom row's keys are wider */
    uint32_t character; /* what it types, as Shift has it now (KB_CHARACTER, KB_SPACE); 0 for the others */
} KbKey;

enum
{
    KB_SHIFT_OFF,
    KB_SHIFT_ONCE,   /* the next character only */
    KB_SHIFT_LOCKED, /* until Shift is pressed again */
};

typedef struct
{
    int row, column; /* the highlight: a row, and a column in it (a wide key is highlighted from any of its) */
    int shift;       /* KB_SHIFT_* */
} Keyboard;

/* What a press does to the game. */
typedef enum
{
    KB_NOTHING,   /* the keyboard's own: the highlight moved, Shift changed */
    KB_TYPE,      /* a character typed */
    KB_BACKSPACE, /* the one before the caret deleted */
    KB_RETURN,    /* done: the game takes the text */
    KB_HIDE,      /* the keyboard put away, the controller back to the game */
} KbAction;

/* The highlight on q, Shift off. */
void kb_reset(Keyboard *k);
/* The key at row, column, any of the columns it covers. */
KbKey kb_key(const Keyboard *k, int row, int column);
/* A controller button pressed (BTN_*): the D-pad moves the highlight (round from one edge to the other), Cross
 * presses its key, Square deletes, Triangle types a space, L1 and L2 are Shift, Circle hides the keyboard, and
 * Options (BTN_START, for a short press: held, it is Dulcamara's menu) is Done. *character: what KB_TYPE types. */
KbAction kb_button(Keyboard *k, int button, uint32_t *character);
/* The highlighted key pressed (Cross, or a tap on the touchpad). */
KbAction kb_press(Keyboard *k, uint32_t *character);
/* The touchpad: the key at u, v (0 to 1 across the keys and down them) highlighted; false off the keys. */
bool kb_point(Keyboard *k, float u, float v);

#endif
