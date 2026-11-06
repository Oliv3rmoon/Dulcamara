/*
 * Dulcamara - insider builds: what is sent to nightly's insider service and how its answer is read.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A verified, active member of the nightly Discord signs in on the insider service's page and gets an insider code,
 * 8 characters of INSIDER_ALPHABET. With Settings > System > Test builds on and a code stored, the update check
 * (update.c) first POSTs the code and the running version to <base>/v1/updates; only an HTTP 200 answer with
 * "status":"update" whose build passes every check below is offered. Anything else (another status, another HTTP
 * code, no network, a timeout, JSON that can't be read, a build that fails a check) means the GitHub check runs
 * exactly as before. This gates the delivery of new builds only: nothing here decides whether the app may run.
 * The service's contract is nightly-insider's docs/API.md.
 *
 * Plain C11 with no SDL or network, so the host test builds it alone (tests/insider_check.c, run by
 * tests/test_insider.py); the request is a function the caller gives (insider.c: net.c's).
 */
#ifndef DULCAMARA_INSIDER_CHECK_H
#define DULCAMARA_INSIDER_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSIDER_APP "dulcamara"                              /* the app's id at the service */
#define INSIDER_ALPHABET "23456789ABCDEFGHJKMNPQRSTVWXYZ" /* a code's characters: no 0 O 1 I L U */
#define INSIDER_CODE_LENGTH 8
#define INSIDER_UPDATES_PATH "/v1/updates"
#define INSIDER_DOWNLOAD_PATH "/v1/download/"
#define INSIDER_URL_MAX 255 /* a download address, as update.c's zip_url holds it */

/* A code as typed ("abcd-efgh", "ABCD EFGH") as the service reads it: uppercase, and every character outside
 * INSIDER_ALPHABET dropped. code gets the first INSIDER_CODE_LENGTH of what is left; returns how many there were in
 * all. Only INSIDER_CODE_LENGTH exactly is a code. */
int insider_code_normalize(const char *text, char code[INSIDER_CODE_LENGTH + 1]);

/* The request's body, {"app":"dulcamara","installed_version":...,"code":...}, the two strings escaped as JSON
 * strings. False when it doesn't fit in size. */
bool insider_request_json(char *out, size_t size, const char *installed, const char *code);

typedef enum
{
    INSIDER_OFF,           /* nothing asked: no service in this build, Test builds off, or no code */
    INSIDER_UPDATE,        /* 200, "update", and the build passed every check: offer it */
    INSIDER_NONE,          /* 200, "none": the member qualifies, nothing newer */
    INSIDER_NOT_QUALIFIED, /* 200, "not_qualified": the code is good, the member no longer qualifies */
    INSIDER_INVALID_CODE,  /* 200, "invalid_code": no such code */
    INSIDER_EXPIRED_CODE,  /* 200, "expired_code" */
    INSIDER_OTHER,         /* 200 with any other status */
    INSIDER_HTTP_ERROR,    /* not HTTP 200: bad_request, rate_limited, unavailable, a 5xx... */
    INSIDER_OFFLINE,       /* the request couldn't be made: no network, no answer in time */
    INSIDER_MALFORMED,     /* 200 with JSON that can't be read, or cut short */
    INSIDER_BAD_BUILD,     /* 200, "update", but its build failed a check (InsiderResult.why) */
} InsiderStatus;

typedef struct
{
    char version[16];  /* X.Y.Z, without a "v" */
    char file[64];     /* its file's name, for the log only ("" when the answer had none fit to show) */
    char url[INSIDER_URL_MAX + 1]; /* <base>/v1/download/<token>: never logged whole (insider_loggable_url) */
    char sha256[65];   /* lowercase hex */
    uint64_t size;
} InsiderBuild;

typedef struct
{
    InsiderStatus status;
    int http;          /* the HTTP status, negative when the request couldn't be made, 0 when none was made */
    char answer[32];   /* the answer's "status", when it had one of a-z and _ ("" otherwise) */
    const char *why;   /* INSIDER_BAD_BUILD: which check its build failed */
} InsiderResult;

/* Reads the service's answer to POST /v1/updates: the HTTP status http (negative: no answer) and its body json,
 * length bytes (may be NULL when length is 0). INSIDER_UPDATE, and *build, only for HTTP 200 with "status":"update"
 * whose build passes every check: its url starts with base "/v1/download/", has at most INSIDER_URL_MAX characters,
 * all of A-Z a-z 0-9 - _ . / :; its sha256 is 64 lowercase hex; its size is above 0; its version is X.Y.Z (or vX.Y.Z)
 * and newer than installed; and an "app" it names is INSIDER_APP. *build is cleared otherwise. */
InsiderResult insider_read_answer(int http, const char *json, size_t length, const char *base, const char *installed,
                                  InsiderBuild *build);

/* The request, as the caller makes it: POST url with the JSON body json (Content-Type application/json), the body
 * of the answer into *body (malloc'd, NUL-terminated: freed with free() by the caller of the function) and its
 * length. Returns the HTTP status, or a negative value when no answer came. */
typedef int (*InsiderRequest)(void *user, const char *url, const char *json, char **body, size_t *length);

typedef enum
{
    INSIDER_NEXT_GITHUB, /* the update check goes on to GitHub's releases exactly as before */
    INSIDER_NEXT_OFFER,  /* the insider build in *build is what the check offers */
} InsiderNext;

/* The update check's first step (update.c's check_thread). With base set (INSIDER_URL), test_builds on and code a
 * code (insider_code_normalize gives INSIDER_CODE_LENGTH), POST base "/v1/updates" through request and read the
 * answer: INSIDER_NEXT_OFFER and *build for an insider build to offer; INSIDER_NEXT_GITHUB otherwise, and then
 * nothing was asked when result->status is INSIDER_OFF. result says what happened (for the log and the hint). */
InsiderNext insider_decide(const char *base, bool test_builds, const char *code, const char *installed,
                           InsiderRequest request, void *user, InsiderBuild *build, InsiderResult *result);

/* True when url is one of the service's download addresses: base is set and url starts with base "/v1/download/". */
bool insider_owns_url(const char *base, const char *url);

/* What the log may show of a download address: url itself, unless it is the service's (insider_owns_url), whose
 * token is never logged: then "<host>, <file>" (file "" for none). Returns out. */
const char *insider_loggable_url(const char *base, const char *url, const char *file, char *out, size_t size);

/* The installer's size check (update.c's install_thread), before the SHA-256 one: true when url isn't the service's,
 * or the file at path is size bytes. An insider build's answer gives its size; a file of another size is not it. */
bool insider_size_matches(const char *base, const char *url, const char *path, uint64_t size);

/* Whether Settings shows the line on where a code comes from: the service in this build (enabled) and either Test
 * builds on without a code, or the last answer was not_qualified, invalid_code or expired_code. */
bool insider_hint_wanted(bool enabled, bool test_builds, bool has_code, InsiderStatus last);

/* A short word for the log: the status's name ("expired_code", "offline", "malformed"...). */
const char *insider_status_text(InsiderStatus status);

#endif
