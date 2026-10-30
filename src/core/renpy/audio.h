/*
 * Dulcamara - Ren'Py's core: Ren'Py's mixer, played for the frontend. Ren'Py opens its sound with SDL_OpenAudio and
 * a callback that mixes 32-bit float stereo (renpysound_core.c); the core keeps the callback and calls it whenever the
 * frontend asks for sound, then hands on 16-bit samples. Ren'Py is told the mixer runs at 48 kHz, whatever it asked
 * for, and resamples its own sounds to that.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_AUDIO_H
#define DR_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DR_AUDIO_RATE 48000

typedef void (*dr_mix_fn)(void *userdata, uint8_t *stream, int len);

/* SDL_OpenAudio as Ren'Py calls it: keeps fn and writes back the format the mixer really has (48 kHz, stereo), as
 * SDL does into `desired` when there is no `obtained`. Paused until audio_pause(false). Returns 0. */
int audio_open(int *freq, uint8_t *channels, dr_mix_fn fn, void *userdata);
void audio_pause(bool paused);
void audio_lock(void); /* SDL_LockAudio: the callback does not run while it is held; may be taken again inside */
void audio_unlock(void);
void audio_close(void);

/* The frontend: frames of sound, 16-bit stereo; silence while nothing is open, or while paused. */
void audio_render(int16_t *out, size_t frames);
void audio_float_to_s16(const float *in, int16_t *out, size_t samples);

#endif
