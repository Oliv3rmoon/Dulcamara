/*
 * Dulcamara - shared types for the frontend (from SwanStationPS5's SwanStationPS5.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything here is plain C11 so the same sources build for the PS5 and for the desktop test build.
 */
#ifndef NP_H
#define NP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NP_NAME "Dulcamara"   /* identifiers: the User-Agent of network requests, the Vulkan application name */
#define NP_TITLE "Dulcamara" /* what is shown on screen */
#define NP_TITLE_ID "PPSA99010"
#define NP_VERSION "0.2.5"
#define NP_DATA "/data/dulcamara" /* the data folder on the console */
#define NP_PATH_MAX 512

/* Pad bits use RetroPad numbering. PlayStation names: Cross = B, Circle = A, Square = Y, Triangle = X. The buttons a
 * game gets are remapped (Settings > Controls), so these are the controller's own. */
enum
{
    BTN_CROSS = 0,
    BTN_SQUARE = 1,
    BTN_SELECT = 2,
    BTN_START = 3,
    BTN_UP = 4,
    BTN_DOWN = 5,
    BTN_LEFT = 6,
    BTN_RIGHT = 7,
    BTN_CIRCLE = 8,
    BTN_TRIANGLE = 9,
    BTN_L1 = 10,
    BTN_R1 = 11,
    BTN_L2 = 12,
    BTN_R2 = 13,
    BTN_L3 = 14,
    BTN_R3 = 15,
    BTN_MENU = 16, /* touchpad click: opens the Dulcamara menu */
};
#define BIT(b) (1u << (b))
#define NP_MAX_PADS 4 /* the platform layer reads four; the games get player 1's */

typedef struct
{
    bool connected;
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t l2, r2;    /* how far the triggers are pressed, 0..255 */
    bool motion;       /* quat holds the controller's orientation */
    float quat[4];     /* x, y, z, w */
    int16_t gun_x, gun_y; /* unused here: the platform layer's light gun */
    bool gun_offscreen;
    bool touching;     /* the touchpad (one finger) */
    uint16_t touch_x, touch_y; /* 0..1919, 0..1079 */
    int16_t mouse_dx, mouse_dy;
} PadState;

enum UpscaleFilter
{
    UPSCALE_SHARP = 0,     /* nearest-neighbour prescale: crisp pixels */
    UPSCALE_SMOOTH_PIXELS, /* Scale2x/Scale3x edge smoothing */
    UPSCALE_XBR,           /* xBR: edge-directed, smoothest (2x passes) */
    UPSCALE_FILTER_COUNT
};

/* The game's buttons: RetroPad ids, what Settings > Controls maps the controller's buttons to. EasyRPG Player reads
 * A as Decision, B and X as Cancel (and the menu), Y as Shift, Start as its own settings, L and R as its debug keys
 * and Select as Reset (back to the title screen). */
enum
{
    PAD_B = 0,
    PAD_Y = 1,
    PAD_SELECT = 2,
    PAD_START = 3,
    PAD_UP = 4,
    PAD_DOWN = 5,
    PAD_LEFT = 6,
    PAD_RIGHT = 7,
    PAD_A = 8,
    PAD_X = 9,
    PAD_L = 10,
    PAD_R = 11,
    PAD_L2 = 12,
    PAD_R2 = 13,
    PAD_L3 = 14,
    PAD_R3 = 15,
    PAD_NONE = -1
};
#define MAP_BUTTONS 16 /* BTN_CROSS..BTN_R3 */
#define NP_CORES 3 /* CORE_COUNT (core/cores.h), for the settings' button maps */

typedef struct
{
    /* the picture (the platform layer reads the first group) */
    bool integer_scale; /* whole-number scale factors only */
    bool smooth;        /* bilinear for the final scale to the screen; off: sharp pixels */
    int shader;         /* vk_present.c's numbers (SHADERS in ui/settings.c), 0 none */
    bool supersampling; /* always off: the platform layer reads it */
    int crt;            /* software scanlines, 0 off (the shaders do better) */
    int border;         /* around the picture: 0 black */
    int crop_edges;     /* 0: the platform layer reads it */
    bool manual_crop;   /* off: the platform layer reads it */
    int mc_w, mc_h, mc_x, mc_y;
    int aspect;         /* 0 4:3, 1 stretched to 16:9 */
    int upscale;        /* 1..3: prescale before the final scale */
    int upscale_filter; /* enum UpscaleFilter */
    /* playing */
    int ff_speed;       /* fast-forward: 0 x2, 1 x3, 2 x4, 3 x6, 4 x8 */
    int ff_toggle;      /* 1: R2 turns fast-forward on and off; 0: while it is held */
    bool stick_dpad;    /* the left stick moves like the D-pad */
    bool show_fps;
    bool renpy_skip_unseen; /* Ren'Py: fast-forward skips unread text too (its skip_unseen preference) */
    /* each core's (CORE_*): each controller button (BTN_*) the game's button it presses (PAD_*), or -1 */
    int8_t button_map[NP_CORES][MAP_BUTTONS];
    /* the app */
    int ui_sound;       /* SFX_STYLE_* in ui/sfx.h; SFX_STYLE_OFF mutes */
    int ui_volume;      /* 0..3 = 25/50/75/100 % */
    int game_volume;    /* 0..10, tenths of the game's sound */
    bool update_check;  /* look for a newer release at start */
    bool community_seen; /* the empty library has shown nightly's line (community.h, ui/shelf.c): not again */
    int sort_mode;      /* the library's order: 0 title, 1 last played */
    int last_game;      /* the library's cursor, by position */
} Settings;

/* Data layout below the data folder (NP_DATA on the console). */
typedef struct
{
    char root[NP_PATH_MAX];
    char games[NP_PATH_MAX];   /* root/games: one folder (or .zip / .easyrpg) per game */
    char rtp[NP_PATH_MAX];     /* root/rtp: the RPG Maker 2000 and 2003 run-time packages, rtp/2000 and rtp/2003 */
    char logs[NP_PATH_MAX];
    char cache[NP_PATH_MAX];   /* downloads (the update) */
    char config[NP_PATH_MAX];  /* root/settings.ini */
    char played[NP_PATH_MAX];  /* root/played.txt: when each game was last played */
} Paths;

void np_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void np_log_open(const char *path);

/* Small string helpers shared by the frontend. */
void str_copy(char *dst, size_t size, const char *src);
void path_join(char *dst, size_t size, const char *a, const char *b);
bool path_exists(const char *path);
bool path_is_dir(const char *path);
bool make_dirs(const char *path);
bool file_copy(const char *from, const char *to); /* replaces `to` */
const char *path_ext(const char *path);  /* lower-case-insensitive extension without dot, "" if none */
int str_icmp(const char *a, const char *b);
/* localtime into *out (the PS5 libc has no localtime_r); false on failure */
struct tm;
bool local_time(long long when, struct tm *out);

#endif
