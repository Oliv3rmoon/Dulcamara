/*
 * Dulcamara - the cores' table (cores.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cores.h"

#include "../nightshade.h"

#include <string.h>

/* The libretro API of the core whose archive renamed it <prefix>retro_*. */
#define CORE_DECLARE(p)                                                                                            \
    void p##retro_set_environment(retro_environment_t);                                                            \
    void p##retro_set_video_refresh(retro_video_refresh_t);                                                        \
    void p##retro_set_audio_sample(retro_audio_sample_t);                                                          \
    void p##retro_set_audio_sample_batch(retro_audio_sample_batch_t);                                              \
    void p##retro_set_input_poll(retro_input_poll_t);                                                              \
    void p##retro_set_input_state(retro_input_state_t);                                                            \
    void p##retro_init(void);                                                                                      \
    void p##retro_deinit(void);                                                                                    \
    unsigned p##retro_api_version(void);                                                                           \
    void p##retro_get_system_info(struct retro_system_info *);                                                     \
    void p##retro_get_system_av_info(struct retro_system_av_info *);                                               \
    void p##retro_set_controller_port_device(unsigned, unsigned);                                                  \
    void p##retro_reset(void);                                                                                     \
    void p##retro_run(void);                                                                                       \
    size_t p##retro_serialize_size(void);                                                                          \
    bool p##retro_serialize(void *, size_t);                                                                       \
    bool p##retro_unserialize(const void *, size_t);                                                               \
    bool p##retro_load_game(const struct retro_game_info *);                                                       \
    void p##retro_unload_game(void);

#define CORE_FUNCTIONS(p)                                                                                          \
    p##retro_set_environment, p##retro_set_video_refresh, p##retro_set_audio_sample,                               \
        p##retro_set_audio_sample_batch, p##retro_set_input_poll, p##retro_set_input_state, p##retro_init,         \
        p##retro_deinit, p##retro_api_version, p##retro_get_system_info, p##retro_get_system_av_info,              \
        p##retro_set_controller_port_device, p##retro_reset, p##retro_run, p##retro_serialize_size,                \
        p##retro_serialize, p##retro_unserialize, p##retro_load_game, p##retro_unload_game

/* On the console every core's API is renamed (tools/build-<core>.sh). The desktop build links the one core that
 * tools/build-easyrpg.sh host builds, as it is. */
#if defined(__PROSPERO__) || defined(NP_CORES_RENAMED)
#define EASYRPG_PREFIX easyrpg_
#else
#define EASYRPG_PREFIX
#endif
#define DECLARE_WITH(p) CORE_DECLARE(p)
#define FUNCTIONS_WITH(p) CORE_FUNCTIONS(p)

DECLARE_WITH(EASYRPG_PREFIX)

/* Ren'Py's two cores are built for the console only (tools/build-renpy.sh and build-renpy7.sh ps5; their host builds
 * are test programs). The desktop build lists them without functions, and host_load refuses their games there. */
#if defined(__PROSPERO__) || defined(NP_HAVE_RENPY)
CORE_DECLARE(renpy_)
CORE_DECLARE(renpy7_)
#define RENPY_FUNCTIONS CORE_FUNCTIONS(renpy_)
#define RENPY7_FUNCTIONS CORE_FUNCTIONS(renpy7_)
#else
#define RENPY_FUNCTIONS                                                                                            \
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
#define RENPY7_FUNCTIONS RENPY_FUNCTIONS
#endif

/* EasyRPG Player's buttons (src/platform/libretro/input_buttons.cpp in it): A Decision, B and X Cancel, Y Shift,
 * L3 and R3 the 0 and 5 keys, Start its own settings, Select Reset. Cross confirms and Circle cancels, as on the
 * PS5; Triangle opens the game's menu; Select (the game's reset) and the debug keys on L1 / R1 are left off. */
static const int8_t EASYRPG_BUTTONS[MAP_BUTTONS] = {
    [BTN_CROSS] = PAD_A,   [BTN_SQUARE] = PAD_Y,   [BTN_SELECT] = PAD_NONE, [BTN_START] = PAD_START,
    [BTN_UP] = PAD_UP,     [BTN_DOWN] = PAD_DOWN,  [BTN_LEFT] = PAD_LEFT,   [BTN_RIGHT] = PAD_RIGHT,
    [BTN_CIRCLE] = PAD_B,  [BTN_TRIANGLE] = PAD_X, [BTN_L1] = PAD_NONE,     [BTN_R1] = PAD_NONE,
    [BTN_L2] = PAD_NONE,   [BTN_R2] = PAD_NONE,    [BTN_L3] = PAD_L3,       [BTN_R3] = PAD_R3,
};

/* Ren'Py's buttons are SDL's controller buttons, by where they sit (src/core/renpy/input.c): Cross is the bottom one,
 * which Ren'Py takes as "confirm", Circle opens the game menu, L1 and R1 roll back and forward, Start is the menu
 * too; Square and Triangle are Ren'Py's own (hide the window, skip). Ren'Py 7 takes them as Ren'Py 8 does. */
static const int8_t RENPY_BUTTONS[MAP_BUTTONS] = {
    [BTN_CROSS] = PAD_B,   [BTN_SQUARE] = PAD_Y,   [BTN_SELECT] = PAD_SELECT, [BTN_START] = PAD_START,
    [BTN_UP] = PAD_UP,     [BTN_DOWN] = PAD_DOWN,  [BTN_LEFT] = PAD_LEFT,     [BTN_RIGHT] = PAD_RIGHT,
    [BTN_CIRCLE] = PAD_A,  [BTN_TRIANGLE] = PAD_X, [BTN_L1] = PAD_L,          [BTN_R1] = PAD_R,
    [BTN_L2] = PAD_NONE,   [BTN_R2] = PAD_NONE,    [BTN_L3] = PAD_L3,         [BTN_R3] = PAD_R3,
};

const CoreInfo CORES[CORE_COUNT] = {
    [CORE_EASYRPG] = {"easyrpg", "EasyRPG Player", "RPG Maker 2000 / 2003", "GPL-3.0-or-later",
                      FUNCTIONS_WITH(EASYRPG_PREFIX), EASYRPG_BUTTONS, true, false},
    [CORE_RENPY] = {"renpy", "Ren'Py", "Ren'Py 8", "MIT (Ren'Py), PSF (Python), LGPL-2.1 (FFmpeg)", RENPY_FUNCTIONS,
                    RENPY_BUTTONS, true, true},
    [CORE_RENPY7] = {"renpy7", "Ren'Py 7", "Ren'Py 7 / 6",
                     "MIT (Ren'Py), PSF (Python), zlib (pygame_sdl2), LGPL-2.1 (FFmpeg)", RENPY7_FUNCTIONS,
                     RENPY_BUTTONS, true, true},
};

_Static_assert(CORE_COUNT == NP_CORES, "NP_CORES (nightshade.h) is the number of cores");

const CoreInfo *core_by_id(const char *id)
{
    for (int i = 0; i < CORE_COUNT; ++i)
        if (!strcmp(CORES[i].id, id))
            return &CORES[i];
    return NULL;
}
