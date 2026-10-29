/*
 * Nightshade Player - reading RPG Maker 2000 / 2003 games' files for the library (rpgmaker.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RPG_RT.ldb is liblcf's "LcfDataBase": a length-prefixed header, then chunks, each a compressed integer id, a
 * compressed integer length and the data; a struct's fields are chunks the same way, ended by id 0. Compressed
 * integers are big-endian groups of 7 bits, the high bit set on all but the last (liblcf's LcfReader::ReadInt).
 */
#include "rpgmaker.h"

#include "../third_party/miniz/miniz.h"

#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool rpgm_name_is(const char *name, const char *want)
{
    for (; *name && *want; ++name, ++want)
    {
        char a = *name, b = *want;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return !*name && !*want;
}

/* ---------------------------------------------------------------- encodings */

static bool valid_utf8(const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n;)
    {
        const unsigned char c = b[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (len == 0 || (len == 2 && c < 0xC2) || i + len > n)
            return false;
        for (size_t k = 1; k < len; ++k)
            if ((b[i + k] & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}

/* The same guess as liblcf's without ICU (tools/patches/liblcf-iconv.patch), for the few words of a title. */
int rpgm_guess_codepage(const char *text, size_t n)
{
    const unsigned char *b = (const unsigned char *)text;
    size_t high = 0, letters_c0 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        high += b[i] >= 0x80;
        letters_c0 += b[i] >= 0xC0;
    }
    if (high == 0 || valid_utf8(b, n))
        return 65001;
    /* Shift-JIS: every high byte in a valid pair (lead 81-9F / E0-FC, trail 40-7E / 80-FC) or a half-width katakana
     * (A1-DF); Japanese is mostly kana and punctuation, whose leads are below A0 */
    size_t pairs = 0, low_leads = 0, kana = 0, bad = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = b[i];
        if (c < 0x80)
            continue;
        if (c >= 0xA1 && c <= 0xDF)
            ++kana;
        else if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < n &&
                 ((b[i + 1] >= 0x40 && b[i + 1] <= 0x7E) || (b[i + 1] >= 0x80 && b[i + 1] <= 0xFC)))
        {
            ++pairs;
            low_leads += c <= 0x9F;
            ++i;
        }
        else
            ++bad;
    }
    if (bad == 0 && pairs > 0 && kana * 4 <= pairs && low_leads * 2 >= pairs)
        return 932;
    /* the EUC-style double-byte encodings: Korean when most pairs are Hangul syllables (leads B0-C8) */
    size_t euc = 0, hangul = 0, euc_bad = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = b[i];
        if (c < 0x80)
            continue;
        if (c >= 0x81 && c <= 0xFE && i + 1 < n && b[i + 1] >= 0x41 && b[i + 1] <= 0xFE)
        {
            ++euc;
            hangul += c >= 0xB0 && c <= 0xC8 && b[i + 1] >= 0xA1;
            ++i;
        }
        else
            ++euc_bad;
    }
    if (euc_bad == 0 && euc > 0 && hangul * 10 >= euc * 7)
        return 949;
    /* Cyrillic: most of the text is letters C0-FF (a Western title has a few accents among ASCII) */
    if (letters_c0 * 10 >= high * 7 && high * 2 >= n && high >= 3)
        return 1251;
    if (euc_bad == 0 && euc > 0 && high * 2 >= n)
        return 936;
    return 1252;
}

/* windows-1252's 0x80-0x9F, for when iconv is missing it */
static const uint16_t CP1252_HIGH[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                         0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                         0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                         0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

static size_t put_utf8(char *out, size_t size, size_t at, unsigned cp)
{
    char buf[4];
    size_t n = cp < 0x80 ? (buf[0] = (char)cp, 1)
               : cp < 0x800 ? (buf[0] = (char)(0xC0 | cp >> 6), buf[1] = (char)(0x80 | (cp & 0x3F)), 2)
                            : (buf[0] = (char)(0xE0 | cp >> 12), buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)),
                               buf[2] = (char)(0x80 | (cp & 0x3F)), 3);
    if (at + n >= size)
        return at;
    memcpy(out + at, buf, n);
    return at + n;
}

void rpgm_to_utf8(const char *text, size_t len, int codepage, char *out, size_t size)
{
    if (size == 0)
        return;
    out[0] = '\0';
    if (codepage == 0)
        codepage = rpgm_guess_codepage(text, len);
    if (codepage == 65001)
    {
        size_t n = len < size - 1 ? len : size - 1;
        while (n > 0 && n < len && ((unsigned char)text[n] & 0xC0) == 0x80)
            --n; /* never cut a character in two */
        memcpy(out, text, n);
        out[n] = '\0';
        return;
    }
    char name[16];
    snprintf(name, sizeof(name), "CP%d", codepage);
    iconv_t cd = iconv_open("UTF-8", name);
    if (cd == (iconv_t)-1 && codepage == 932)
        cd = iconv_open("UTF-8", "SHIFT_JIS");
    if (cd != (iconv_t)-1)
    {
        char *in = (char *)text, *to = out;
        size_t in_left = len, out_left = size - 1;
        while (in_left > 0 && out_left > 0)
        {
            if (iconv(cd, &in, &in_left, &to, &out_left) != (size_t)-1)
                break;
            if (errno == E2BIG)
                break;
            /* a byte with no character: '?' in its place */
            *to++ = '?';
            --out_left;
            ++in;
            --in_left;
        }
        iconv_close(cd);
        *to = '\0';
        return;
    }
    /* no iconv for it: windows-1252 by hand (Latin-1 with its 0x80-0x9F) */
    size_t at = 0;
    for (size_t i = 0; i < len; ++i)
    {
        const unsigned char c = (unsigned char)text[i];
        at = put_utf8(out, size, at, c >= 0x80 && c < 0xA0 ? CP1252_HIGH[c - 0x80] : c);
    }
    out[at] = '\0';
}

/* ---------------------------------------------------------------- RPG_RT.ini */

/* The value of key in [section] (case-insensitive names), trimmed; false when it isn't there. */
static bool ini_value(const char *ini, size_t len, const char *section, const char *key, const char **value,
                      size_t *value_len)
{
    bool in_section = false;
    for (size_t i = 0; i < len;)
    {
        size_t end = i;
        while (end < len && ini[end] != '\n' && ini[end] != '\r')
            ++end;
        size_t a = i, b = end;
        while (a < b && (ini[a] == ' ' || ini[a] == '\t'))
            ++a;
        while (b > a && (ini[b - 1] == ' ' || ini[b - 1] == '\t'))
            --b;
        /* a UTF-8 byte order mark before the first section */
        if (a == 0 && b - a >= 3 && !memcmp(ini, "\xEF\xBB\xBF", 3))
            a += 3;
        if (b > a && ini[a] == '[')
        {
            const char *close = memchr(ini + a, ']', b - a);
            char name[64];
            size_t n = close ? (size_t)(close - (ini + a + 1)) : 0;
            if (n >= sizeof(name))
                n = sizeof(name) - 1;
            memcpy(name, ini + a + 1, n);
            name[n] = '\0';
            in_section = close && rpgm_name_is(name, section);
        }
        else if (in_section && b > a && ini[a] != ';' && ini[a] != '#')
        {
            const char *eq = memchr(ini + a, '=', b - a);
            if (eq)
            {
                size_t kb = (size_t)(eq - ini);
                while (kb > a && (ini[kb - 1] == ' ' || ini[kb - 1] == '\t'))
                    --kb;
                char name[64];
                size_t n = kb - a < sizeof(name) - 1 ? kb - a : sizeof(name) - 1;
                memcpy(name, ini + a, n);
                name[n] = '\0';
                if (rpgm_name_is(name, key))
                {
                    size_t v = (size_t)(eq - ini) + 1;
                    while (v < b && (ini[v] == ' ' || ini[v] == '\t'))
                        ++v;
                    *value = ini + v;
                    *value_len = b - v;
                    return true;
                }
            }
        }
        i = end;
        while (i < len && (ini[i] == '\n' || ini[i] == '\r'))
            ++i;
    }
    return false;
}

bool rpgm_ini_title(const char *ini, size_t len, char *out, size_t size)
{
    const char *value;
    size_t n;
    if (size)
        out[0] = '\0';
    if (!ini || !ini_value(ini, len, "RPG_RT", "GameTitle", &value, &n) || n == 0)
        return false;
    int codepage = 0;
    const char *enc;
    size_t enc_len;
    if (ini_value(ini, len, "EasyRPG", "Encoding", &enc, &enc_len) && enc_len > 0 && enc_len < 8)
    {
        char number[8];
        memcpy(number, enc, enc_len);
        number[enc_len] = '\0';
        codepage = atoi(number);
    }
    rpgm_to_utf8(value, n, codepage, out, size);
    return out[0] != '\0';
}

/* ---------------------------------------------------------------- RPG_RT.ldb */

static bool read_int(const uint8_t *p, size_t len, size_t *pos, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 5; ++i)
    {
        if (*pos >= len)
            return false;
        const uint8_t b = p[(*pos)++];
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80))
        {
            *out = v;
            return true;
        }
    }
    return false;
}

bool rpgm_ldb_info(const uint8_t *ldb, size_t len, char *title_name, size_t size, int *engine)
{
    if (size)
        title_name[0] = '\0';
    if (engine)
        *engine = 0;
    size_t pos = 0;
    uint32_t header;
    if (!read_int(ldb, len, &pos, &header) || header != 11 || pos + header > len ||
        memcmp(ldb + pos, "LcfDataBase", 11) != 0)
        return false;
    pos += header;
    bool rpg2003 = false, found_system = false;
    while (pos < len)
    {
        uint32_t id, length;
        if (!read_int(ldb, len, &pos, &id) || id == 0 || !read_int(ldb, len, &pos, &length) || length > len - pos)
            break;
        if (id == 0x1D || id == 0x1E) /* battle commands, classes: RPG Maker 2003's */
            rpg2003 = true;
        if (id == 0x16) /* System */
        {
            found_system = true;
            size_t p = pos, end = pos + length;
            while (p < end)
            {
                uint32_t field, flen;
                if (!read_int(ldb, end, &p, &field) || field == 0 || !read_int(ldb, end, &p, &flen) ||
                    flen > end - p)
                    break;
                if (field == 0x11 && size) /* title_name */
                {
                    size_t n = flen < size - 1 ? flen : size - 1;
                    memcpy(title_name, ldb + p, n);
                    title_name[n] = '\0';
                }
                p += flen;
            }
        }
        pos += length;
    }
    if (engine)
        *engine = rpg2003 ? 2003 : 2000;
    return found_system;
}

/* ---------------------------------------------------------------- XYZ pictures */

uint8_t *rpgm_decode_xyz(const uint8_t *data, size_t len, int *width, int *height)
{
    if (len < 8 || memcmp(data, "XYZ1", 4) != 0)
        return NULL;
    const int w = data[4] | data[5] << 8, h = data[6] | data[7] << 8;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return NULL;
    const size_t want = 768 + (size_t)w * h;
    size_t got = 0;
    uint8_t *raw = tinfl_decompress_mem_to_heap(data + 8, len - 8, &got, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!raw)
        return NULL;
    uint8_t *rgba = got >= want ? malloc((size_t)w * h * 4) : NULL;
    if (rgba)
    {
        const uint8_t *palette = raw, *pixels = raw + 768;
        for (size_t i = 0; i < (size_t)w * h; ++i)
        {
            const uint8_t *c = palette + pixels[i] * 3;
            rgba[i * 4] = c[0];
            rgba[i * 4 + 1] = c[1];
            rgba[i * 4 + 2] = c[2];
            rgba[i * 4 + 3] = 255;
        }
        *width = w;
        *height = h;
    }
    mz_free(raw);
    return rgba;
}
