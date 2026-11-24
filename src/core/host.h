/*
 * Dulcamara - the libretro host: runs the core a game needs (cores.h) and hands its pictures, sound and
 * buttons to and from the platform layer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_HOST_H
#define NP_HOST_H

#include "../library.h"
#include "../nightshade.h"
#include "runtime_protocol.h"

enum HostState
{
    HOST_IDLE,
    HOST_LOADING,
    HOST_RUNNING,
    HOST_STOPPING,
    HOST_RESTART_REQUIRED,
    HOST_FAILURE
};
/* Frontend/main thread only. poll/request return without waiting on Python.
 * FAILURE with owns_runtime=true is sticky unsafe timeout, not an unloaded core. */
enum HostState host_poll(void);
enum HostState host_state(void);
bool host_owns_runtime(void);
void host_request_stop(void);
/* Copies owned data. True means supported v4; inspect available/complete before use.
 * Unknown locations and custom Python I/O are not inferred. */
bool host_copy_save_layout(struct dr_save_layout *out);
/* Generation must match the currently held checkpoint; stale releases fail.
 * Not releasing is the hold operation. RenPy gates on its thread; EasyRPG starts
 * synchronously on release, and may do game initialization I/O in that call. */
bool host_release_checkpoint(uint32_t generation);
bool host_snapshot_eligible(void);
void host_reset_transient(void);
typedef struct
{
    uint64_t engine_calls, engine_us, presentations, fresh_pictures, audio_frames, audio_underruns,
        frame_bytes, presentation_us, peak_resident_bytes, heap_mapped_bytes;
} HostCounters;
void host_counters(HostCounters *out);
void host_note_presentation(uint64_t elapsed_us);

/* Starts the game on its core. system_dir: the core's system folder (EasyRPG's RTP is in <system>/rtp/2000 and
 * rtp/2003). False with a reason in error. */
bool host_load(const Game *g, const char *system_dir, char *error, size_t error_size);
/* Compatibility wrapper: requests stop and polls once; does not imply completion. */
void host_unload(void);
bool host_loaded(void);
const char *host_core_name(void); /* "EasyRPG Player" while a game runs */
bool host_core_skips(void);       /* fast-forward is the running core's own skip (CoreInfo.own_skip) */

/* Player 1's controller for the next frames, its buttons mapped to the game's by the settings. */
void host_set_input(const PadState *pad, const Settings *settings);
/* Runs `times` of the core's frames, each `seconds` of the game's time (fast-forward runs several a refresh). */
void host_run(int times, double seconds);
/* Keeps the sound queued: asks the core for more while less than `seconds` is waiting. With `grows` (a Ren'Py game's),
 * each underrun (what the last top-up, less than a second before, left queued has run out with sound expected: not a
 * pause; counted once until the queue is refilled) doubles
 * what is kept ahead, up to 0.4 s, for the rest of the game, and more calls a pass refill it; each underrun is logged
 * (the first 20 of a game, a second apart at least), with the queue's state, the time since the last top-up and what is
 * now kept ahead. */
void host_feed_audio(double seconds, bool grows);
void host_set_volume(int tenths); /* 0..10 */
void host_set_fast_forward(bool on); /* what RETRO_ENVIRONMENT_GET_FASTFORWARDING answers */
/* A core option's value from Dulcamara's Settings ("renpy_skip_unseen", "on"), kept across loads: what
 * RETRO_ENVIRONMENT_GET_VARIABLE answers for it before the core's default. */
void host_set_option(const char *key, const char *value);
/* The core asked to close (EasyRPG: Exit on the title screen). */
bool host_shutdown_requested(void);
/* Whether the game wants text typed now (a Ren'Py game's name entry: RETRO_ENVIRONMENT_DULCAMARA_TEXT_INPUT, from a
 * core that takes typing), and where its own input is: its top and bottom, 0 to 1 down the picture (both 0 when the
 * core does not say). */
bool host_text_wanted(float *top, float *bottom);
/* One press of a key (a RETROK_*: RETROK_BACKSPACE, RETROK_RETURN) or of a character (with RETROK_UNKNOWN), from the
 * on-screen keyboard: down and up, given to the core's keyboard callback as it polls at its next frame. */
void host_type(unsigned retro_key, uint32_t character);
void host_reset(void);

double host_fps(void);
int host_sample_rate(void);
float host_aspect(void); /* the picture's shape, width / height */
/* The latest picture; NULL until the core has drawn one. fresh: drawn since the last call. */
const void *host_frame(int *width, int *height, size_t *pitch, int *pixel_format, bool *fresh);

/* Save states, when the core has them (EasyRPG Player has not: its saves are the game's own). */
bool host_can_save_states(void);

#endif
