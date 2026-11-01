/*
 * Dulcamara - which GitHub release the in-app update offers, and the checks it passes.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * update.c asks https://api.github.com/repos/<UPDATE_REPO>/releases/latest, the newest release
 * of Oliv3rmoon/Dulcamara that is neither a draft nor a pre-release, and hands the
 * answer to release_evaluate(). With Settings > System > Test builds on (update_test_builds), it
 * asks UPDATE_LIST_URL instead, the repository's 20 most recent releases, so that testers are
 * offered its test builds (pre-releases) too, and hands that list to release_evaluate_list(): the
 * newest of them by version that passes checks 1 and 2 below is offered, and one that fails them is
 * passed over for an older one; check 3 (update.c, once SHA256SUMS is fetched) is the chosen one's
 * alone, and when it fails nothing is offered. A test build already offered is withdrawn once Test
 * builds is off (update_state()). A release is offered only when it is not a draft (nor a
 * pre-release, unless Test builds is on) and:
 *   1. its tag is a version, X.Y.Z (or vX.Y.Z), newer than the running one;
 *   2. it carries both UPDATE_ZIP ("PPSA99010.zip") and UPDATE_SUMS ("SHA256SUMS"), uploaded,
 *      and both download addresses are this repository's own release files for that tag;
 *   3. SHA256SUMS lists the ZIP on one well-formed line (release_sums_lookup()), and, when GitHub
 *      reports the asset's digest, the two agree.
 * The download is then checked against that SHA-256 before anything is unpacked. darkxex's
 * SwanStationPS5 releases are never asked for, and a file anywhere else is never taken.
 *
 * Plain C11 with no SDL or network, so the host test builds it alone
 * (tests/update_release_check.c, run by tests/test_update_release.py).
 */
#ifndef DULCAMARA_UPDATE_RELEASE_H
#define DULCAMARA_UPDATE_RELEASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_REPO "Oliv3rmoon/Dulcamara"
#define UPDATE_ZIP "PPSA99010.zip"
#define UPDATE_SUMS "SHA256SUMS"
#define UPDATE_LATEST_URL "https://api.github.com/repos/" UPDATE_REPO "/releases/latest"
#define UPDATE_LIST_URL "https://api.github.com/repos/" UPDATE_REPO "/releases?per_page=20" /* Test builds on */
#define UPDATE_DOWNLOAD_PREFIX "https://github.com/" UPDATE_REPO "/releases/download/"

typedef enum
{
    RELEASE_OFFER = 0,     /* newer, with both files: fetch SHA256SUMS next */
    RELEASE_NOT_NEWER,     /* this version or older */
    RELEASE_UNREADABLE,    /* not the JSON GitHub sends */
    RELEASE_NOT_PUBLISHED, /* a draft, or a pre-release where they are not offered */
    RELEASE_BAD_TAG,       /* the tag is not X.Y.Z */
    RELEASE_NO_ZIP,        /* no uploaded PPSA99010.zip */
    RELEASE_NO_SUMS,       /* no uploaded SHA256SUMS */
    RELEASE_FOREIGN_FILE,  /* a file's address is not this repository's release download */
} ReleaseVerdict;

typedef struct
{
    char version[16];    /* "1.6.0" (the tag without a "v") */
    char zip_url[256];   /* https://github.com/Oliv3rmoon/Dulcamara/releases/download/<tag>/PPSA99010.zip */
    char sums_url[256];  /* the same for SHA256SUMS */
    char zip_digest[65]; /* the ZIP's SHA-256 as GitHub reports it (lowercase hex), or "" */
    uint64_t zip_size;
    bool prerelease; /* a test build: only release_evaluate_list() offers one */
} Release;

/* X.Y.Z (each part 0..99999) into out; false for anything else. */
bool release_version_parse(const char *text, int out[3]);
/* <0, 0 or >0 as a is older than, the same as or newer than b; both must parse (else 0). */
int release_version_compare(const char *a, const char *b);

/* Reads releases/latest's answer (json, length bytes) against the running version. */
ReleaseVerdict release_evaluate(const char *json, size_t length, const char *installed, Release *out);
/* Reads UPDATE_LIST_URL's answer, an array of releases each read as release_evaluate() reads one, a
 * pre-release passing when allow_prerelease: RELEASE_OFFER and out for the newest by version of those
 * it would offer. When there is none, the verdict (and out->version) of the newest by version of the
 * others, one without a version counting as older than any; RELEASE_NOT_NEWER for an empty array.
 * RELEASE_UNREADABLE, and nothing offered, when any of it can't be read. */
ReleaseVerdict release_evaluate_list(const char *json, size_t length, const char *installed, bool allow_prerelease,
                                     Release *out);

/* The SHA-256 SHA256SUMS gives `file` ("<64 hex>  <file>" or "<64 hex> *<file>", as sha256sum
 * writes them): 1 and sha256 (lowercase) when there is exactly one well-formed line for it, 0 when
 * there is none, -1 when its line is malformed or there are two. */
int release_sums_lookup(const char *text, size_t length, const char *file, char sha256[65]);

/* The SHA-256 of a file, as 64 lowercase hex characters. */
bool release_file_sha256(const char *path, char hex[65]);

/* A short English phrase for the log. */
const char *release_verdict_text(ReleaseVerdict verdict);

#endif
