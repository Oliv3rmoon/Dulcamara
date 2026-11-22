/* Local user metadata independent of the current scan. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_USERDATA_H
#define NP_USERDATA_H
#include "nightshade.h"
#define USER_METADATA_MAX_RECORDS 65536

typedef struct
{
    char id[24];
    bool favorite;
    uint64_t playtime_seconds;
} GameUserData;

typedef struct
{
    GameUserData *records;
    size_t count, capacity;
} UserMetadata;

/* Initialize to zero. Records survive absent drives; returned pointers expire on
 * mutation/load/free. */
const GameUserData *user_metadata_find(const UserMetadata *metadata, const char *id);
bool user_metadata_set_favorite(UserMetadata *metadata, const char *id, bool favorite);
/* Saturates at UINT64_MAX rather than wrapping. */
bool user_metadata_add_playtime(UserMetadata *metadata, const char *id, uint64_t seconds);
/* Load replaces owned state; missing/malformed/truncated/version mismatch clears and returns false.
 */
bool user_metadata_load(UserMetadata *metadata, const char *path);
bool user_metadata_save(const UserMetadata *metadata, const char *path);
void user_metadata_free(UserMetadata *metadata);
#endif
