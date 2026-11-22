/*
 * Dulcamara - insider builds in the app (insider.h): the stored code, the request through net.c, what the log and
 * Settings show. What is sent and how the answer is judged is insider_check.c's.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "insider.h"

#include "app.h"
#include "net.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

/* the last answer's InsiderStatus (INSIDER_OFF until one, and again once the code changes), for the hint: written by
 * the update's threads, read by Settings */
static SDL_atomic_t last_status;
/* Settings' view of the stored code: -1 until the file is read */
static int has_code = -1;
/* the offered build's file name and when it was offered, for install_thread (the update's threads only) */
static char offered_file[64];
static Uint32 offered_at;

#define URL_FRESH_MS (30u * 60u * 1000u) /* a download address younger than this is used as it is */

bool insider_enabled(void)
{
    return INSIDER_URL[0] != '\0';
}

static void code_path(char *out, size_t size)
{
    path_join(out, size, app.paths.root, "insider-code");
}

bool insider_load_code(char code[INSIDER_CODE_LENGTH + 1])
{
    code[0] = '\0';
    char path[NP_PATH_MAX], line[64] = "";
    code_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    const bool read = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0'; /* the line's end */
    return read && insider_code_normalize(line, code);
}

bool insider_has_code(void)
{
    if (has_code < 0)
    {
        char code[INSIDER_CODE_LENGTH + 1];
        has_code = insider_load_code(code);
    }
    return has_code == 1;
}

InsiderCodeResult insider_set_code(const char *text)
{
    char path[NP_PATH_MAX], temp[NP_PATH_MAX + 8], code[INSIDER_CODE_LENGTH + 1];
    code_path(path, sizeof(path));
    if (strspn(text, " ") == strlen(text))
    {
        remove(path);
        has_code = 0;
        SDL_AtomicSet(&last_status, INSIDER_OFF);
        np_log("insider: no code stored");
        return INSIDER_CODE_CLEARED;
    }
    if (!insider_code_normalize(text, code))
        return INSIDER_CODE_NOT_A_CODE;
    /* written whole, then renamed into place: the update's thread may be reading it */
    snprintf(temp, sizeof(temp), "%s.new", path);
    FILE *f = fopen(temp, "w");
    const bool written = f && fprintf(f, "%s\n", code) == INSIDER_CODE_LENGTH + 1;
    if ((f && fclose(f) != 0) || !written || rename(temp, path) != 0)
    {
        remove(temp);
        np_log("insider: couldn't write %s", path);
        return INSIDER_CODE_NOT_WRITTEN;
    }
    has_code = 1;
    SDL_AtomicSet(&last_status, INSIDER_OFF);
    np_log("insider: a code is stored"); /* never the code itself */
    return INSIDER_CODE_SAVED;
}

static int post(void *user, const char *url, const char *json, char **body, size_t *length)
{
    (void)user;
    /* never redirected: the code goes to INSIDER_URL alone, and a 3xx is an answer like any other not 200 (GitHub) */
    return net_request_within(url, json, "application/json", NP_NAME "/" NP_VERSION, INSIDER_TIMEOUT_SECONDS, false,
                              body, length);
}

/* The service asked, with the stored code; one line in the log for what came back. */
static InsiderNext ask(bool test_builds, InsiderBuild *build, InsiderResult *result)
{
    char code[INSIDER_CODE_LENGTH + 1];
    if (!insider_enabled() || !test_builds || !insider_load_code(code))
    {
        memset(build, 0, sizeof(*build));
        memset(result, 0, sizeof(*result));
        result->status = INSIDER_OFF;
        return INSIDER_NEXT_GITHUB; /* nothing asked, nothing logged: as before */
    }
    const InsiderNext next = insider_decide(INSIDER_URL, test_builds, code, NP_VERSION, post, NULL, build, result);
    if (result->status != INSIDER_OFF)
    {
        SDL_AtomicSet(&last_status, (int)result->status);
        np_log("update: insider status=%s http=%d%s%s%s",
               result->answer[0] ? result->answer : insider_status_text(result->status), result->http,
               result->why ? " (rejected: " : "", result->why ? result->why : "", result->why ? ")" : "");
    }
    return next;
}

bool insider_first(bool test_builds, char *version, size_t version_size, char *url, size_t url_size,
                   char sha256[65], uint64_t *size)
{
    InsiderBuild build;
    InsiderResult result;
    if (ask(test_builds, &build, &result) != INSIDER_NEXT_OFFER || strlen(build.url) >= url_size ||
        strlen(build.version) >= version_size)
        return false;
    str_copy(version, version_size, build.version);
    str_copy(url, url_size, build.url);
    str_copy(sha256, 65, build.sha256);
    *size = build.size;
    str_copy(offered_file, sizeof(offered_file), build.file);
    offered_at = SDL_GetTicks();
    char shown[160];
    np_log("update: %s (an insider build) available, %s, %llu bytes, sha256 %s", version,
           insider_loggable(url, shown, sizeof(shown)), (unsigned long long)build.size, sha256);
    return true;
}

void insider_refresh_url(const char *version, const char *sha256, char *url, size_t url_size)
{
    if (!insider_owns_url(INSIDER_URL, url) || SDL_GetTicks() - offered_at < URL_FRESH_MS)
        return;
    InsiderBuild build;
    InsiderResult result;
    if (ask(true, &build, &result) == INSIDER_NEXT_OFFER && !strcmp(build.sha256, sha256) &&
        !strcmp(build.version, version) && strlen(build.url) < url_size)
    {
        str_copy(url, url_size, build.url);
        offered_at = SDL_GetTicks();
        np_log("update: a fresh download address for the insider build %s", version);
    }
    else
        np_log("update: no fresh download address for the insider build %s; the one it had is used", version);
}

const char *insider_loggable(const char *url, char *out, size_t size)
{
    return insider_loggable_url(INSIDER_URL, url, offered_file, out, size);
}

bool insider_size_ok(const char *url, const char *path, uint64_t size)
{
    if (insider_size_matches(INSIDER_URL, url, path, size))
        return true;
    np_log("update: the insider build isn't the %llu bytes its answer gave; not installed", (unsigned long long)size);
    return false;
}

bool insider_hint(void)
{
    return insider_hint_wanted(insider_enabled(), app.settings.update_test_builds, insider_has_code(),
                               (InsiderStatus)SDL_AtomicGet(&last_status));
}
