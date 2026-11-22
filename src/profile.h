/* Sparse per-game settings; legacy Ren'Py keys/sounds remain separate. */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_PROFILE_H
#define NP_PROFILE_H
#include "nightshade.h"

typedef enum
{
    PROFILE_INTEGER_SCALE,
    PROFILE_SMOOTH,
    PROFILE_SHADER,
    PROFILE_ASPECT,
    PROFILE_UPSCALE,
    PROFILE_UPSCALE_FILTER,
    PROFILE_GAME_VOLUME,
    PROFILE_FF_SPEED,
    PROFILE_FF_TOGGLE,
    PROFILE_STICK_DPAD,
    PROFILE_SHOW_FPS,
    PROFILE_RENPY_SKIP_UNSEEN,
    PROFILE_AUTOMATIC_BACKUPS,
    PROFILE_FIELD_COUNT
} ProfileField;

typedef struct
{
    uint32_t present; /* BIT(ProfileField): zero means inherit, even when an explicit value would be
                         zero */
    int values[PROFILE_FIELD_COUNT];
    uint16_t button_present[NP_CORES];
    int8_t button_map[NP_CORES][MAP_BUTTONS];
} GameProfile;

/* All-zero is an empty profile. Invalid values fail without changing it. */
bool profile_set(GameProfile *profile, ProfileField field, int value);
void profile_reset(GameProfile *profile, ProfileField field);
bool profile_set_button(GameProfile *profile, int core, int button, int mapping);
void profile_reset_button(GameProfile *profile, int core, int button);
/* Missing, malformed, truncated or unsupported files return false and clear to inheritance. */
bool profile_load(GameProfile *profile, const char *path);
bool profile_save(const GameProfile *profile, const char *path);
/* effective must be separate storage from global; aliasing is ignored to protect global defaults.
 */
void profile_resolve(const Settings *global, const GameProfile *profile, Settings *effective);
#endif
