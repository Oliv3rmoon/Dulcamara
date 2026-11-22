/*
 * Dulcamara - insider builds: nightly's test builds for its verified, active members before everyone else
 * (docs/GUIDE.md, Insider builds). A member signs in on the insider service's page with Discord and gets an 8-character
 * insider code, which Settings > System > Insider code stores in the data folder (insider-code). With Test builds on
 * and a code stored, the update check asks the service first (insider_check.h); anything but a newer insider build,
 * and Dulcamara checks GitHub exactly as before. Only the code and the running version are sent. This gates the
 * delivery of updates only: offline, without a code or not qualifying, the app works as it always does.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_INSIDER_H
#define DULCAMARA_INSIDER_H

#include "insider_check.h"
#include "nightshade.h"

/* The insider service's address, such as https://nightly-insider.<account>.workers.dev (no "/" at the end), given at
 * build time: make INSIDER_URL=... (tools/build.sh passes it as -DINSIDER_URL="..."). Empty: no insider builds at
 * all, no code asked for, nothing sent, nothing shown. */
#ifndef INSIDER_URL
#define INSIDER_URL ""
#endif

/* The request's limit, all told (net.c's connection limit is 8 of it): the update check never waits longer on the
 * service before it asks GitHub. */
#define INSIDER_TIMEOUT_SECONDS 12

bool insider_enabled(void); /* INSIDER_URL is set */

/* The stored code (app.paths.root/insider-code): true and code when there is one. */
bool insider_load_code(char code[INSIDER_CODE_LENGTH + 1]);
bool insider_has_code(void);
typedef enum
{
    INSIDER_CODE_SAVED,
    INSIDER_CODE_CLEARED,   /* nothing typed: no code */
    INSIDER_CODE_NOT_A_CODE, /* not exactly 8 of INSIDER_ALPHABET once normalized: nothing stored */
    INSIDER_CODE_NOT_WRITTEN, /* the data folder can't be written */
} InsiderCodeResult;
/* A code as typed: normalized (insider_code_normalize) and stored, or the stored one removed when text has nothing
 * but spaces. */
InsiderCodeResult insider_set_code(const char *text);

/* check_thread's first step (update.c): with Test builds on (test_builds), INSIDER_URL set and a code stored, asks the
 * service; true when its newer insider build is what the check offers: its version, download address, SHA-256 and
 * size into the update's fields. False: the check goes on to GitHub exactly as before. */
bool insider_first(bool test_builds, char *version, size_t version_size, char *url, size_t url_size,
                   char sha256[65], uint64_t *size);
/* install_thread (update.c), before an insider build's download: its address lasts an hour, so one that may have run
 * out is asked for again, for the same build (the same SHA-256); url is left as it is when no fresh one comes. */
void insider_refresh_url(const char *version, const char *sha256, char *url, size_t url_size);

/* What the log may show of a download address (insider_loggable_url): for the service's, its host and the build's
 * file name, never its token. */
const char *insider_loggable(const char *url, char *out, size_t size);
/* install_thread's size check for INSIDER_URL's builds (insider_size_matches), with a line in the log when it fails. */
bool insider_size_ok(const char *url, const char *path, uint64_t size);

/* Settings > System, under Test builds and Insider code: true when the line on where a code comes from is shown. */
bool insider_hint(void);

#endif
