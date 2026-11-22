/*
 * Dulcamara - a game added from a direct link (docs/game-links.md): its ZIP downloaded into a hidden staging folder
 * beside the games, checked, unpacked there and renamed into games/ only when complete. Nothing here draws; the
 * Settings rows and the shelf's selection are ui/links_ui.c's. tests/game_links_check.c runs all of it on the host.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_LINKS_H
#define NP_LINKS_H

#include "library.h"
#include "nightshade.h"

#define LINKS_STAGING ".dulcamara-links" /* in games/: a name the library scan skips (it starts with '.') */
#define LINKS_FILE "links.txt"           /* in the data folder: the user's links, never rewritten */
#define LINKS_DONE_FILE "links-done.txt" /* in the data folder: the processed ones */

#define LINKS_URL_MAX 2048
#define LINKS_PER_RUN 64
#define LINKS_FILE_MAX (256 * 1024)
#define LINKS_MAX_REDIRECTS 10
#ifndef LINKS_STALL_SECONDS
#define LINKS_STALL_SECONDS 60 /* given up when under LINKS_STALL_BYTES a second for this long */
#endif
#define LINKS_STALL_BYTES 1024
#define LINKS_RESUMES 5 /* a transfer that stopped after receiving data goes on with a Range request */
#define LINKS_MAX_ENTRIES 65536u
#define LINKS_MAX_TOTAL (64ull << 30)
#define LINKS_RATIO_FLOOR (256ull << 20) /* the archive's overall ratio is checked above this unpacked size */
#define LINKS_MAX_RATIO 200u
#define LINKS_DEFLATE_RATIO 1032u /* Deflate's own ceiling, per entry */
#define LINKS_RESERVE (16ull << 20)
#define LINKS_ENTRY_OVERHEAD 4096u
#define LINKS_FOLDER_MAX 96
#define LINKS_SUFFIX_MAX 99 /* Name, Name (2) ... Name (99) */

typedef enum
{
    LINK_STAGE_DOWNLOAD = 1, /* done, total: bytes (total 0 until the server says) */
    LINK_STAGE_CHECK,        /* the archive's entries are being checked */
    LINK_STAGE_UNPACK,       /* done, total: bytes unpacked */
    LINK_STAGE_INSTALL,      /* done 0: about to rename into games/; done 1: renamed */
} LinkStage;

/* A worker's view of its caller: cancelled() is asked often (from the downloading or unpacking thread), progress()
 * hears each stage as it starts (done 0) and as it goes. Either may be NULL. */
typedef struct
{
    bool (*cancelled)(void *context);
    void (*progress)(void *context, LinkStage stage, uint64_t done, uint64_t total);
    void *context;
} LinkObserver;

typedef enum
{
    LINK_OK,
    LINK_CANCELLED,
    LINK_BUSY,       /* a game is running, or startup recovery, a backup, a restore or an update holds the gate */
    LINK_BAD_URL,    /* not an http:// or https:// URL */
    LINK_CONNECT,    /* can't find or reach the server */
    LINK_TLS,        /* the secure connection failed */
    LINK_HTTP,       /* the server answered an error (not found, refused...) */
    LINK_WEB_PAGE,   /* the link opened a web page, not a file */
    LINK_STALLED,    /* no data for LINKS_STALL_SECONDS */
    LINK_NETWORK,    /* the connection dropped and could not go on, or no network library */
    LINK_NO_SPACE,
    LINK_FORMAT,     /* not a ZIP: RAR, 7-Zip, an installer... (the message says which, and what to do) */
    LINK_UNSAFE,     /* a ZIP refused for safety (traversal, links, a bomb, names too long...) */
    LINK_DAMAGED,    /* a ZIP that can't be read (CRC, truncated) */
    LINK_IO,         /* the storage could not be written */
} LinkError;

typedef struct
{
    LinkError error;
    char message[256];         /* for the user: what happened and what to do */
    char folder[NP_PATH_MAX];  /* on LINK_OK: the installed game's folder, games/<Folder> */
    uint64_t downloaded;       /* the archive's size */
    bool resumed;              /* the archive was put together from more than one transfer */
} LinkResult;

/* Whether an error means the link can never work as it is (so a links.txt line is not tried again). */
bool link_error_permanent(LinkError error);
/* An http:// or https:// URL with a host, no spaces or control characters, under LINKS_URL_MAX. */
bool link_url_valid(const char *url);

/* The whole of one link, blocking: the gate (MAINT_LINKS), the download, the checks, the unpacking and the rename
 * into games, then the staging cleaned. games is the games folder (it is created when missing). True on LINK_OK. */
bool link_add(const char *games, const char *url, const LinkObserver *observer, LinkResult *result);

/* Install the small, trusted itch.io shelf asset from this app into the user's library. */
bool links_install_shelf(const char *games, const char *asset, LinkResult *result);

/* The steps link_add runs, for the tests. link_download fetches url to dest (dest.part while it comes), and fills
 * name (may be NULL) with the file's name as the server or the URL gives it. */
bool link_download(const char *url, const char *dest, const char *space_root, const LinkObserver *observer,
                   LinkResult *result, char *name, size_t name_size);
/* Checks the ZIP and unpacks it into staging/unpack-<tag>/, then renames its game into games. name_hint names the
 * folder when the archive has no single top folder. The caller holds the gate. */
bool link_unpack(const char *games, const char *staging, const char *zip, const char *tag, const char *name_hint,
                 const LinkObserver *observer, LinkResult *result);

/* At startup, before the first scan: games/.dulcamara-links removed whole, without following links. */
bool links_cleanup(const char *games);

/* links.txt's lines not yet in links-done.txt, in order, at most max (each LINKS_URL_MAX). Returns how many, or -1
 * when links.txt can't be read (why says so). Invalid lines are skipped and counted in *invalid (may be NULL). */
int links_pending(const char *links_file, const char *done_file, char (*urls)[LINKS_URL_MAX], int max,
                  int *invalid, char *why, size_t why_size);
/* Records url (without any user:password@) as processed in links-done.txt (atomically): outcome "added" or
 * "failed". */
bool links_mark(const char *done_file, const char *url, const char *outcome);

/* The game the library lists in folder (an installed link's), or -1 with why it is not shown: the scan's own reason
 * for folder or for what is inside it, else that there is no RPG Maker 2000/2003 or Ren'Py game in it. */
int links_find_game(const Library *lib, const char *folder, char *reason, size_t reason_size);

/* ---------------------------------------------------------------- the background queue (main thread calls) */

typedef enum
{
    LINKS_IDLE,
    LINKS_RUNNING,
    LINKS_FINISHED, /* every link was tried; results says how each went */
    LINKS_CANCELLED,
} LinksState;

#define LINKS_RESULTS_MAX 8 /* the first few links' results are kept for the message at the end */

typedef struct
{
    LinksState state;
    LinkStage stage;
    int index, count; /* the link being worked on (0-based), of count */
    uint64_t done, total;
    int added, failed;
    LinkResult results[LINKS_RESULTS_MAX];
    char last_folder[NP_PATH_MAX]; /* the last game installed: the one to select */
} LinksStatus;

/* Starts a worker over urls (copied). done_file (may be NULL: a typed link) records each processed link. False when a
 * queue is already running or the thread can't start. */
bool links_start(const char *games, const char *done_file, const char (*urls)[LINKS_URL_MAX], int count);
void links_poll(LinksStatus *status);
void links_cancel(void);
/* After FINISHED or CANCELLED has been read: back to IDLE (the worker has ended). */
void links_acknowledge(void);

#endif
