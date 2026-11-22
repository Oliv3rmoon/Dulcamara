/*
 * Dulcamara - the Ren'Py cores' mbstowcs on the PS5 (ps5_libc.c): UTF-8, as a UTF-8 locale's. Plain C, so
 * tests/ps5_libc_check.c checks it on the build machine.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_PS5_UTF8_H
#define DULCAMARA_PS5_UTF8_H

#include <errno.h>
#include <stddef.h>
#include <wchar.h>

/* the next character of UTF-8 text and its length, or a length of 0 where the text is not UTF-8 (a stray or missing
 * continuation byte, an overlong form, a surrogate, or beyond U+10FFFF) */
static size_t utf8_next(const unsigned char *p, unsigned long *c)
{
    static const unsigned long LEAST[5] = {0, 0, 0x80, 0x800, 0x10000};
    size_t length;
    if (p[0] < 0x80)
        length = 1;
    else if (p[0] >= 0xC2 && p[0] < 0xE0)
        length = 2;
    else if (p[0] >= 0xE0 && p[0] < 0xF0)
        length = 3;
    else if (p[0] >= 0xF0 && p[0] < 0xF5)
        length = 4;
    else
        return 0;
    unsigned long value = length == 1 ? p[0] : p[0] & (0x7F >> length);
    for (size_t i = 1; i < length; ++i)
    {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        value = value << 6 | (p[i] & 0x3F);
    }
    if (value < LEAST[length] || (value >= 0xD800 && value < 0xE000) || value > 0x10FFFF)
        return 0;
    *c = value;
    return length;
}

/* mbstowcs for UTF-8 text: with no out, how many characters it has; otherwise up to n of them into out, with its 0
 * when there is room; how many were written without the 0, or (size_t)-1 and EILSEQ where it is not UTF-8 */
static size_t utf8_mbstowcs(wchar_t *out, const char *text, size_t n)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t count = 0;
    while (!out || count < n)
    {
        unsigned long c = 0;
        const size_t length = utf8_next(p, &c);
        if (!length)
        {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (!c)
        {
            if (out)
                out[count] = L'\0';
            return count;
        }
        if (out)
            out[count] = (wchar_t)c;
        ++count;
        p += length;
    }
    return count;
}

#endif
