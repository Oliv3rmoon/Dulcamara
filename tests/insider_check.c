/*
 * Dulcamara - host checks for insider builds (src/insider_check.c): the code, the request, the service's answers, the
 * update check's first step with a request that is the test's own, and the installer's checks of the download.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built and run by tests/test_insider.py:
 *   insider-check <scratch folder>       every check; prints "PASS: <n> checks" when all pass
 *   insider-check json <installed> <code>  prints the request's body, for the test to read back as JSON
 * The answers are shaped like nightly-insider's docs/API.md. src/util.c's np_log is the app's own: its log is read
 * back to show a download address's token never reaches it.
 */
#include "insider_check.h"
#include "nightshade.h"
#include "update_release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++failures;                                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

static const char *const BASE = "https://nightly-insider.example.workers.dev";
static const char *const INSTALLED = "0.2.13";
static const char *const SHA = "4def0fd289a935cb37b5df04bf19dbc3523c8a2de8c7b1f92020ea0d63c77bca";
static const char *const TOKEN = "eyJvIjoiYnVpbGRzL2R1bGNhbWFyYSJ9.c2lnbmF0dXJlLW9mLXRoZS10b2tlbg";

/* An "update" answer, the build's fields as given (each a JSON value, or NULL to leave it out). */
static char *update_answer(const char *version, const char *url, const char *sha, const char *size, const char *extra)
{
    char *out = malloc(8192);
    char fields[4096] = "";
    size_t n = 0;
    if (version)
        n += (size_t)snprintf(fields + n, sizeof(fields) - n, "\"version\": %s, ", version);
    n += (size_t)snprintf(fields + n, sizeof(fields) - n,
                          "\"tag\": \"0.2.14\", \"title\": \"Dulcamara 0.2.14 (insider build)\", "
                          "\"file\": \"PPSA99010.zip\", ");
    if (size)
        n += (size_t)snprintf(fields + n, sizeof(fields) - n, "\"size\": %s, ", size);
    if (sha)
        n += (size_t)snprintf(fields + n, sizeof(fields) - n, "\"sha256\": %s, ", sha);
    n += (size_t)snprintf(fields + n, sizeof(fields) - n,
                          "\"notes\": \"Fixed: \\\"url\\\": \\\"https://evil.example/x\\\", status none {[\", "
                          "\"published_at\": \"2026-11-08T05:00:00Z\", \"public_after\": \"2026-11-15T05:00:00Z\", ");
    if (url)
        n += (size_t)snprintf(fields + n, sizeof(fields) - n, "\"url\": %s, ", url);
    snprintf(fields + n, sizeof(fields) - n, "\"url_expires_at\": \"2026-11-08T06:00:00Z\"");
    snprintf(out, 8192, "{\n  \"status\": \"update\",\n  \"app\": \"dulcamara\",%s\n  \"build\": {%s}\n}\n",
             extra ? extra : "", fields);
    return out;
}

static char *quoted(const char *s)
{
    static char buffers[4][600];
    static int next;
    char *b = buffers[next++ % 4];
    snprintf(b, sizeof(buffers[0]), "\"%s\"", s);
    return b;
}

static char good_url[256];

static InsiderResult read_answer(int http, const char *json, InsiderBuild *b)
{
    return insider_read_answer(http, json, json ? strlen(json) : 0, BASE, INSTALLED, b);
}

/* ---------------------------------------------------------------- the code */

static void test_code(void)
{
    char code[INSIDER_CODE_LENGTH + 1];
    CHECK(insider_code_normalize("abcd-efgh", code) == 8 && !strcmp(code, "ABCDEFGH"));
    CHECK(insider_code_normalize("ABCD-EFGH", code) == 8 && !strcmp(code, "ABCDEFGH"));
    CHECK(insider_code_normalize(" abcd efgh\n", code) == 8 && !strcmp(code, "ABCDEFGH"));
    CHECK(insider_code_normalize("2345-6789", code) == 8 && !strcmp(code, "23456789"));
    CHECK(insider_code_normalize("wxyz-2345", code) == 8 && !strcmp(code, "WXYZ2345"));
    /* every character of the alphabet, and only those */
    CHECK(insider_code_normalize(INSIDER_ALPHABET, code) == 30 && !strcmp(code, "23456789"));
    CHECK(insider_code_normalize("01OILU oilu", code) == 0 && !strcmp(code, ""));
    CHECK(insider_code_normalize("", code) == 0 && !strcmp(code, ""));
    CHECK(insider_code_normalize("ABCD-EFG", code) == 7);              /* one short */
    CHECK(insider_code_normalize("ABCD-EFGHJ", code) == 9);            /* one too many: not cut to 8 */
    CHECK(insider_code_normalize("ABCD-EFG0", code) == 7);             /* a zero is no code character */
    CHECK(insider_code_normalize("AB\xC3\xA9" "CDEFGH", code) == 8 && !strcmp(code, "ABCDEFGH")); /* é dropped */
    CHECK(insider_code_normalize("a-b-c-d-e-f-g-h", code) == 8 && !strcmp(code, "ABCDEFGH"));
}

/* ---------------------------------------------------------------- the request */

static void test_request_json(void)
{
    char json[256];
    CHECK(insider_request_json(json, sizeof(json), "0.2.13", "ABCDEFGH"));
    CHECK(!strcmp(json, "{\"app\":\"dulcamara\",\"installed_version\":\"0.2.13\",\"code\":\"ABCDEFGH\"}"));
    CHECK(insider_request_json(json, sizeof(json), "1\"2\\3\n\t\x01", "AB\"CD"));
    CHECK(!strcmp(json, "{\"app\":\"dulcamara\",\"installed_version\":\"1\\\"2\\\\3\\u000a\\u0009\\u0001\","
                        "\"code\":\"AB\\\"CD\"}"));
    /* too small: nothing half-written */
    char small[40];
    CHECK(!insider_request_json(small, sizeof(small), "0.2.13", "ABCDEFGH") && small[0] == '\0');
    const size_t exact = strlen("{\"app\":\"dulcamara\",\"installed_version\":\"0.2.13\",\"code\":\"ABCDEFGH\"}");
    char fits[128];
    CHECK(insider_request_json(fits, exact + 1, "0.2.13", "ABCDEFGH") && strlen(fits) == exact);
    CHECK(!insider_request_json(fits, exact, "0.2.13", "ABCDEFGH"));
}

/* ---------------------------------------------------------------- the answers */

static void test_update(void)
{
    InsiderBuild b;
    char *json = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), "81234567", NULL);
    InsiderResult r = read_answer(200, json, &b);
    CHECK(r.status == INSIDER_UPDATE && r.http == 200 && !strcmp(r.answer, "update") && !r.why);
    CHECK(!strcmp(b.version, "0.2.14") && !strcmp(b.url, good_url) && !strcmp(b.sha256, SHA) && b.size == 81234567);
    CHECK(!strcmp(b.file, "PPSA99010.zip"));
    free(json);
    /* "v" before the version, and an answer with no "app" */
    json = update_answer("\"v0.3.0\"", quoted(good_url), quoted(SHA), "1", NULL);
    char *no_app = strstr(json, "\"app\": \"dulcamara\",");
    memmove(no_app, no_app + strlen("\"app\": \"dulcamara\","), strlen(no_app + strlen("\"app\": \"dulcamara\",")) + 1);
    r = read_answer(200, json, &b);
    CHECK(r.status == INSIDER_UPDATE && !strcmp(b.version, "0.3.0") && b.size == 1);
    free(json);
    /* the same in one line, with no spaces, and a "hint" after the build */
    char line[2048];
    snprintf(line, sizeof(line),
             "{\"status\":\"update\",\"app\":\"dulcamara\",\"build\":{\"version\":\"0.2.14\",\"file\":\"PPSA99010.zip\","
             "\"size\":5,\"sha256\":\"%s\",\"url\":\"%s\"},\"hint\":null}",
             SHA, good_url);
    r = read_answer(200, line, &b);
    CHECK(r.status == INSIDER_UPDATE && b.size == 5);
    /* a file name that can't be shown is left out of the log, not a reason to refuse the build */
    json = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), "7", NULL);
    char *file = strstr(json, "PPSA99010.zip");
    memcpy(file, "../../x y.zip", strlen("../../x y.zip"));
    r = read_answer(200, json, &b);
    CHECK(r.status == INSIDER_UPDATE && !strcmp(b.file, ""));
    free(json);
}

static void test_other_statuses(void)
{
    InsiderBuild b;
    struct
    {
        const char *json;
        InsiderStatus status;
    } cases[] = {
        {"{\"status\":\"none\"}", INSIDER_NONE},
        {"{\"status\":\"none\",\"hint\":\"Nothing newer.\"}", INSIDER_NONE},
        {"{\"status\":\"not_qualified\",\"hint\":\"Insider builds: get a code in the nightly Discord\"}",
         INSIDER_NOT_QUALIFIED},
        {"{\"status\":\"invalid_code\",\"hint\":\"x\"}", INSIDER_INVALID_CODE},
        {" {\"hint\": \"x\", \"status\": \"expired_code\"} \n", INSIDER_EXPIRED_CODE},
        {"{\"status\":\"rate_limited\"}", INSIDER_OTHER},
        {"{\"status\":\"something_new\",\"build\":{\"url\":\"x\"}}", INSIDER_OTHER},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        memset(&b, 0x55, sizeof(b));
        const InsiderResult r = read_answer(200, cases[i].json, &b);
        CHECK(r.status == cases[i].status);
        CHECK(b.url[0] == '\0' && b.size == 0); /* cleared */
    }
    /* another HTTP status, whatever the body says: the public path */
    InsiderResult r = read_answer(503, "{\"status\":\"unavailable\"}", &b);
    CHECK(r.status == INSIDER_HTTP_ERROR && r.http == 503 && !strcmp(r.answer, "unavailable"));
    r = read_answer(429, "{\"status\":\"rate_limited\"}", &b);
    CHECK(r.status == INSIDER_HTTP_ERROR && !strcmp(r.answer, "rate_limited"));
    r = read_answer(415, "{\"status\":\"bad_request\"}", &b); /* not application/json */
    CHECK(r.status == INSIDER_HTTP_ERROR && r.http == 415);
    r = read_answer(403, "{\"status\":\"forbidden\"}", &b); /* a web page's Origin or Sec-Fetch-Site */
    CHECK(r.status == INSIDER_HTTP_ERROR && r.http == 403 && !strcmp(r.answer, "forbidden"));
    r = read_answer(404, "{\"status\":\"unknown_app\"}", &b);
    CHECK(r.status == INSIDER_HTTP_ERROR);
    r = read_answer(400, "{\"status\":\"bad_request\"}", &b);
    CHECK(r.status == INSIDER_HTTP_ERROR);
    r = read_answer(500, "<html>error</html>", &b);
    CHECK(r.status == INSIDER_HTTP_ERROR && r.answer[0] == '\0');
    char *json = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), "81234567", NULL);
    r = read_answer(201, json, &b); /* an "update" with another status is no update */
    CHECK(r.status == INSIDER_HTTP_ERROR && b.url[0] == '\0');
    free(json);
    r = read_answer(-1, NULL, &b);
    CHECK(r.status == INSIDER_OFFLINE && r.http == -1);
    /* a status of characters it can't have is not repeated in the log */
    r = read_answer(200, "{\"status\":\"Update now!\"}", &b);
    CHECK(r.status == INSIDER_OTHER && r.answer[0] == '\0');
}

static void test_malformed(void)
{
    InsiderBuild b;
    const char *const cases[] = {
        "", "not json", "{", "[]", "[{\"status\":\"update\"}]", "\"update\"", "{\"status\":}", "{\"status\":1}",
        "{\"status\":null}", "{\"status\":\"update\"", "{\"status\":\"none\"} trailing", "{\"status\":\"none\",}",
        "{\"status\":\"none\"}{\"status\":\"none\"}", "{\"status\":\"\\x\"}", "{status:\"none\"}",
        "{\"hint\":\"no status\"}", "{\"status\":\"none\" \"hint\":\"x\"}",
        "{\"status\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        const InsiderResult r = read_answer(200, cases[i], &b);
        if (r.status != INSIDER_MALFORMED)
            printf("case %zu: %s\n", i, insider_status_text(r.status));
        CHECK(r.status == INSIDER_MALFORMED);
    }
    CHECK(insider_read_answer(200, NULL, 0, BASE, INSTALLED, &b).status == INSIDER_MALFORMED);
    /* a NUL inside the answer ends nothing early: it is not JSON */
    const char nul[] = "{\"status\":\"none\"}\0{";
    CHECK(insider_read_answer(200, nul, sizeof(nul) - 1, BASE, INSTALLED, &b).status == INSIDER_MALFORMED);
    /* nesting deeper than the scanner follows */
    char deep[400] = "{\"status\":\"none\",\"x\":";
    for (int i = 0; i < 60; ++i)
        strcat(deep, "[");
    for (int i = 0; i < 60; ++i)
        strcat(deep, "]");
    strcat(deep, "}");
    CHECK(read_answer(200, deep, &b).status == INSIDER_MALFORMED);

    /* an "update" cut short anywhere is never offered */
    char *json = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), "81234567", NULL);
    const size_t length = strlen(json);
    int offered = 0;
    for (size_t cut = 0; cut + 2 < length; ++cut) /* the last two are "}\n": cut there, it is whole */
    {
        const InsiderResult r = insider_read_answer(200, json, cut, BASE, INSTALLED, &b);
        offered += r.status == INSIDER_UPDATE;
        CHECK(r.status == INSIDER_MALFORMED);
    }
    CHECK(offered == 0);
    CHECK(insider_read_answer(200, json, length - 1, BASE, INSTALLED, &b).status == INSIDER_UPDATE);
    free(json);
}

static void expect_bad(const char *json, const char *why_part)
{
    InsiderBuild b;
    memset(&b, 0x55, sizeof(b));
    const InsiderResult r = read_answer(200, json, &b);
    CHECK(r.status == INSIDER_BAD_BUILD);
    CHECK(b.url[0] == '\0' && b.version[0] == '\0' && b.size == 0);
    if (r.status != INSIDER_BAD_BUILD || !r.why || !strstr(r.why, why_part))
        printf("  expected a build refused for \"%s\", got %s (%s): %.200s\n", why_part,
               insider_status_text(r.status), r.why ? r.why : "-", json);
    CHECK(r.why && strstr(r.why, why_part));
}

static void test_bad_builds(void)
{
    char long_url[400];
    snprintf(long_url, sizeof(long_url), "\"%s/v1/download/%0*d\"", BASE, 255 - (int)strlen(BASE) - 13 + 1, 7);
    char at_limit[400];
    snprintf(at_limit, sizeof(at_limit), "\"%s/v1/download/%0*d\"", BASE, 255 - (int)strlen(BASE) - 13, 7);
    const char *const wrong_urls[] = {
        "\"https://evil.example/v1/download/abc\"",
        "\"https://nightly-insider.example.workers.dev.evil.example/v1/download/abc\"",
        "\"http://nightly-insider.example.workers.dev/v1/download/abc\"",
        "\"https://nightly-insider.example.workers.dev/v1/updates\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/\"",
        "\"https://nightly-insider.example.workers.dev/v1/downloads/abc\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/../../v1/updates\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/abc/./x\"",
        "\"https://nightly-insider.example.workers.dev/v1/download//x\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/abc/..\"",
        "\"/v1/download/abc\"",
    };
    for (size_t i = 0; i < sizeof(wrong_urls) / sizeof(wrong_urls[0]); ++i)
    {
        char *json = update_answer("\"0.2.14\"", wrong_urls[i], quoted(SHA), "100", NULL);
        expect_bad(json, "insider service's");
        free(json);
    }
    const char *const odd_characters[] = {
        "\"https://nightly-insider.example.workers.dev@evil.example/v1/download/abc\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/abc?x=1\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/abc def\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/abc%2F\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/abc\\u00e9\"",
        "\"https://nightly-insider.example.workers.dev/v1/download/abc#x\"",
    };
    for (size_t i = 0; i < sizeof(odd_characters) / sizeof(odd_characters[0]); ++i)
    {
        char *json = update_answer("\"0.2.14\"", odd_characters[i], quoted(SHA), "100", NULL);
        expect_bad(json, "characters");
        free(json);
    }
    char *json = update_answer("\"0.2.14\"", long_url, quoted(SHA), "100", NULL);
    expect_bad(json, "too long");
    free(json);
    json = update_answer("\"0.2.14\"", at_limit, quoted(SHA), "100", NULL);
    InsiderBuild b;
    CHECK(read_answer(200, json, &b).status == INSIDER_UPDATE && strlen(b.url) == 255);
    free(json);
    json = update_answer("\"0.2.14\"", NULL, quoted(SHA), "100", NULL);
    expect_bad(json, "no download address");
    free(json);
    json = update_answer("\"0.2.14\"", "42", quoted(SHA), "100", NULL);
    expect_bad(json, "no download address");
    free(json);

    const char *const bad_shas[] = {
        "\"4DEF0FD289A935CB37B5DF04BF19DBC3523C8A2DE8C7B1F92020EA0D63C77BCA\"", /* uppercase */
        "\"4def0fd289a935cb37b5df04bf19dbc3523c8a2de8c7b1f92020ea0d63c77bc\"",  /* 63 */
        "\"4def0fd289a935cb37b5df04bf19dbc3523c8a2de8c7b1f92020ea0d63c77bcaa\"", /* 65 */
        "\"4def0fd289a935cb37b5df04bf19dbc3523c8a2de8c7b1f92020ea0d63c77bcg\"", /* not hex */
        "\"sha256:4def0fd289a935cb37b5df04bf19dbc3523c8a2de8c7b1f92020ea0d6\"", "\"\"", "null", "1234",
    };
    for (size_t i = 0; i < sizeof(bad_shas) / sizeof(bad_shas[0]); ++i)
    {
        json = update_answer("\"0.2.14\"", quoted(good_url), bad_shas[i], "100", NULL);
        expect_bad(json, "SHA-256");
        free(json);
    }
    json = update_answer("\"0.2.14\"", quoted(good_url), NULL, "100", NULL);
    expect_bad(json, "SHA-256");
    free(json);

    const char *const bad_sizes[] = {"0", "-5", "\"100\"", "1.5", "1e9", "18446744073709551616", "null", "true",
                                     "{}", "[1]"};
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i)
    {
        json = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), bad_sizes[i], NULL);
        expect_bad(json, "size");
        free(json);
    }
    json = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), NULL, NULL);
    expect_bad(json, "size");
    free(json);

    const char *const older[] = {"\"0.2.13\"", "\"0.2.12\"", "\"v0.2.13\"", "\"00.002.013\"", "\"0.1.99\""};
    for (size_t i = 0; i < sizeof(older) / sizeof(older[0]); ++i)
    {
        json = update_answer(older[i], quoted(good_url), quoted(SHA), "100", NULL);
        expect_bad(json, "not newer");
        free(json);
    }
    const char *const not_versions[] = {"\"0.2\"", "\"0.2.14-rc1\"", "\"0.2.14.1\"", "\"vv0.2.14\"", "\"\"", "14",
                                        "\"1234567.0.0\"", "\"0.2.14 \""};
    for (size_t i = 0; i < sizeof(not_versions) / sizeof(not_versions[0]); ++i)
    {
        json = update_answer(not_versions[i], quoted(good_url), quoted(SHA), "100", NULL);
        expect_bad(json, "version");
        free(json);
    }
    json = update_answer(NULL, quoted(good_url), quoted(SHA), "100", NULL);
    expect_bad(json, "no version");
    free(json);

    /* a build for another app, or no build at all */
    json = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), "100", NULL);
    char *app = strstr(json, "\"dulcamara\"");
    memcpy(app, "\"nightshad\"", strlen("\"nightshad\""));
    expect_bad(json, "another app");
    free(json);
    expect_bad("{\"status\":\"update\",\"app\":\"dulcamara\"}", "no build");
    expect_bad("{\"status\":\"update\",\"build\":\"0.2.14\"}", "no build");
}

/* ---------------------------------------------------------------- the update check's first step */

typedef struct
{
    int calls;
    int http;           /* what the "service" answers */
    const char *answer; /* its body, or NULL for none */
    char url[300], json[300];
} Fake;

static int fake_request(void *user, const char *url, const char *json, char **body, size_t *length)
{
    Fake *f = user;
    ++f->calls;
    snprintf(f->url, sizeof(f->url), "%s", url);
    snprintf(f->json, sizeof(f->json), "%s", json);
    *body = NULL;
    *length = 0;
    if (f->answer)
    {
        *length = strlen(f->answer);
        *body = malloc(*length + 1);
        memcpy(*body, f->answer, *length + 1);
    }
    return f->http;
}

static InsiderNext decide(Fake *f, const char *base, bool test_builds, const char *code, InsiderBuild *b,
                          InsiderResult *r)
{
    return insider_decide(base, test_builds, code, INSTALLED, fake_request, f, b, r);
}

static void test_decide(void)
{
    InsiderBuild b;
    InsiderResult r;
    char *update = update_answer("\"0.2.14\"", quoted(good_url), quoted(SHA), "81234567", NULL);

    /* the update: the insider build wins, asked for at <base>/v1/updates with the code and the version */
    Fake f = {0, 200, update, "", ""};
    CHECK(decide(&f, BASE, true, "ABCDEFGH", &b, &r) == INSIDER_NEXT_OFFER);
    CHECK(f.calls == 1 && r.status == INSIDER_UPDATE && !strcmp(b.version, "0.2.14") && !strcmp(b.url, good_url));
    CHECK(!strcmp(f.url, "https://nightly-insider.example.workers.dev/v1/updates"));
    CHECK(!strcmp(f.json, "{\"app\":\"dulcamara\",\"installed_version\":\"0.2.13\",\"code\":\"ABCDEFGH\"}"));
    /* a code as typed is sent as the service reads it */
    f = (Fake){0, 200, update, "", ""};
    CHECK(decide(&f, BASE, true, "abcd-efgh\n", &b, &r) == INSIDER_NEXT_OFFER && strstr(f.json, "\"ABCDEFGH\""));

    /* an expired code, a refused one, no build newer: GitHub, as before */
    const struct
    {
        int http;
        const char *answer;
        InsiderStatus status;
    } github[] = {
        {200, "{\"status\":\"expired_code\",\"hint\":\"get a new code\"}", INSIDER_EXPIRED_CODE},
        {200, "{\"status\":\"invalid_code\"}", INSIDER_INVALID_CODE},
        {200, "{\"status\":\"not_qualified\"}", INSIDER_NOT_QUALIFIED},
        {200, "{\"status\":\"none\"}", INSIDER_NONE},
        {-1, NULL, INSIDER_OFFLINE},  /* no network, no answer in time */
        {-7, NULL, INSIDER_OFFLINE},
        {500, "{\"status\":\"error\"}", INSIDER_HTTP_ERROR},
        {502, "<html>Bad gateway</html>", INSIDER_HTTP_ERROR},
        {503, "{\"status\":\"unavailable\"}", INSIDER_HTTP_ERROR},
        {429, "{\"status\":\"rate_limited\"}", INSIDER_HTTP_ERROR},
        {415, "{\"status\":\"bad_request\"}", INSIDER_HTTP_ERROR},
        {403, "{\"status\":\"forbidden\"}", INSIDER_HTTP_ERROR},
        {200, "{\"status\":\"update\"", INSIDER_MALFORMED},
        {200, NULL, INSIDER_MALFORMED},
        {200, "{\"status\":\"update\",\"build\":{\"version\":\"0.2.12\"}}", INSIDER_BAD_BUILD},
    };
    for (size_t i = 0; i < sizeof(github) / sizeof(github[0]); ++i)
    {
        f = (Fake){0, github[i].http, github[i].answer, "", ""};
        memset(&b, 0x55, sizeof(b));
        const InsiderNext next = decide(&f, BASE, true, "ABCDEFGH", &b, &r);
        if (next != INSIDER_NEXT_GITHUB || r.status != github[i].status)
            printf("  case %zu: next %d, status %s\n", i, (int)next, insider_status_text(r.status));
        CHECK(next == INSIDER_NEXT_GITHUB && f.calls == 1 && r.status == github[i].status);
        CHECK(b.url[0] == '\0' && b.size == 0);
        CHECK(r.http == github[i].http);
    }

    /* nothing asked at all: no service in this build, Test builds off, no code or not a code */
    const struct
    {
        const char *base;
        bool test_builds;
        const char *code;
    } off[] = {
        {"", true, "ABCDEFGH"}, {NULL, true, "ABCDEFGH"}, {BASE, false, "ABCDEFGH"}, {BASE, true, ""},
        {BASE, true, NULL},     {BASE, true, "ABCDEFG"},   {BASE, true, "ABCDEFGHJ"}, {BASE, true, "OOOOIIII"},
    };
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); ++i)
    {
        f = (Fake){0, 200, update, "", ""};
        CHECK(decide(&f, off[i].base, off[i].test_builds, off[i].code, &b, &r) == INSIDER_NEXT_GITHUB);
        CHECK(f.calls == 0 && r.status == INSIDER_OFF && r.http == 0);
    }
    /* no request function: nothing asked */
    CHECK(insider_decide(BASE, true, "ABCDEFGH", INSTALLED, NULL, NULL, &b, &r) == INSIDER_NEXT_GITHUB &&
          r.status == INSIDER_OFF);
    /* a service address so long the request's can't be made: nothing asked */
    char long_base[400];
    memset(long_base, 'a', sizeof(long_base) - 1);
    long_base[sizeof(long_base) - 1] = '\0';
    f = (Fake){0, 200, update, "", ""};
    CHECK(decide(&f, long_base, true, "ABCDEFGH", &b, &r) == INSIDER_NEXT_GITHUB && f.calls == 0);
    free(update);
}

/* ---------------------------------------------------------------- the download */

static bool write_file(const char *path, const char *bytes, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    const bool ok = fwrite(bytes, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* The installer's checks, as update.c's install_thread makes them (tests/test_insider.py checks it is this line). */
static bool installer_accepts(const char *url, const char *path, const char *sha256, uint64_t size)
{
    char got[65] = "";
    return !(!insider_size_matches(BASE, url, path, size) || !release_file_sha256(path, got) ||
             strcmp(got, sha256) != 0);
}

static void test_download(const char *scratch)
{
    char path[1024], sha[65];
    snprintf(path, sizeof(path), "%s/update-0.2.14.zip", scratch);
    static const char build[] = "PK\x03\x04 the insider build's bytes";
    const size_t size = sizeof(build) - 1;
    CHECK(write_file(path, build, size) && release_file_sha256(path, sha));
    CHECK(installer_accepts(good_url, path, sha, size));
    /* tampered: the same size, another byte */
    static const char tampered[] = "PK\x03\x04 the insider build's bytez";
    CHECK(write_file(path, tampered, size));
    CHECK(!installer_accepts(good_url, path, sha, size));
    /* the bytes it should have and more, or fewer: refused by its size before its SHA-256 is read */
    CHECK(write_file(path, build, size));
    CHECK(!insider_size_matches(BASE, good_url, path, size + 1) && !installer_accepts(good_url, path, sha, size + 1));
    CHECK(!insider_size_matches(BASE, good_url, path, size - 1) && !installer_accepts(good_url, path, sha, size - 1));
    CHECK(!insider_size_matches(BASE, good_url, path, 0));
    CHECK(insider_size_matches(BASE, good_url, path, size));
    /* no file: refused */
    char missing[1024];
    snprintf(missing, sizeof(missing), "%s/missing.zip", scratch);
    CHECK(!insider_size_matches(BASE, good_url, missing, size) && !installer_accepts(good_url, missing, sha, size));
    /* a GitHub release's size is GitHub's, not checked here: its SHA-256 alone, as before */
    const char *github = "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.14/PPSA99010.zip";
    CHECK(insider_size_matches(BASE, github, path, size + 100));
    CHECK(installer_accepts(github, path, sha, size + 100));
    CHECK(write_file(path, tampered, size) && !installer_accepts(github, path, sha, size));
    /* no service in this build: no address is its */
    CHECK(!insider_owns_url("", good_url) && insider_size_matches("", good_url, path, size + 1));
    CHECK(insider_owns_url(BASE, good_url) && !insider_owns_url(BASE, github));
}

/* ---------------------------------------------------------------- the log and the hint */

static void test_log(const char *scratch)
{
    char out[160];
    CHECK(!strcmp(insider_loggable_url(BASE, good_url, "PPSA99010.zip", out, sizeof(out)),
                  "nightly-insider.example.workers.dev, PPSA99010.zip"));
    CHECK(!strstr(out, TOKEN) && !strstr(out, "/v1/download/"));
    CHECK(!strcmp(insider_loggable_url(BASE, good_url, "", out, sizeof(out)),
                  "nightly-insider.example.workers.dev, (no file name)"));
    const char *github = "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.14/PPSA99010.zip";
    CHECK(!strcmp(insider_loggable_url(BASE, github, "x", out, sizeof(out)), github));
    CHECK(!strcmp(insider_loggable_url("", good_url, "x", out, sizeof(out)), good_url)); /* not this build's */

    /* np_log (src/util.c) never writes a download address's token, whoever logs it: net.c's lines name the address */
    char path[1024];
    snprintf(path, sizeof(path), "%s/dulcamara.log", scratch);
    np_log_open(path);
    np_log("net: %s: going on from %d bytes", good_url, 1000);
    np_log("net: %s -> curl %d (%s), http %ld", good_url, 28, "Operation timed out", 0L);
    np_log("net: https://h.example/v1/download/ab and https://h.example/v1/download/%s?x", TOKEN);
    np_log("update: downloading %s", "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.14/PPSA99010.zip");
    np_log_open(path); /* closes it (and keeps it as .old.log) */
    char old[1100];
    snprintf(old, sizeof(old), "%s/dulcamara.old.log", scratch);
    FILE *f = fopen(old, "r");
    char text[4096] = "";
    if (f)
    {
        text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
        fclose(f);
    }
    CHECK(f != NULL);
    CHECK(!strstr(text, TOKEN) && !strstr(text, "c2lnbmF0dXJl"));
    CHECK(strstr(text, "net: https://nightly-insider.example.workers.dev/v1/download/...: going on from 1000 bytes"));
    CHECK(strstr(text, "https://h.example/v1/download/.. and https://h.example/v1/download/...?x"));
    CHECK(strstr(text, "releases/download/0.2.14/PPSA99010.zip")); /* GitHub's addresses stay whole */
    printf("log:\n%s", text);

    CHECK(!insider_hint_wanted(false, true, false, INSIDER_EXPIRED_CODE)); /* no service in this build: never */
    CHECK(insider_hint_wanted(true, true, false, INSIDER_OFF));           /* Test builds on, no code */
    CHECK(!insider_hint_wanted(true, false, false, INSIDER_OFF));          /* Test builds off, no code */
    CHECK(!insider_hint_wanted(true, true, true, INSIDER_OFF));
    CHECK(insider_hint_wanted(true, true, true, INSIDER_EXPIRED_CODE));
    CHECK(insider_hint_wanted(true, true, true, INSIDER_INVALID_CODE));
    CHECK(insider_hint_wanted(true, true, true, INSIDER_NOT_QUALIFIED));
    /* offline, or anything else: nothing added */
    const InsiderStatus quiet[] = {INSIDER_UPDATE, INSIDER_NONE, INSIDER_OTHER, INSIDER_HTTP_ERROR, INSIDER_OFFLINE,
                                   INSIDER_MALFORMED, INSIDER_BAD_BUILD};
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); ++i)
        CHECK(!insider_hint_wanted(true, true, true, quiet[i]));
    CHECK(!strcmp(insider_status_text(INSIDER_EXPIRED_CODE), "expired_code"));
    CHECK(!strcmp(insider_status_text(INSIDER_OFFLINE), "offline"));
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "json"))
    {
        char json[1024];
        if (!insider_request_json(json, sizeof(json), argv[2], argv[3]))
            return 1;
        puts(json);
        return 0;
    }
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <scratch folder> | json <installed> <code>\n", argv[0]);
        return 2;
    }
    snprintf(good_url, sizeof(good_url), "%s/v1/download/%s", BASE, TOKEN);
    test_code();
    test_request_json();
    test_update();
    test_other_statuses();
    test_malformed();
    test_bad_builds();
    test_decide();
    test_download(argv[1]);
    test_log(argv[1]);
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
