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
void update_install(void); /* in the background; needs UPDATE_AVAILABLE */
int update_state(void);
const char *update_version(void);
const char *update_message(void);

#endif
