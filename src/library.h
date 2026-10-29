/*
 * Dulcamara - the game library: what is in /data/dulcamara/games.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A game is a folder holding RPG_RT.ldb and RPG_RT.lmt (any case), straight in games/ or one folder down
 * (games/Series/Game), or a .zip or .easyrpg archive that holds them, at its root or in a folder inside it. Its
 * title is RPG_RT.ini's GameTitle, else the folder's or archive's name; its cover is the picture of its title screen
 * (Title/ in the game, the one RPG_RT.ldb names).
 */
#ifndef NP_LIBRARY_H
#define NP_LIBRARY_H

#include "nightshade.h"

typedef struct
{
    char title[160];              /* UTF-8 */
    char name[160];               /* the folder's or archive's own name (an archive's without its extension) */
    char location[NP_PATH_MAX];   /* the game's folder, or the archive */
    char inner[NP_PATH_MAX];      /* in an archive, the folder that holds the game ("" at its root, else "Dir/") */
    char path[NP_PATH_MAX];       /* what the core loads: the folder, or <archive>/<inner> */
    char id[24];                  /* a stable key for the game: its location's hash */
    bool archive;
    int engine;                   /* 2000, 2003; 0 until the cover is looked up (it reads RPG_RT.ldb) */
    int core;                     /* which core runs it (CORE_* in core/cores.h) */
    long long played;             /* when it was last played (seconds since 1970), 0 never */
} Game;

typedef struct
{
    Game *games;
    int count;
    int capacity;
} Library;

/* A folder that holds RPG_RT.ldb and RPG_RT.lmt. */
bool library_dir_is_game(const char *dir);
/* An archive that holds a game: fills g (title, inner, path...) and returns true. */
bool library_read_archive(const char *archive, Game *g);
/* A game folder: fills g. */
bool library_read_folder(const char *dir, Game *g);

/* Every game in root (the games folder), in no particular order. */
void library_scan(Library *lib, const char *root);
void library_free(Library *lib);
/* mode 0: by title; 1: the last played first, then by title. */
void library_sort(Library *lib, int mode);

/* The picture of the game's title screen: a file path for a folder, the name of the entry inside an archive. Reads
 * RPG_RT.ldb for its name (and sets g->engine); without one, the only picture in Title/. False when there is none. */
bool library_title_image(Game *g, char *out, size_t size);

/* When each game was last played: one "id<TAB>seconds" a line in the file. */
void library_load_played(Library *lib, const char *file);
void library_mark_played(Library *lib, int index, const char *file);

#endif
