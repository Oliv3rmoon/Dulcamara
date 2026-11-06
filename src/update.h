/*
 * SwanStationPS5 - updates from the project's GitHub releases.
 * Dulcamara: Oliv3rmoon/Dulcamara's (UPDATE_REPO, update_release.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_UPDATE_H
#define NP_UPDATE_H

#include "nightshade.h"

enum UpdateState
{
    UPDATE_IDLE,
    UPDATE_CHECKING,
    UPDATE_NONE,      /* this is the newest version */
    UPDATE_AVAILABLE, /* update_version() is newer */
    UPDATE_INSTALLING,
    UPDATE_INSTALLED, /* restart the app to use it */
    UPDATE_FAILED,    /* update_message() says why */
};

void update_check(void);   /* in the background */
/* In the background; needs UPDATE_AVAILABLE, or UPDATE_FAILED after an install that failed (update_can_retry()),
 * which it tries again. */
void update_install(void);
int update_state(void);
const char *update_version(void);
const char *update_message(void);
bool update_test_build(void);  /* what is offered is a test build (a pre-release) */
uint64_t update_size(void);    /* the download's size in bytes as GitHub lists it, or 0 */
bool update_can_retry(void);   /* UPDATE_FAILED, and it was the install that failed: update_install() tries again */
/* While UPDATE_INSTALLING: the bytes downloaded so far and how many there are (0 until known). */
void update_progress(uint64_t *done, uint64_t *total);

#endif
