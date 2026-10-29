/*
 * Dulcamara - what the library reads from an RPG Maker 2000 / 2003 game without running it: its title
 * (RPG_RT.ini), the picture of its title screen and its engine (RPG_RT.ldb), and RPG Maker's own XYZ pictures.
 * Plain functions over bytes, so tests/library_check.c runs them on the build machine.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_RPGMAKER_H
#define NP_RPGMAKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The files that make a folder a game. */
#define RPGM_DATABASE "RPG_RT.ldb"
#define RPGM_MAP_TREE "RPG_RT.lmt"
#define RPGM_INI "RPG_RT.ini"

/* Case-insensitive ASCII name comparison: RPG_RT.ldb is rpg_rt.LDB on some copies. */
bool rpgm_name_is(const char *name, const char *want);

/* A Windows code page the game's text may be in: 932 Shift-JIS, 949 Korean, 936 / 950 Chinese, 1250 to 1253. */
/* The likeliest one for these bytes: 65001 when they are valid UTF-8 (or plain ASCII), else 932, 949, 1251 or 1252. */
int rpgm_guess_codepage(const char *text, size_t len);
/* Text in a game's encoding as UTF-8: codepage 0 guesses it. Bytes that don't convert become '?'. */
void rpgm_to_utf8(const char *text, size_t len, int codepage, char *out, size_t size);

/* RPG_RT.ini: [RPG_RT] GameTitle, as UTF-8 (the ini's [EasyRPG] Encoding when it names a code page, else guessed).
 * False when the ini has no title. */
bool rpgm_ini_title(const char *ini, size_t len, char *out, size_t size);

/* RPG_RT.ldb: the title screen's picture (System, title_name: its name in Title/ without extension, raw bytes in
 * the game's encoding) and the engine, 2000 or 2003 (a 2003 database has battle commands and classes). False when
 * this is not an RPG Maker database; title_name is empty when the database names no picture. */
bool rpgm_ldb_info(const uint8_t *ldb, size_t len, char *title_name, size_t size, int *engine);

/* An XYZ picture (RPG Maker 2000's own: "XYZ1", width, height, then a zlib stream of a 256-colour palette and one
 * byte a pixel) as RGBA; free() it. NULL when it isn't one. */
uint8_t *rpgm_decode_xyz(const uint8_t *data, size_t len, int *width, int *height);

#endif
