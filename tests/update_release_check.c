/*
 * Nightshade Player - host checks for the update's release decision (src/update_release.c).
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built and run by tests/test_update_release.py: the answers are shaped like GitHub's
 * releases/latest, the files like the release workflow's (.github/workflows/build.yml).
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
             "{\n  \"url\": \"https://api.github.com/repos/Oliv3rmoon/Nightshade-Player/releases/1\",\n"
             "  \"html_url\": \"https://github.com/Oliv3rmoon/Nightshade-Player/releases/tag/%s\",\n"
             "  \"id\": 1,\n  \"author\": {\"login\": \"github-actions[bot]\", \"id\": 41898282, \"type\": \"Bot\"},\n"
             "  \"node_id\": \"RE_x\",\n  \"tag_name\": \"%s\",\n  \"target_commitish\": \"main\",\n"
             "  \"name\": \"Nightshade Player %s\",\n  %s,\n  \"immutable\": false,\n"
             "  \"created_at\": \"2026-10-28T00:00:00Z\",\n  \"published_at\": \"2026-10-28T00:00:00Z\",\n"
             "  \"assets\": [%s],\n"
             "  \"tarball_url\": \"https://api.github.com/repos/Oliv3rmoon/Nightshade-Player/tarball/%s\",\n"
             "  \"zipball_url\": \"https://api.github.com/repos/Oliv3rmoon/Nightshade-Player/zipball/%s\",\n"
             "  \"body\": \"%s\",\n  \"reactions\": {\"total_count\": 0, \"+1\": 0}\n}\n",
             tag, tag, tag, flags, assets, tag, tag, body);
    return out;
}

static const char *const PUBLISHED = "\"draft\": false, \"prerelease\": false";
static const char *const OURS = "Oliv3rmoon/Nightshade-Player";

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
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Nightshade-Player/releases/download/1.6.1/PPSA99010.zip"));
    CHECK(!strcmp(r.sums_url, "https://github.com/Oliv3rmoon/Nightshade-Player/releases/download/1.6.1/SHA256SUMS"));
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
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Nightshade-Player/releases/download/v1.7.0/PPSA99010.zip"));
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

    /* Nightshade Station's release (another app of the same author): never offered */
    both(files, sizeof(files), "Oliv3rmoon/Nightshade-Station", "1.9.0", SHA_A);
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
    asset(one, sizeof(one), "Oliv3rmoon/Nightshade-Player.evil.example/x", "1.6.1", "PPSA99010.zip", "uploaded", SHA_A);
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
    CHECK(!strcmp(r.zip_url, "https://github.com/Oliv3rmoon/Nightshade-Player/releases/download/1.6.1/PPSA99010.zip"));
    free(json);

    /* GitHub's own escape of a slash in an address is read as the slash */
    const char *escaped =
        "{\"tag_name\":\"1.6.1\",\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"PPSA99010.zip\",\"state\":\"uploaded\",\"size\":1,\"digest\":null,\"browser_download_url\":"
        "\"https:\\/\\/github.com\\/Oliv3rmoon\\/Nightshade-Player\\/releases\\/download\\/1.6.1\\/PPSA99010.zip\"},"
        "{\"name\":\"SHA256SUMS\",\"state\":\"uploaded\",\"size\":80,\"digest\":null,\"browser_download_url\":"
        "\"https://github.com/Oliv3rmoon/Nightshade-Player/releases/download/1.6.1/SHA256SUMS\"}]}";
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
    test_sums();
    test_file_sha256(argc > 1 ? argv[1] : ".");
    CHECK(!strcmp(UPDATE_REPO, "Oliv3rmoon/Nightshade-Player"));
    CHECK(!strcmp(UPDATE_LATEST_URL, "https://api.github.com/repos/Oliv3rmoon/Nightshade-Player/releases/latest"));
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
