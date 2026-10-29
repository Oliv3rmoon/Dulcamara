/*
 * Nightshade Player - which GitHub release the in-app update offers, and the checks it passes.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See update_release.h. The JSON is read with a small scanner that understands the whole grammar
 * (strings with escapes, nested objects and arrays), so text in a release's notes can never be
 * mistaken for one of its fields.
 */
#include "update_release.h"

#include "../examples/self-update/self_update_sha256.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- versions */

bool release_version_parse(const char *text, int out[3])
{
    const char *p = text;
    for (int i = 0; i < 3; ++i)
    {
        if (*p < '0' || *p > '9')
            return false;
        int value = 0, digits = 0;
        while (*p >= '0' && *p <= '9')
        {
            if (++digits > 5)
                return false;
            value = value * 10 + (*p++ - '0');
        }
        out[i] = value;
        if (i < 2 && *p++ != '.')
            return false;
    }
    return *p == '\0';
}

int release_version_compare(const char *a, const char *b)
{
    int x[3], y[3];
    if (!release_version_parse(a, x) || !release_version_parse(b, y))
        return 0;
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

/* ---------------------------------------------------------------- a JSON scanner */

typedef struct
{
    const char *p, *end;
} Json;

static void json_space(Json *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        ++j->p;
}

static bool json_take(Json *j, char c)
{
    json_space(j);
    if (j->p < j->end && *j->p == c)
    {
        ++j->p;
        return true;
    }
    return false;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* A string. With out, its text (an escaped character outside ASCII becomes '?'); *fits is false
 * when it was longer than size - 1 (out is then ""). False when it is not a well-formed string. */
static bool json_string(Json *j, char *out, size_t size, bool *fits)
{
    size_t n = 0;
    bool room = true;
    if (!json_take(j, '"'))
        return false;
    while (j->p < j->end && *j->p != '"')
    {
        char c = *j->p++;
        if ((unsigned char)c < 0x20)
            return false;
        if (c == '\\')
        {
            if (j->p >= j->end)
                return false;
            char e = *j->p++;
            switch (e)
            {
            case '"': case '\\': case '/': c = e; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
            {
                int code = 0;
                for (int k = 0; k < 4; ++k)
                {
                    int d = j->p < j->end ? hex_digit(*j->p++) : -1;
                    if (d < 0)
                        return false;
                    code = code * 16 + d;
                }
                c = code > 0 && code < 0x80 ? (char)code : '?';
                break;
            }
            default: return false;
            }
        }
        if (out)
        {
            if (n + 1 < size)
                out[n++] = c;
            else
                room = false;
        }
    }
    if (j->p >= j->end)
        return false;
    ++j->p; /* the closing quote */
    if (out && size)
        out[room ? n : 0] = '\0';
    if (fits)
        *fits = room;
    return true;
}

/* A number, true, false or null: the characters they are made of. */
static bool json_word(Json *j, char *out, size_t size)
{
    json_space(j);
    size_t n = 0;
    while (j->p < j->end && (strchr("+-.0123456789eE", *j->p) || (*j->p >= 'a' && *j->p <= 'z')))
    {
        if (n + 1 >= size)
            return false;
        out[n++] = *j->p++;
    }
    out[n] = '\0';
    return n > 0;
}

static bool json_skip(Json *j, int depth)
{
    if (depth > 32)
        return false;
    json_space(j);
    if (j->p >= j->end)
        return false;
    if (*j->p == '"')
        return json_string(j, NULL, 0, NULL);
    if (*j->p == '{' || *j->p == '[')
    {
        const char close = *j->p == '{' ? '}' : ']';
        const bool object = close == '}';
        ++j->p;
        if (json_take(j, close))
            return true;
        do
        {
            if (object && (!json_string(j, NULL, 0, NULL) || !json_take(j, ':')))
                return false;
            if (!json_skip(j, depth + 1))
                return false;
        } while (json_take(j, ','));
        return json_take(j, close);
    }
    char word[64];
    return json_word(j, word, sizeof(word));
}

static bool json_bool(Json *j, bool *out)
{
    char word[8];
    if (!json_word(j, word, sizeof(word)))
        return false;
    if (!strcmp(word, "true") || !strcmp(word, "false") || !strcmp(word, "null"))
    {
        *out = !strcmp(word, "true");
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- the release */

typedef struct
{
    char name[64], url[256], digest[80], state[16];
    uint64_t size;
    bool found;
} Asset;

static bool read_asset(Json *j, Asset *a)
{
    memset(a, 0, sizeof(*a));
    if (!json_take(j, '{'))
        return false;
    if (json_take(j, '}'))
        return true;
    do
    {
        char key[32];
        bool fits = true;
        if (!json_string(j, key, sizeof(key), &fits) || !json_take(j, ':'))
            return false;
        json_space(j);
        const bool is_string = j->p < j->end && *j->p == '"';
        char *field = NULL;
        size_t size = 0;
        if (fits && !strcmp(key, "name"))
            field = a->name, size = sizeof(a->name);
        else if (fits && !strcmp(key, "browser_download_url"))
            field = a->url, size = sizeof(a->url);
        else if (fits && !strcmp(key, "digest"))
            field = a->digest, size = sizeof(a->digest);
        else if (fits && !strcmp(key, "state"))
            field = a->state, size = sizeof(a->state);
        if (field && is_string)
        {
            bool value_fits = true;
            if (!json_string(j, field, size, &value_fits))
                return false;
        }
        else if (fits && !strcmp(key, "size") && !is_string)
        {
            char word[24];
            if (!json_word(j, word, sizeof(word)))
                return false;
            uint64_t v = 0;
            for (const char *c = word; *c; ++c)
            {
                if (*c < '0' || *c > '9' || v > UINT64_MAX / 10)
                    return false;
                v = v * 10 + (uint64_t)(*c - '0');
            }
            a->size = v;
        }
        else if (!json_skip(j, 2))
            return false;
    } while (json_take(j, ','));
    return json_take(j, '}');
}

/* "sha256:<64 hex>" -> the hex, lowercase; anything else -> "". */
static void digest_hex(const char *digest, char out[65])
{
    out[0] = '\0';
    if (strncmp(digest, "sha256:", 7) != 0 || strlen(digest + 7) != 64)
        return;
    for (int i = 0; i < 64; ++i)
    {
        char c = digest[7 + i];
        if (hex_digit(c) < 0)
        {
            out[0] = '\0';
            return;
        }
        out[i] = c >= 'A' && c <= 'F' ? (char)(c - 'A' + 'a') : c;
    }
    out[64] = '\0';
}

ReleaseVerdict release_evaluate(const char *json, size_t length, const char *installed, Release *out)
{
    memset(out, 0, sizeof(*out));
    Json j = {json, json + length};
    char tag[40] = "";
    bool tag_fits = true, draft = false, prerelease = false;
    Asset zip = {0}, sums = {0};
    if (!json_take(&j, '{'))
        return RELEASE_UNREADABLE;
    if (!json_take(&j, '}'))
    {
        do
        {
            char key[32];
            bool fits = true;
            if (!json_string(&j, key, sizeof(key), &fits) || !json_take(&j, ':'))
                return RELEASE_UNREADABLE;
            if (fits && !strcmp(key, "tag_name"))
            {
                if (!json_string(&j, tag, sizeof(tag), &tag_fits))
                    return RELEASE_UNREADABLE;
            }
            else if (fits && !strcmp(key, "draft"))
            {
                if (!json_bool(&j, &draft))
                    return RELEASE_UNREADABLE;
            }
            else if (fits && !strcmp(key, "prerelease"))
            {
                if (!json_bool(&j, &prerelease))
                    return RELEASE_UNREADABLE;
            }
            else if (fits && !strcmp(key, "assets"))
            {
                if (!json_take(&j, '['))
                    return RELEASE_UNREADABLE;
                if (!json_take(&j, ']'))
                {
                    do
                    {
                        Asset a;
                        if (!read_asset(&j, &a))
                            return RELEASE_UNREADABLE;
                        Asset *slot = !strcmp(a.name, UPDATE_ZIP) ? &zip : !strcmp(a.name, UPDATE_SUMS) ? &sums : NULL;
                        if (slot && !slot->found && !strcmp(a.state, "uploaded"))
                        {
                            *slot = a;
                            slot->found = true;
                        }
                    } while (json_take(&j, ','));
                    if (!json_take(&j, ']'))
                        return RELEASE_UNREADABLE;
                }
            }
            else if (!json_skip(&j, 1))
                return RELEASE_UNREADABLE;
        } while (json_take(&j, ','));
        if (!json_take(&j, '}'))
            return RELEASE_UNREADABLE;
    }
    if (draft || prerelease)
        return RELEASE_NOT_PUBLISHED;
    const char *version = tag[0] == 'v' ? tag + 1 : tag;
    int parts[3];
    if (!tag_fits || !release_version_parse(version, parts))
        return RELEASE_BAD_TAG;
    snprintf(out->version, sizeof(out->version), "%s", version);
    if (release_version_compare(version, installed) <= 0)
        return RELEASE_NOT_NEWER;
    if (!zip.found)
        return RELEASE_NO_ZIP;
    if (!sums.found)
        return RELEASE_NO_SUMS;
    /* Both files must be this repository's own, for this tag: never a file anywhere else. */
    char want_zip[256], want_sums[256];
    snprintf(want_zip, sizeof(want_zip), "%s%s/%s", UPDATE_DOWNLOAD_PREFIX, tag, UPDATE_ZIP);
    snprintf(want_sums, sizeof(want_sums), "%s%s/%s", UPDATE_DOWNLOAD_PREFIX, tag, UPDATE_SUMS);
    if (strcmp(zip.url, want_zip) != 0 || strcmp(sums.url, want_sums) != 0)
        return RELEASE_FOREIGN_FILE;
    snprintf(out->zip_url, sizeof(out->zip_url), "%s", want_zip);
    snprintf(out->sums_url, sizeof(out->sums_url), "%s", want_sums);
    digest_hex(zip.digest, out->zip_digest);
    out->zip_size = zip.size;
    return RELEASE_OFFER;
}

/* ---------------------------------------------------------------- SHA256SUMS */

int release_sums_lookup(const char *text, size_t length, const char *file, char sha256[65])
{
    const size_t file_len = strlen(file);
    int found = 0;
    const char *p = text, *end = text + length;
    while (p < end)
    {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        const char *next = eol ? eol + 1 : end;
        if (line_end > p && line_end[-1] == '\r')
            --line_end;
        const size_t n = (size_t)(line_end - p);
        /* a line for `file` ends with " <file>" or "*<file>" */
        if (file_len && n > file_len && !memcmp(line_end - file_len, file, file_len) &&
            (line_end[-(long)file_len - 1] == ' ' || line_end[-(long)file_len - 1] == '*'))
        {
            bool ok = n == 64 + 2 + file_len && p[64] == ' ' && (p[65] == ' ' || p[65] == '*');
            for (int i = 0; ok && i < 64; ++i)
                ok = hex_digit(p[i]) >= 0;
            if (!ok || ++found > 1)
                return -1;
            for (int i = 0; i < 64; ++i)
                sha256[i] = p[i] >= 'A' && p[i] <= 'F' ? (char)(p[i] - 'A' + 'a') : p[i];
            sha256[64] = '\0';
        }
        p = next;
    }
    return found;
}

bool release_file_sha256(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    static unsigned char chunk[64 * 1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        self_update_sha256_add(&hash, chunk, n);
    const bool ok = !ferror(f);
    fclose(f);
    if (!ok)
        return false;
    unsigned char digest[32];
    self_update_sha256_finish(&hash, digest);
    self_update_sha256_hex(digest, hex);
    return true;
}

const char *release_verdict_text(ReleaseVerdict verdict)
{
    switch (verdict)
    {
    case RELEASE_OFFER: return "newer, offered";
    case RELEASE_NOT_NEWER: return "not newer";
    case RELEASE_UNREADABLE: return "an answer that can't be read";
    case RELEASE_NOT_PUBLISHED: return "a draft or a pre-release";
    case RELEASE_BAD_TAG: return "its tag is not a version";
    case RELEASE_NO_ZIP: return "no " UPDATE_ZIP;
    case RELEASE_NO_SUMS: return "no " UPDATE_SUMS;
    case RELEASE_FOREIGN_FILE: return "a file from elsewhere";
    }
    return "?";
}
