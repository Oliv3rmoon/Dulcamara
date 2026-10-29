/*
 * Nightshade Player - interface language. This first release is in English: tr() hands the English text back, and
 * the font atlases need nothing beyond Latin-1. The API is SwanStationPS5's, so translations can come back as a
 * table like its i18n.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "i18n.h"

#include <stddef.h>

static int current = LANG_EN;

const char *tr(const char *english)
{
    return english;
}

void i18n_set(int lang)
{
    current = lang >= 0 && lang < LANG_COUNT ? LANG_EN : LANG_EN;
}

int i18n_get(void)
{
    return current;
}

const char *i18n_name(int lang)
{
    (void)lang;
    return "English";
}

const char *i18n_string(int lang, int index)
{
    (void)lang;
    (void)index;
    return NULL; /* no characters beyond Latin-1 to pack */
}
