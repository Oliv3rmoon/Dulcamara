/*
 * Dulcamara - host checks for the update's release decision (src/update_release.c).
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built and run by tests/test_update_release.py: the answers are shaped like GitHub's
 * releases/latest and releases?per_page=20 (Test builds on), the files like the release workflow's
 * (.github/workflows/build.yml).
 */
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

static const char *const SHA_A = "4def0fd289a935cb37b5df04bf19dbc3523c8a2de8c7b1f92020ea0d63c77bca";
static const char *const SHA_B = "51b79f4af0cf038fca576560b04fce479f74fbdf6dad4b80953ba544a2a5a6b1";

/* One asset as GitHub lists it. */
static void asset(char *out, size_t size, const char *repo, const char *tag, const char *name, const char *state,
                  const char *digest)
{
    char digest_json[96];
    if (digest)
        snprintf(digest_json, sizeof(digest_json), "\"sha256:%s\"", digest);
    else
        snprintf(digest_json, sizeof(digest_json), "null");
    snprintf(out, size,
             "{\"url\":\"https://api.github.com/repos/%s/releases/assets/1\",\"id\":1,\"node_id\":\"RA_x\","
             "\"name\":\"%s\",\"label\":\"\",\"uploader\":{\"login\":\"github-actions[bot]\",\"id\":41898282,"
             "\"type\":\"Bot\",\"site_admin\":false},\"content_type\":\"application/zip\",\"state\":\"%s\","
             "\"size\":18267896,\"digest\":%s,\"download_count\":3,\"created_at\":\"2026-10-28T00:00:00Z\","
             "\"updated_at\":\"2026-10-28T00:00:00Z\","
             "\"browser_download_url\":\"https://github.com/%s/releases/download/%s/%s\"}",
             repo, name, state, digest_json, repo, tag, name);
}

/* A releases/latest answer: the fields before and after the assets as GitHub sends them. */
static char *release(const char *tag, const char *flags, const char *assets, const char *body)
{
    size_t size = strlen(assets) + strlen(body) + 4096;
    char *out = malloc(size);
    snprintf(out, size,
             "{\n  \"url\": \"https://api.github.com/repos/Oliv3rmoon/Dulcamara/releases/1\",\n"
             "  \"html_url\": \"https://github.com/Oliv3rmoon/Dulcamara/releases/tag/%s\",\n"
             "  \"id\": 1,\n  \"author\": {\"login\": \"github-actions[bot]\", \"id\": 41898282, \"type\": \"Bot\"},\n"
             "  \"node_id\": \"RE_x\",\n  \"tag_name\": \"%s\",\n  \"target_commitish\": \"main\",\n"
             "  \"name\": \"Dulcamara %s\",\n  %s,\n  \"immutable\": false,\n"
             "  \"created_at\": \"2026-10-28T00:00:00Z\",\n  \"published_at\": \"2026-10-28T00:00:00Z\",\n"
             "  \"assets\": [%s],\n"
             "  \"tarball_url\": \"https://api.github.com/repos/Oliv3rmoon/Dulcamara/tarball/%s\",\n"
             "  \"zipball_url\": \"https://api.github.com/repos/Oliv3rmoon/Dulcamara/zipball/%s\",\n"
             "  \"body\": \"%s\",\n  \"reactions\": {\"total_count\": 0, \"+1\": 0}\n}\n",
             tag, tag, tag, flags, assets, tag, tag, body);
    return out;
}

static const char *const PUBLISHED = "\"draft\": false, \"prerelease\": false";
static const char *const OURS = "Oliv3rmoon/Dulcamara";

/* Both files of a release of `repo` at `tag`. */
static void both(char *out, size_t size, const char *repo, const char *tag, const char *digest)
{
    char zip[1024], sums[1024], exfat[1024];
    asset(exfat, sizeof(exfat), repo, tag, "PPSA99010.exfat", "uploaded", SHA_B);
    asset(zip, sizeof(zip), repo, tag, "PPSA99010.zip", "uploaded", digest);
    asset(sums, sizeof(sums), repo, tag, "SHA256SUMS", "uploaded", SHA_B);
    snprintf(out, size, "%s,%s,%s", exfat, zip, sums);
}

static ReleaseVerdict evaluate(const char *json, const char *installed, Release *r)
{
    return release_evaluate(json, strlen(json), installed, r);
}

static const char *const PRERELEASE = "\"draft\": false, \"prerelease\": true";
static const char *const DRAFT = "\"draft\": true, \"prerelease\": false";

/* A release of ours at tag with both files (malloc'd). */
static char *ours(const char *tag, const char *flags)
{
    char files[4096];
    both(files, sizeof(files), OURS, tag, SHA_A);
    return release(tag, flags, files, "");
}

/* A releases?per_page=20 answer: the releases, most recent first as GitHub lists them (malloc'd). */
static char *list(char *const *releases, size_t count)
{
    size_t size = 3;
    for (size_t i = 0; i < count; ++i)
        size += strlen(releases[i]) + 1;
    char *out = malloc(size);
    strcpy(out, "[");
    for (size_t i = 0; i < count; ++i)
    {
        if (i)
            strcat(out, ",");
        strcat(out, releases[i]);
    }
    strcat(out, "]");
    return out;
}

static void discard(char **releases, size_t count, char *json)
{
    for (size_t i = 0; i < count; ++i)
        free(releases[i]);
    free(json);
}

static ReleaseVerdict evaluate_list(const char *json, const char *installed, bool allow_prerelease, Release *r)
{
    return release_evaluate_list(json, strlen(json), installed, allow_prerelease, r);
}

static void test_versions(void)
{
    int v[3];
    CHECK(release_version_parse("1.6.0", v) && v[0] == 1 && v[1] == 6 && v[2] == 0);
    CHECK(release_version_parse("01.006.010", v) && v[0] == 1 && v[1] == 6 && v[2] == 10);
    CHECK(!release_version_parse("1.6", v));
    CHECK(!release_version_parse("1.6.0-rc1", v));
    CHECK(!release_version_parse("1.6.0.1", v));
    CHECK(!release_version_parse("", v));
    CHECK(!release_version_parse("a.b.c", v));
    CHECK(!release_version_parse("123456.0.0", v));
    CHECK(!release_version_parse("v1.6.0", v)); /* the "v" is taken off before */
    CHECK(release_version_compare("1.10.0", "1.9.2") > 0);
    CHECK(release_version_compare("1.6.0", "1.5.9") > 0);
    CHECK(release_version_compare("1.5.9", "1.6.0") < 0);
    CHECK(release_version_compare("1.6.0", "01.006.000") == 0);
}

static void test_offer(void)
{
    char files[4096];
    Release r;
    both(files, sizeof(files), OURS, "1.6.1", SHA_A);
    char *json = release("1.6.1", PUBLISHED, files, "- Fixes.\\r\\n- More fixes.");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_OFFER);
    CHECK(!strcmp(r.version, "1.6.1"));
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/1.6.1/PPSA99010.zip"));
    CHECK(!strcmp(r.sums_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/1.6.1/SHA256SUMS"));
    CHECK(!strcmp(r.zip_digest, SHA_A));
    CHECK(r.zip_size == 18267896u);
    /* not newer: the same version, an older one */
    CHECK(evaluate(json, "1.6.1", &r) == RELEASE_NOT_NEWER);
    CHECK(evaluate(json, "1.7.0", &r) == RELEASE_NOT_NEWER);
    free(json);

    /* a "v" tag: the version without it, the addresses with it */
    both(files, sizeof(files), OURS, "v1.7.0", NULL);
    json = release("v1.7.0", PUBLISHED, files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_OFFER);
    CHECK(!strcmp(r.version, "1.7.0"));
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/v1.7.0/PPSA99010.zip"));
    CHECK(r.zip_digest[0] == '\0'); /* no digest from GitHub: SHA256SUMS alone */
    free(json);

    /* an upper-case digest comes out lower-case */
    char upper[65];
    for (int i = 0; i < 64; ++i)
        upper[i] = (char)(SHA_A[i] >= 'a' && SHA_A[i] <= 'f' ? SHA_A[i] - 'a' + 'A' : SHA_A[i]);
    upper[64] = '\0';
    both(files, sizeof(files), OURS, "1.6.1", upper);
    json = release("1.6.1", PUBLISHED, files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_OFFER && !strcmp(r.zip_digest, SHA_A));
    free(json);
}

static void test_not_offered(void)
{
    char files[4096], one[1024], two[1024];
    Release r;
    char *json;

    both(files, sizeof(files), OURS, "1.6.1", SHA_A);
    json = release("1.6.1", "\"draft\": false, \"prerelease\": true", files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_NOT_PUBLISHED);
    free(json);
    json = release("1.6.1", "\"draft\": true, \"prerelease\": false", files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_NOT_PUBLISHED);
    free(json);

    /* darkxex's own release, whatever its version: never offered */
    both(files, sizeof(files), "darkxex/SwanStationPS5", "1.5.9", SHA_A);
    json = release("1.5.9", PUBLISHED, files, "- No changes from 1.5.8.");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_NOT_NEWER);
    free(json);
    both(files, sizeof(files), "darkxex/SwanStationPS5", "1.9.0", SHA_A);
    json = release("1.9.0", PUBLISHED, files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_FOREIGN_FILE);
    free(json);

    /* Mandrake's release (another app of the same author): never offered */
    both(files, sizeof(files), "Oliv3rmoon/Mandrake", "1.9.0", SHA_A);
    json = release("1.9.0", PUBLISHED, files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_FOREIGN_FILE);
    free(json);

    /* one file of another tag, or another host */
    asset(one, sizeof(one), OURS, "1.6.0", "PPSA99010.zip", "uploaded", SHA_A);
    asset(two, sizeof(two), OURS, "1.6.1", "SHA256SUMS", "uploaded", NULL);
    snprintf(files, sizeof(files), "%s,%s", one, two);
    json = release("1.6.1", PUBLISHED, files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_FOREIGN_FILE);
    free(json);
    asset(one, sizeof(one), "Oliv3rmoon/Dulcamara.evil.example/x", "1.6.1", "PPSA99010.zip", "uploaded", SHA_A);
    snprintf(files, sizeof(files), "%s,%s", one, two);
    json = release("1.6.1", PUBLISHED, files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_FOREIGN_FILE);
    free(json);

    /* a file missing, or not uploaded yet */
    asset(one, sizeof(one), OURS, "1.6.1", "PPSA99010.zip", "uploaded", SHA_A);
    json = release("1.6.1", PUBLISHED, one, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_NO_SUMS);
    free(json);
    asset(one, sizeof(one), OURS, "1.6.1", "SHA256SUMS", "uploaded", SHA_A);
    json = release("1.6.1", PUBLISHED, one, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_NO_ZIP);
    free(json);
    asset(one, sizeof(one), OURS, "1.6.1", "PPSA99010.zip", "starter", SHA_A);
    snprintf(files, sizeof(files), "%s,%s", one, two);
    json = release("1.6.1", PUBLISHED, files, "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_NO_ZIP);
    free(json);
    json = release("1.6.1", PUBLISHED, "", "");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_NO_ZIP);
    free(json);

    /* tags that are not versions */
    const char *const tags[] = {"build-cache", "1.6", "1.6.1-rc1", "nightly", ""};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i)
    {
        both(files, sizeof(files), OURS, tags[i], SHA_A);
        json = release(tags[i], PUBLISHED, files, "");
        CHECK(evaluate(json, "1.6.0", &r) == RELEASE_BAD_TAG);
        free(json);
    }
}

static void test_notes_are_not_fields(void)
{
    /* Notes that look like the fields, escaped as GitHub escapes them, change nothing. */
    char files[4096];
    Release r;
    both(files, sizeof(files), OURS, "1.6.1", SHA_A);
    char *json = release("1.6.1", PUBLISHED, files,
                         "Not \\\"tag_name\\\": \\\"9.9.9\\\", \\\"prerelease\\\": true, \\\"draft\\\": true,\\n"
                         "\\\"assets\\\": [{\\\"name\\\": \\\"PPSA99010.zip\\\", \\\"browser_download_url\\\": "
                         "\\\"https://evil.example/PPSA99010.zip\\\"}] \\u00e9\\u65e5 \\\\ \\/ done");
    CHECK(evaluate(json, "1.6.0", &r) == RELEASE_OFFER);
    CHECK(!strcmp(r.version, "1.6.1"));
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/1.6.1/PPSA99010.zip"));
    free(json);

    /* GitHub's own escape of a slash in an address is read as the slash */
    const char *escaped =
        "{\"tag_name\":\"1.6.1\",\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"PPSA99010.zip\",\"state\":\"uploaded\",\"size\":1,\"digest\":null,\"browser_download_url\":"
        "\"https:\\/\\/github.com\\/Oliv3rmoon\\/Dulcamara\\/releases\\/download\\/1.6.1\\/PPSA99010.zip\"},"
        "{\"name\":\"SHA256SUMS\",\"state\":\"uploaded\",\"size\":80,\"digest\":null,\"browser_download_url\":"
        "\"https://github.com/Oliv3rmoon/Dulcamara/releases/download/1.6.1/SHA256SUMS\"}]}";
    CHECK(evaluate(escaped, "1.6.0", &r) == RELEASE_OFFER);
}

static void test_unreadable(void)
{
    Release r;
    const char *const bad[] = {"", "[]", "null", "{", "{\"tag_name\":", "{\"tag_name\":\"1.6.1\"",
                               "{\"tag_name\":\"1.6.1\",\"assets\":[{\"name\":\"PPSA99010.zip\"}",
                               "{\"tag_name\":\"1.6.1\" \"draft\":false}", "{\"draft\":maybe}",
                               "{\"body\":\"\\q\"}", "{\"body\":\"line\nbreak\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(evaluate(bad[i], "1.6.0", &r) == RELEASE_UNREADABLE);
    /* a cut-off answer, at every length */
    char files[4096];
    both(files, sizeof(files), OURS, "1.6.1", SHA_A);
    char *json = release("1.6.1", PUBLISHED, files, "notes");
    size_t n = strlen(json);
    while (n > 0 && (json[n - 1] == '\n' || json[n - 1] == '}'))
        --n; /* everything before the final brace */
    int cut_ok = 1;
    for (size_t len = 0; len < n; ++len)
        if (release_evaluate(json, len, "1.6.0", &r) == RELEASE_OFFER)
            cut_ok = 0;
    CHECK(cut_ok);
    free(json);
}

static void test_list_test_builds(void)
{
    Release r;
    /* the newest that passes is a test build: offered with Test builds on, not with it off */
    char *rel[] = {ours("0.2.10", PRERELEASE), ours("0.2.9", PUBLISHED)};
    char *json = list(rel, 2);
    CHECK(evaluate_list(json, "0.2.9", true, &r) == RELEASE_OFFER);
    CHECK(!strcmp(r.version, "0.2.10") && r.prerelease);
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.10/PPSA99010.zip"));
    CHECK(!strcmp(r.sums_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.10/SHA256SUMS"));
    CHECK(!strcmp(r.zip_digest, SHA_A) && r.zip_size == 18267896u);
    CHECK(evaluate_list(json, "0.2.9", false, &r) == RELEASE_NOT_NEWER);
    CHECK(!strcmp(r.version, "0.2.9") && r.zip_url[0] == '\0');
    /* full releases still count: with it off, the full one when it is newer */
    CHECK(evaluate_list(json, "0.2.8", false, &r) == RELEASE_OFFER && !strcmp(r.version, "0.2.9") && !r.prerelease);
    /* not newer: this version, or an older one */
    CHECK(evaluate_list(json, "0.2.10", true, &r) == RELEASE_NOT_NEWER && !strcmp(r.version, "0.2.10"));
    CHECK(evaluate_list(json, "0.3.0", true, &r) == RELEASE_NOT_NEWER && r.zip_url[0] == '\0');
    discard(rel, 2, json);

    /* with it on, a full release newer than every test build */
    char *full[] = {ours("0.2.11", PRERELEASE), ours("0.3.0", PUBLISHED), ours("0.2.10", PRERELEASE)};
    json = list(full, 3);
    CHECK(evaluate_list(json, "0.2.9", true, &r) == RELEASE_OFFER && !strcmp(r.version, "0.3.0") && !r.prerelease);
    discard(full, 3, json);

    /* a test build on its own, one release object: release_evaluate() still never offers it */
    char *one = ours("0.2.10", PRERELEASE);
    CHECK(evaluate(one, "0.2.9", &r) == RELEASE_NOT_PUBLISHED);
    free(one);
}

static void test_list_passes_over(void)
{
    Release r;
    char files[4096], zip[1024], sums[1024];
    /* newer test builds that fail a check are passed over for an older one that passes them all: darkxex's files,
     * a ZIP on another host, a ZIP of another tag, no SHA256SUMS */
    char *rel[5];
    both(files, sizeof(files), "darkxex/SwanStationPS5", "0.2.14", SHA_A);
    rel[0] = release("0.2.14", PRERELEASE, files, "");
    asset(zip, sizeof(zip), "Oliv3rmoon/Dulcamara.evil.example/x", "0.2.13", "PPSA99010.zip", "uploaded", SHA_A);
    asset(sums, sizeof(sums), OURS, "0.2.13", "SHA256SUMS", "uploaded", NULL);
    snprintf(files, sizeof(files), "%s,%s", zip, sums);
    rel[1] = release("0.2.13", PRERELEASE, files, "");
    asset(zip, sizeof(zip), OURS, "0.2.10", "PPSA99010.zip", "uploaded", SHA_A);
    asset(sums, sizeof(sums), OURS, "0.2.12", "SHA256SUMS", "uploaded", NULL);
    snprintf(files, sizeof(files), "%s,%s", zip, sums);
    rel[2] = release("0.2.12", PRERELEASE, files, "");
    asset(zip, sizeof(zip), OURS, "0.2.11", "PPSA99010.zip", "uploaded", SHA_A);
    rel[3] = release("0.2.11", PRERELEASE, zip, "");
    rel[4] = ours("0.2.10", PRERELEASE);
    char *json = list(rel, 5);
    CHECK(evaluate_list(json, "0.2.9", true, &r) == RELEASE_OFFER);
    CHECK(!strcmp(r.version, "0.2.10"));
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.10/PPSA99010.zip"));
    CHECK(!strcmp(r.sums_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.10/SHA256SUMS"));
    /* with nothing to fall back on, nothing is offered, and the newest says why */
    CHECK(evaluate_list(json, "0.2.10", true, &r) == RELEASE_FOREIGN_FILE);
    CHECK(!strcmp(r.version, "0.2.14") && r.zip_url[0] == '\0' && r.sums_url[0] == '\0');
    discard(rel, 5, json);
}

static void test_list_drafts(void)
{
    Release r;
    /* a draft is never offered, however new, a pre-release or not */
    char *rel[] = {ours("0.2.12", DRAFT), ours("0.2.11", "\"draft\": true, \"prerelease\": true"),
                   ours("0.2.10", PRERELEASE)};
    char *json = list(rel, 3);
    CHECK(evaluate_list(json, "0.2.9", true, &r) == RELEASE_OFFER && !strcmp(r.version, "0.2.10"));
    CHECK(evaluate_list(json, "0.2.9", false, &r) == RELEASE_NOT_PUBLISHED && r.version[0] == '\0');
    discard(rel, 3, json);
    char *draft[] = {ours("0.2.12", DRAFT)};
    json = list(draft, 1);
    CHECK(evaluate_list(json, "0.2.9", true, &r) == RELEASE_NOT_PUBLISHED);
    CHECK(r.version[0] == '\0' && r.zip_url[0] == '\0');
    discard(draft, 1, json);
}

static void test_list_order(void)
{
    Release r;
    /* by version, not as text and not as listed: 0.2.10 is newer than 0.2.9 */
    char *rel[] = {ours("0.2.9", PRERELEASE), ours("0.2.10", PRERELEASE), ours("0.2.1", PRERELEASE)};
    char *json = list(rel, 3);
    CHECK(evaluate_list(json, "0.2.8", true, &r) == RELEASE_OFFER && !strcmp(r.version, "0.2.10"));
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/0.2.10/PPSA99010.zip"));
    discard(rel, 3, json);
    char *other[] = {ours("0.2.10", PRERELEASE), ours("v0.2.9", PUBLISHED), ours("0.10.0", PRERELEASE),
                     ours("0.9.99", PRERELEASE)};
    json = list(other, 4);
    CHECK(evaluate_list(json, "0.2.8", true, &r) == RELEASE_OFFER && !strcmp(r.version, "0.10.0"));
    CHECK(evaluate_list(json, "0.2.8", false, &r) == RELEASE_OFFER && !strcmp(r.version, "0.2.9"));
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/v0.2.9/PPSA99010.zip"));
    /* not newer: the newest of them is named */
    CHECK(evaluate_list(json, "0.10.0", true, &r) == RELEASE_NOT_NEWER && !strcmp(r.version, "0.10.0"));
    discard(other, 4, json);
    /* the same version twice (v0.2.10 and 0.2.10): the one listed first, the most recent, is offered */
    char *twice[] = {ours("v0.2.10", PRERELEASE), ours("0.2.10", PRERELEASE)};
    json = list(twice, 2);
    CHECK(evaluate_list(json, "0.2.9", true, &r) == RELEASE_OFFER);
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Dulcamara/releases/download/v0.2.10/PPSA99010.zip"));
    discard(twice, 2, json);
}

static void test_sums_of_another_tag(void)
{
    Release r;
    /* the ZIP is the tag's own but SHA256SUMS another tag's, or another host's: never offered, alone or listed */
    char files[4096], zip[1024], sums[1024];
    asset(zip, sizeof(zip), OURS, "0.2.10", "PPSA99010.zip", "uploaded", SHA_A);
    asset(sums, sizeof(sums), OURS, "0.2.9", "SHA256SUMS", "uploaded", NULL);
    snprintf(files, sizeof(files), "%s,%s", zip, sums);
    char *json = release("0.2.10", PUBLISHED, files, "");
    CHECK(evaluate(json, "0.2.8", &r) == RELEASE_FOREIGN_FILE && r.sums_url[0] == '\0');
    char *rel[] = {release("0.2.10", PRERELEASE, files, "")};
    char *listed = list(rel, 1);
    CHECK(evaluate_list(listed, "0.2.8", true, &r) == RELEASE_FOREIGN_FILE && r.zip_url[0] == '\0');
    discard(rel, 1, listed);
    free(json);
    asset(sums, sizeof(sums), "Oliv3rmoon/Dulcamara.evil.example/x", "0.2.10", "SHA256SUMS", "uploaded", NULL);
    snprintf(files, sizeof(files), "%s,%s", zip, sums);
    json = release("0.2.10", PUBLISHED, files, "");
    CHECK(evaluate(json, "0.2.8", &r) == RELEASE_FOREIGN_FILE);
    free(json);
}

static void test_list_unreadable(void)
{
    Release r;
    /* an empty list: nothing newer */
    CHECK(evaluate_list("[]", "0.2.9", true, &r) == RELEASE_NOT_NEWER && r.version[0] == '\0' && r.zip_url[0] == '\0');
    CHECK(evaluate_list(" [ ] ", "0.2.9", true, &r) == RELEASE_NOT_NEWER);
    /* a release that is not a version: no offer */
    CHECK(evaluate_list("[{}]", "0.2.9", true, &r) == RELEASE_BAD_TAG);
    const char *const bad[] = {"",
                               "{}",
                               "null",
                               "[",
                               "[{}",
                               "[{},]",
                               "[,]",
                               "[1]",
                               "[null]",
                               "[\"0.2.10\"]",
                               "[{\"draft\":\"no\"}]",
                               "[{\"tag_name\":\"0.2.10\"},{\"tag_name\":}]",
                               "[{\"tag_name\":\"0.2.10\"} {\"tag_name\":\"0.2.11\"}]",
                               "[{\"tag_name\":5}]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(evaluate_list(bad[i], "0.2.9", true, &r) == RELEASE_UNREADABLE && r.zip_url[0] == '\0');
    /* releases/latest's answer, one release, is not a list */
    char *one = ours("0.2.10", PRERELEASE);
    CHECK(evaluate_list(one, "0.2.9", true, &r) == RELEASE_UNREADABLE);
    /* one that would be offered, then one that can't be read: nothing */
    size_t size = strlen(one) + 64;
    char *json = malloc(size);
    snprintf(json, size, "[%s,{\"tag_name\":\"0.2.11\",\"draft\":maybe}]", one);
    CHECK(evaluate_list(json, "0.2.9", true, &r) == RELEASE_UNREADABLE && r.zip_url[0] == '\0');
    free(json);
    free(one);
    /* a cut-off list, at every length */
    char *rel[] = {ours("0.2.10", PRERELEASE), ours("0.2.9", PUBLISHED)};
    json = list(rel, 2);
    const size_t n = strlen(json) - 1; /* everything before the final bracket */
    int cut_ok = 1;
    for (size_t len = 0; len < n; ++len)
        if (release_evaluate_list(json, len, "0.2.8", true, &r) != RELEASE_UNREADABLE || r.zip_url[0])
            cut_ok = 0;
    CHECK(cut_ok);
    CHECK(release_evaluate_list(json, n + 1, "0.2.8", true, &r) == RELEASE_OFFER);
    discard(rel, 2, json);
}

static void test_sums(void)
{
    char sha[65], text[512];
    snprintf(text, sizeof(text), "%s  PPSA99010.zip\n", SHA_A);
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == 1 && !strcmp(sha, SHA_A));
    snprintf(text, sizeof(text), "%s *PPSA99010.zip", SHA_A);
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == 1 && !strcmp(sha, SHA_A));
    snprintf(text, sizeof(text), "%s  SwanStationPS5-DiscDaemon.elf\r\n%s  PPSA99010.zip\r\n", SHA_B, SHA_A);
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == 1 && !strcmp(sha, SHA_A));
    snprintf(text, sizeof(text), "4DEF0FD289A935CB37B5DF04BF19DBC3523C8A2DE8C7B1F92020EA0D63C77BCA  PPSA99010.zip\n");
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == 1 && !strcmp(sha, SHA_A));
    snprintf(text, sizeof(text), "%s  SwanStationPS5-DiscDaemon.elf\n%s  XPPSA99010.zip\n", SHA_B, SHA_A);
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == 0);
    CHECK(release_sums_lookup("", 0, "PPSA99010.zip", sha) == 0);
    snprintf(text, sizeof(text), "%s  PPSA99010.zip\n%s  PPSA99010.zip\n", SHA_A, SHA_B);
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == -1);
    snprintf(text, sizeof(text), "4def0fd2  PPSA99010.zip\n");
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == -1);
    snprintf(text, sizeof(text), "%.63sg  PPSA99010.zip\n", SHA_A);
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == -1);
    snprintf(text, sizeof(text), "%s PPSA99010.zip\n", SHA_A); /* one space */
    CHECK(release_sums_lookup(text, strlen(text), "PPSA99010.zip", sha) == -1);
}

static void test_file_sha256(const char *dir)
{
    char path[1024], hex[65];
    snprintf(path, sizeof(path), "%s/empty", dir);
    FILE *f = fopen(path, "wb");
    fclose(f);
    CHECK(release_file_sha256(path, hex) &&
          !strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    snprintf(path, sizeof(path), "%s/abc", dir);
    f = fopen(path, "wb");
    fputs("abc", f);
    fclose(f);
    CHECK(release_file_sha256(path, hex) &&
          !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* a million "a", across the reader's chunks */
    snprintf(path, sizeof(path), "%s/million", dir);
    f = fopen(path, "wb");
    for (int i = 0; i < 1000000; ++i)
        fputc('a', f);
    fclose(f);
    CHECK(release_file_sha256(path, hex) &&
          !strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    snprintf(path, sizeof(path), "%s/missing", dir);
    CHECK(!release_file_sha256(path, hex));
}

int main(int argc, char **argv)
{
    test_versions();
    test_offer();
    test_not_offered();
    test_notes_are_not_fields();
    test_unreadable();
    test_list_test_builds();
    test_list_passes_over();
    test_list_drafts();
    test_list_order();
    test_sums_of_another_tag();
    test_list_unreadable();
    test_sums();
    test_file_sha256(argc > 1 ? argv[1] : ".");
    CHECK(!strcmp(UPDATE_REPO, "Oliv3rmoon/Dulcamara"));
    CHECK(!strcmp(UPDATE_LATEST_URL, "https://api.github.com/repos/Oliv3rmoon/Dulcamara/releases/latest"));
    CHECK(!strcmp(UPDATE_LIST_URL, "https://api.github.com/repos/Oliv3rmoon/Dulcamara/releases?per_page=20"));
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
