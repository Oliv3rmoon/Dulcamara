/*
 * Dulcamara - the libretro cores linked into the app, one table entry each.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each core is a static archive built by tools/build-<core>.sh with its libretro API renamed <core>_retro_*, so
 * several live in one eboot.bin. The frontend calls a core only through its CoreInfo: host.c never names a
 * core's functions. Adding a core (ScummVM's next):
 *
 *   1. tools/build-scummvm.sh builds build/scummvm-ps5/libscummvm.a, its API renamed scummvm_retro_*, as
 *      tools/build-easyrpg.sh does; the Makefile adds it to APP_STATIC_ARCHIVES and `make core` runs it.
 *   2. cores.c declares its functions with CORE_DECLARE(scummvm_) and adds CORE_ENTRY(...) to CORES, with its
 *      id, its name, the games it runs and its button defaults.
 *   3. library.c recognises its games (a ScummVM game folder, or a .scummvm file) and sets Game.core to it.
 *
 * host.c answers the environment calls the cores make; one a new core relies on is added there.
 */
#ifndef NP_CORES_H
#define NP_CORES_H

#include "libretro.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
    CORE_EASYRPG = 0, /* EasyRPG Player: RPG Maker 2000 and 2003 */
    CORE_COUNT
};

typedef struct
{
    const char *id;      /* "easyrpg": the archive's prefix, the log's name for it */
    const char *name;    /* "EasyRPG Player" */
    const char *runs;    /* what the library says it plays: "RPG Maker 2000 / 2003" */
    const char *license; /* for Settings > About */
    /* the libretro API, as tools/build-<id>.sh renamed it */
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    /* the frontend's side */
    const int8_t *buttons; /* the default button map (Settings.button_map): each controller button's RetroPad id */
    bool frame_time;       /* the core paces itself by RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK */
} CoreInfo;

extern const CoreInfo CORES[CORE_COUNT];

/* The core with this id ("easyrpg"), or NULL. */
const CoreInfo *core_by_id(const char *id);

#endif
