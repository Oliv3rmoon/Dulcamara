/*
 * Dulcamara - the libretro host: runs the core a game needs (cores.h) and hands its pictures, sound and
 * buttons to and from the platform layer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_HOST_H
#define NP_HOST_H

#include "../library.h"
#include "../nightshade.h"

/* Starts the game on its core. system_dir: the core's system folder (EasyRPG's RTP is in <system>/rtp/2000 and
 * rtp/2003). False with a reason in error. */
bool host_load(const Game *g, const char *system_dir, char *error, size_t error_size);
void host_unload(void);
bool host_loaded(void);
const char *host_core_name(void); /* "EasyRPG Player" while a game runs */

/* Player 1's controller for the next frames, its buttons mapped to the game's by the settings. */
void host_set_input(const PadState *pad, const Settings *settings);
/* Runs `times` of the core's frames, each `seconds` of the game's time (fast-forward runs several a refresh). */
void host_run(int times, double seconds);
/* Keeps the sound queued: asks the core for more while less than `seconds` is waiting. */
void host_feed_audio(double seconds);
void host_set_volume(int tenths); /* 0..10 */
void host_set_fast_forward(bool on); /* what RETRO_ENVIRONMENT_GET_FASTFORWARDING answers */
/* The core asked to close (EasyRPG: Exit on the title screen). */
bool host_shutdown_requested(void);
void host_reset(void);

double host_fps(void);
int host_sample_rate(void);
float host_aspect(void); /* the picture's shape, width / height */
/* The latest picture; NULL until the core has drawn one. fresh: drawn since the last call. */
const void *host_frame(int *width, int *height, size_t *pitch, int *pixel_format, bool *fresh);

/* Save states, when the core has them (EasyRPG Player has not: its saves are the game's own). */
bool host_can_save_states(void);

#endif
