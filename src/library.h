/*
 * Dulcamara - the game library: what is in /data/dulcamara/games.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A game is a folder holding RPG_RT.ldb and RPG_RT.lmt (any case), straight in games/ or up to two folders down
 * (games/Series/Game, games/Windows/Series/Game), or a .zip or .easyrpg archive that holds them, at its root or in a
 * folder inside it. Its
 * title is RPG_RT.ini's GameTitle, else the folder's or archive's name; its cover is the picture of its title screen
 * (Title/ in the game, the one RPG_RT.ldb names).
 *
 * A Ren'Py game is a folder with game/ holding its scripts or archives (.rpy, .rpyc, .rpa), there or in a folder of
 * game/ (game/scripts/). Its title is its
 * launcher's name (<name>.exe or <name>.sh beside game/), else the folder's; its cover is game/gui/main_menu.png, else
 * game/gui/window_icon.png; its Ren'Py version comes from its renpy/ folder.
 */
#ifndef NP_LIBRARY_H
#define NP_LIBRARY_H

#include "nightshade.h"
#include "userdata.h"

typedef struct
{
    char title[160];              /* UTF-8 */
    char name[160];               /* the folder's or archive's own name (an archive's without its extension) */
    char location[NP_PATH_MAX];   /* the game's folder, or the archive */
    char inner[NP_PATH_MAX];      /* in an archive, the folder that holds the game ("" at its root, else "Dir/") */
    char path[NP_PATH_MAX];       /* what the core loads: the folder, or <archive>/<inner> */
    char id[24];                  /* a stable key for the game: its location's hash */
    bool archive;
    int engine;                   /* 2000, 2003; 0 until the cover is looked up (it reads RPG_RT.ldb). Ren'Py: its
                                     major version (8, 7, 6), 0 when the game does not say */
    int core;                     /* which core runs it (CORE_* in core/cores.h) */
    long long played;             /* when it was last played (seconds since 1970), 0 never */
} Game;

/* Something under the games folder that looks like a game but is not listed, and why (a scan's own report: the
 * scan writes each to the log, the shelf says how many there are). */
typedef struct
{
    char path[NP_PATH_MAX];
    char reason[128];
} LibrarySkip;

typedef struct
{
    Game *games;
    int count;
    int capacity;
    uint64_t generation; /* snapshot identity; assigned by the scan service */
    LibrarySkip *skipped; /* owned like games; a cached snapshot has none */
    int skipped_count;
    int skipped_capacity;
} Library;

/* The library's sections, in their order on the shelf: RPG Maker's games, then Ren'Py's (8's, 7's and 6's alike). */
enum
{
    SECTION_RPG_MAKER,
    SECTION_RENPY,
    SECTION_COUNT
};

/* A folder that holds RPG_RT.ldb and RPG_RT.lmt. */
bool library_dir_is_game(const char *dir);
/* An archive that holds a game: fills g (title, inner, path...) and returns true. */
bool library_read_archive(const char *archive, Game *g);
/* A game folder (RPG Maker or Ren'Py): fills g. */
bool library_read_folder(const char *dir, Game *g);

/* Every game in root (the games folder), in no particular order. */
void library_scan(Library *lib, const char *root);
void library_free(Library *lib);
/* Legacy detached-library sort: never use on an active app snapshot.
 * Section by section (SECTION_*), and in each, mode 0: by title; 1: the last played first, then by title. */
void library_sort(Library *lib, int mode);
/* The section a game is in. */
int library_section(const Game *g);
/* Where a section's games start in a sorted library (library_sort keeps each together), and how many there are. */
int library_section_start(const Library *lib, int section, int *count);

/* The picture of the game's title screen: a file path for a folder, the name of the entry inside an archive. Reads
 * RPG_RT.ldb for its name (and sets g->engine); without one, the only picture in Title/. False when there is none. */
bool library_title_image(Game *g, char *out, size_t size);

/* When each game was last played: one "id<TAB>seconds" a line in the file. */
void library_load_played(Library *lib, const char *file);
/* False leaves both disk and the in-memory played timestamp unchanged. */
bool library_mark_played(Library *lib, int index, const char *file);

/* Stable selection wins; a legacy positional cursor migrates once. Missing IDs are kept for absent drives. */
int library_restore_selection(const Library *lib, Settings *settings);
/* Call on explicit cursor changes and launch, then persist settings when appropriate. */
bool library_select(const Library *lib, Settings *settings, int index);

/* Cancellable traversal; replaces lib only on complete success. Callback runs on
 * the calling thread before each entry; false cancels. Empty is success, I/O
 * failure is not: error then names the entry that could not be read. Whatever
 * looks like a game but is not listed (another engine, nested too deep, still in
 * an archive) is in lib->skipped with its reason, and written to the log. The
 * legacy synchronous wrapper uses this same traversal. */
typedef bool (*LibraryScanProgress)(void *context, unsigned visited, unsigned found);
bool library_scan_checked(Library *lib, const char *root, LibraryScanProgress progress,
                          void *context, char *error, size_t error_size);
/* Validate descriptor structure without disk I/O; launch revalidation rereads the
 * actual folder/archive and verifies that its identity and core still agree. */
bool library_game_valid(const Game *game, const char *root);
bool library_revalidate(const Game *game);
bool library_cache_load(Library *lib, const char *root, const char *file);
bool library_cache_save(const Library *lib, const char *root, const char *file);

typedef struct LibraryScanner LibraryScanner;
enum LibraryScanState { LIBRARY_SCAN_IDLE, LIBRARY_SCAN_RUNNING, LIBRARY_SCAN_DONE,
                        LIBRARY_SCAN_CANCELLED, LIBRARY_SCAN_ERROR };
typedef struct {
    enum LibraryScanState state;
    uint64_t generation; /* request identity; Library.generation identifies each offered snapshot */
    unsigned visited, found;
    bool cached; /* the last offered snapshot came from the cache */
    char error[NP_PATH_MAX + 96]; /* names what could not be read, when it was one entry */
} LibraryScanStatus;
/* Create/destroy own one persistent SDL worker. Request/cancel/poll never wait
 * for filesystem work. Main thread owns these calls and all published libraries.
 * Destroy joins the worker and may wait for an in-flight filesystem operation. */
LibraryScanner *library_scanner_create(void);
bool library_scanner_request(LibraryScanner *scanner, const char *root, const char *cache);
void library_scanner_cancel(LibraryScanner *scanner);
/* A true result transfers a complete snapshot into an EMPTY result. False
 * leaves result untouched. Read status with result=NULL to defer publication
 * (e.g. while a game holds an active library index). */
bool library_scanner_poll(LibraryScanner *scanner, Library *result, LibraryScanStatus *status);
void library_scanner_destroy(LibraryScanner *scanner);

typedef struct {
    char search[160]; /* UTF-8; ASCII folds, all other characters match exactly */
    bool favorites_only, recent_only;
    int section; /* -1 all, otherwise SECTION_* */
} LibraryFilter;
typedef struct { int *indices; int count; uint64_t generation; } LibraryView;
/* Main thread only; sort/filter indexes, never Game storage. Failure preserves
 * the old view. Rebuild after snapshot publication or metadata/filter changes. */
bool library_view_build(LibraryView *view, const Library *lib, const UserMetadata *metadata,
                        const LibraryFilter *filter, int sort_mode);
void library_view_free(LibraryView *view);
/* Returns a view position, deterministic first-row fallback, -1 when empty.
 * A hidden/absent selected ID is retained. Explicit selection changes it. */
int library_view_selection(const LibraryView *view, const Library *lib, const Settings *settings);
/* Legacy last_game was a displayed sorted position. Migrate through the view
 * before converting last_game to the immutable snapshot's underlying index. */
int library_view_restore_selection(const LibraryView *view, const Library *lib, Settings *settings);
bool library_view_select(const LibraryView *view, const Library *lib, Settings *settings, int row);
int library_view_section_start(const LibraryView *view, const Library *lib, int section, int *count);
#endif
