/*
 * Dulcamara - insider builds: the request and the answer (insider_check.h).
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The answer is read with a small scanner that understands the whole JSON grammar (strings with escapes, nested
 * objects and arrays), so text in a build's notes can never be mistaken for one of its fields.
 */
#include "insider_check.h"

#include "update_release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- the code */

bool insider_code_normalize(const char *text, char code[INSIDER_CODE_LENGTH + 1])
{
    int n = 0;
    code[0] = '\0';
    for (const char *p = text; *p; ++p)
    {
        char c = *p;
        if (c == ' ' || c == '-')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (n == INSIDER_CODE_LENGTH || !strchr(INSIDER_ALPHABET, c))
        {
            code[0] = '\0'; /* refused, not repaired */
            return false;
        }
        code[n++] = c;
    }
    code[n] = '\0';
    if (n != INSIDER_CODE_LENGTH)
        code[0] = '\0';
    return n == INSIDER_CODE_LENGTH;
}

/* ---------------------------------------------------------------- the request */

/* s as a JSON string's characters, quotes and backslashes escaped and control characters as \u00XX; false when out
 * is full. */
static bool json_escape(char *out, size_t size, size_t *at, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
    {
        char piece[8];
        if (*p == '"' || *p == '\\')
            snprintf(piece, sizeof(piece), "\\%c", *p);
        else if (*p < 0x20 || *p == 0x7f)
            snprintf(piece, sizeof(piece), "\\u%04x", *p);
        else
            piece[0] = (char)*p, piece[1] = '\0';
        const size_t n = strlen(piece);
        if (*at + n >= size)
            return false;
        memcpy(out + *at, piece, n);
        *at += n;
    }
    out[*at] = '\0';
    return true;
}

static bool append(char *out, size_t size, size_t *at, const char *s)
{
    const size_t n = strlen(s);
    if (*at + n >= size)
        return false;
    memcpy(out + *at, s, n + 1);
    *at += n;
    return true;
}

bool insider_request_json(char *out, size_t size, const char *installed, const char *code)
{
    size_t at = 0;
    if (!size)
        return false;
    out[0] = '\0';
    const bool ok = append(out, size, &at, "{\"app\":\"" INSIDER_APP "\",\"installed_version\":\"") &&
                    json_escape(out, size, &at, installed) && append(out, size, &at, "\",\"code\":\"") &&
                    json_escape(out, size, &at, code) && append(out, size, &at, "\"}");
    if (!ok)
        out[0] = '\0';
    return ok;
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

static bool json_at(Json *j, char c)
{
    json_space(j);
    return j->p < j->end && *j->p == c;
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

/* A string. With out, its text (an escaped character outside ASCII becomes '?'); *fits is false when it was longer
 * than size - 1 (out is then ""). False when it is not a well-formed string. */
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
            const char e = *j->p++;
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
                    const int d = j->p < j->end ? hex_digit(*j->p++) : -1;
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
    while (j->p < j->end && (strchr("+-.0123456789eE", *j->p) || (*j->p >= 'a' && *j->p <= 'z')) && *j->p)
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

/* A string field's value into out (size bytes), or, when it isn't a string or doesn't fit, "" and *bad set; false
 * when it can't be read at all. */
static bool read_text(Json *j, char *out, size_t size, bool *bad)
{
    out[0] = '\0';
    if (!json_at(j, '"'))
    {
        *bad = true;
        return json_skip(j, 1);
    }
    bool fits = true;
    if (!json_string(j, out, size, &fits))
        return false;
    if (!fits)
        *bad = true;
    return true;
}

/* ---------------------------------------------------------------- the answer */

typedef struct
{
    char version[24], file[80], url[INSIDER_URL_MAX + 2], sha256[80], size[24];
    bool version_bad, file_bad, url_bad, sha256_bad, size_bad;
    bool has_version, has_url, has_sha256, has_size;
} RawBuild;

static bool read_build(Json *j, RawBuild *b)
{
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
        bool ok;
        if (fits && !strcmp(key, "version"))
        {
            b->has_version = true, b->version_bad = false;
            ok = read_text(j, b->version, sizeof(b->version), &b->version_bad);
        }
        else if (fits && !strcmp(key, "file"))
        {
            b->file_bad = false;
            ok = read_text(j, b->file, sizeof(b->file), &b->file_bad);
        }
        else if (fits && !strcmp(key, "url"))
        {
            b->has_url = true, b->url_bad = false;
            ok = read_text(j, b->url, sizeof(b->url), &b->url_bad);
        }
        else if (fits && !strcmp(key, "sha256"))
        {
            b->has_sha256 = true, b->sha256_bad = false;
            ok = read_text(j, b->sha256, sizeof(b->sha256), &b->sha256_bad);
        }
        else if (fits && !strcmp(key, "size"))
        {
            /* a number: a string, an object or an array is no size */
            b->has_size = true, b->size_bad = false;
            b->size[0] = '\0';
            if (json_at(j, '"') || json_at(j, '{') || json_at(j, '['))
            {
                b->size_bad = true;
                ok = json_skip(j, 1);
            }
            else
                ok = json_word(j, b->size, sizeof(b->size));
        }
        else
            ok = json_skip(j, 1);
        if (!ok)
            return false;
    } while (json_take(j, ','));
    return json_take(j, '}');
}

/* A size: a whole number of bytes above 0, digits only (no sign, fraction or exponent). */
static bool parse_size(const char *word, uint64_t *out)
{
    uint64_t v = 0;
    if (!word[0])
        return false;
    for (const char *c = word; *c; ++c)
    {
        if (*c < '0' || *c > '9' || v > (UINT64_MAX - 9) / 10)
            return false;
        v = v * 10 + (uint64_t)(*c - '0');
    }
    *out = v;
    return v > 0;
}

static bool url_characters_ok(const char *url)
{
    for (const char *c = url; *c; ++c)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
              *c == '_' || *c == '.' || *c == '/' || *c == ':'))
            return false;
    return true;
}

static bool sha256_ok(const char *sha)
{
    if (strlen(sha) != 64)
        return false;
    for (int i = 0; i < 64; ++i)
        if (!((sha[i] >= '0' && sha[i] <= '9') || (sha[i] >= 'a' && sha[i] <= 'f')))
            return false;
    return true;
}

static bool file_name_ok(const char *file)
{
    if (!file[0] || strlen(file) >= sizeof(((InsiderBuild *)0)->file))
        return false;
    for (const char *c = file; *c; ++c)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
              *c == '_' || *c == '.'))
            return false;
    return true;
}

/* After the host: an empty segment ("//") or a dot segment ("/./", "/../", or one at the end), which a client may
 * resolve to another path. */
static bool odd_path(const char *path)
{
    for (const char *c = path; *c; ++c)
        if (c[0] == '/' && (c[1] == '/' || (c[1] == '.' && (c[2] == '/' || c[2] == '\0' ||
                                                            (c[2] == '.' && (c[3] == '/' || c[3] == '\0'))))))
            return true;
    return false;
}

bool insider_owns_url(const char *base, const char *url)
{
    const size_t n = strlen(base), d = strlen(INSIDER_DOWNLOAD_PATH);
    return n > 0 && !strncmp(url, base, n) && !strncmp(url + n, INSIDER_DOWNLOAD_PATH, d);
}

/* The build's checks (insider_check.h); NULL when it passes them, else which it failed. */
static const char *judge_build(const RawBuild *b, const char *base, const char *installed, InsiderBuild *out)
{
    if (!b->has_url || b->url_bad || !b->url[0])
        return "no download address";
    if (strlen(b->url) > INSIDER_URL_MAX)
        return "a download address too long";
    if (!url_characters_ok(b->url))
        return "a download address with characters it can't have";
    if (!insider_owns_url(base, b->url) || !b->url[strlen(base) + strlen(INSIDER_DOWNLOAD_PATH)] ||
        odd_path(b->url + strlen(base)))
        return "a download address that isn't the insider service's";
    if (!b->has_sha256 || b->sha256_bad || !sha256_ok(b->sha256))
        return "no SHA-256 of 64 lowercase hex digits";
    uint64_t size = 0;
    if (!b->has_size || b->size_bad || !parse_size(b->size, &size))
        return "no size above 0";
    if (!b->has_version || b->version_bad)
        return "no version";
    const char *version = b->version[0] == 'v' ? b->version + 1 : b->version;
    int parts[3];
    if (strlen(version) >= sizeof(out->version) || !release_version_parse(version, parts))
        return "a version that isn't X.Y.Z";
    if (release_version_compare(version, installed) <= 0)
        return "not newer than this version";
    snprintf(out->version, sizeof(out->version), "%s", version);
    snprintf(out->url, sizeof(out->url), "%s", b->url);
    snprintf(out->sha256, sizeof(out->sha256), "%s", b->sha256);
    if (!b->file_bad && file_name_ok(b->file))
        snprintf(out->file, sizeof(out->file), "%s", b->file);
    out->size = size;
    return NULL;
}

static bool status_word_ok(const char *s)
{
    if (!s[0])
        return false;
    for (const char *c = s; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || *c == '_'))
            return false;
    return true;
}

InsiderResult insider_read_answer(int http, const char *json, size_t length, const char *base, const char *installed,
                                  InsiderBuild *build)
{
    InsiderResult result = {INSIDER_MALFORMED, http, "", NULL};
    memset(build, 0, sizeof(*build));
    if (http < 0)
    {
        result.status = INSIDER_OFFLINE;
        return result;
    }
    /* the whole answer: one object, its "status", "app" and "build" read, anything else skipped */
    char status[32] = "", app[40] = "";
    bool status_bad = false, app_bad = false, has_app = false, has_build = false;
    RawBuild raw;
    memset(&raw, 0, sizeof(raw));
    Json j = {json ? json : "", (json ? json : "") + (json ? length : 0)};
    bool readable = json_take(&j, '{');
    if (readable && !json_take(&j, '}'))
    {
        do
        {
            char key[32];
            bool fits = true;
            if (!json_string(&j, key, sizeof(key), &fits) || !json_take(&j, ':'))
            {
                readable = false;
                break;
            }
            if (fits && !strcmp(key, "status"))
            {
                status_bad = false;
                readable = read_text(&j, status, sizeof(status), &status_bad);
            }
            else if (fits && !strcmp(key, "app"))
            {
                has_app = true;
                app_bad = false;
                readable = read_text(&j, app, sizeof(app), &app_bad);
            }
            else if (fits && !strcmp(key, "build") && json_at(&j, '{'))
            {
                has_build = true;
                memset(&raw, 0, sizeof(raw));
                readable = read_build(&j, &raw);
            }
            else
                readable = json_skip(&j, 1);
        } while (readable && json_take(&j, ','));
        readable = readable && json_take(&j, '}');
    }
    json_space(&j);
    readable = readable && j.p == j.end; /* nothing after it */
    if (readable && !status_bad && status_word_ok(status))
        snprintf(result.answer, sizeof(result.answer), "%s", status);
    if (http != 200)
    {
        result.status = INSIDER_HTTP_ERROR;
        return result;
    }
    if (!readable || status_bad || !status[0])
    {
        result.answer[0] = '\0';
        result.status = INSIDER_MALFORMED;
        return result;
    }
    if (!strcmp(status, "update"))
    {
        InsiderBuild offered;
        memset(&offered, 0, sizeof(offered));
        result.why = !has_build                                     ? "no build"
                     : has_app && (app_bad || strcmp(app, INSIDER_APP)) ? "a build for another app"
                                                                     : judge_build(&raw, base, installed, &offered);
        result.status = result.why ? INSIDER_BAD_BUILD : INSIDER_UPDATE;
        if (!result.why)
            *build = offered;
        return result;
    }
    result.status = !strcmp(status, "none")            ? INSIDER_NONE
                    : !strcmp(status, "not_qualified") ? INSIDER_NOT_QUALIFIED
                    : !strcmp(status, "invalid_code")  ? INSIDER_INVALID_CODE
                    : !strcmp(status, "expired_code")  ? INSIDER_EXPIRED_CODE
                                                       : INSIDER_OTHER;
    return result;
}

/* ---------------------------------------------------------------- the update check's first step */

InsiderNext insider_decide(const char *base, bool test_builds, const char *code, const char *installed,
                           InsiderRequest request, void *user, InsiderBuild *build, InsiderResult *result)
{
    memset(build, 0, sizeof(*build));
    memset(result, 0, sizeof(*result));
    result->status = INSIDER_OFF;
    char normal[INSIDER_CODE_LENGTH + 1];
    if (!base || !base[0] || !test_builds || !code || !insider_code_normalize(code, normal) || !request)
        return INSIDER_NEXT_GITHUB;
    char url[INSIDER_URL_MAX + 1], json[256];
    if ((size_t)snprintf(url, sizeof(url), "%s" INSIDER_UPDATES_PATH, base) >= sizeof(url) ||
        !insider_request_json(json, sizeof(json), installed, normal))
        return INSIDER_NEXT_GITHUB;
    char *body = NULL;
    size_t length = 0;
    const int http = request(user, url, json, &body, &length);
    *result = insider_read_answer(http, body, body ? length : 0, base, installed, build);
    free(body);
    return result->status == INSIDER_UPDATE ? INSIDER_NEXT_OFFER : INSIDER_NEXT_GITHUB;
}

/* ---------------------------------------------------------------- the download */

const char *insider_loggable_url(const char *base, const char *url, const char *file, char *out, size_t size)
{
    if (!size)
        return out;
    if (!insider_owns_url(base, url))
    {
        snprintf(out, size, "%s", url);
        return out;
    }
    /* the host: what is between "://" and the next "/" */
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    const size_t host_length = strcspn(host, "/");
    snprintf(out, size, "%.*s, %s", (int)host_length, host, file && file[0] ? file : "(no file name)");
    return out;
}

bool insider_size_matches(const char *base, const char *url, const char *path, uint64_t size)
{
    if (!insider_owns_url(base, url))
        return true;
    struct stat st;
    return size > 0 && stat(path, &st) == 0 && st.st_size >= 0 && (uint64_t)st.st_size == size;
}

/* ---------------------------------------------------------------- the hint and the log */

bool insider_hint_wanted(bool enabled, bool test_builds, bool has_code, InsiderStatus last)
{
    if (!enabled)
        return false;
    if (test_builds && !has_code)
        return true;
    return last == INSIDER_NOT_QUALIFIED || last == INSIDER_INVALID_CODE || last == INSIDER_EXPIRED_CODE;
}

const char *insider_status_text(InsiderStatus status)
{
    switch (status)
    {
    case INSIDER_OFF: return "off";
    case INSIDER_UPDATE: return "update";
    case INSIDER_NONE: return "none";
    case INSIDER_NOT_QUALIFIED: return "not_qualified";
    case INSIDER_INVALID_CODE: return "invalid_code";
    case INSIDER_EXPIRED_CODE: return "expired_code";
    case INSIDER_OTHER: return "other";
    case INSIDER_HTTP_ERROR: return "http_error";
    case INSIDER_OFFLINE: return "offline";
    case INSIDER_MALFORMED: return "malformed";
    case INSIDER_BAD_BUILD: return "bad_build";
    }
    return "?";
}
