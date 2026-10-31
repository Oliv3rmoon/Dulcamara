/*
 * Dulcamara - the Ren'Py cores' gmtime_r, localtime_r (src/core/renpy/ps5_time.h) and mbstowcs (ps5_utf8.h) on the
 * PS5, checked against the build machine's libc.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built and run by tests/test_renpy_core.py: for UTC, every field the build machine's gmtime_r gives, and the zone
 * "UTC"; for local time in zones east and west of UTC, on whole and half and quarter hours, the fields of the build
 * machine's localtime_r as a console's struct tm without an offset or a zone, from which the core must work out the
 * same offset and a zone name; for mbstowcs, UTF-8 text as the build machine's UTF-8 locale decodes it, and text
 * that is not UTF-8 refused. Prints "ok" and how many it checked, or each difference and fails.
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "core/renpy/ps5_time.h"
#include "core/renpy/ps5_utf8.h"

#include <locale.h>
#include <stdlib.h>

static int failures;

static bool same_fields(const struct tm *a, const struct tm *b)
{
    return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min && a->tm_hour == b->tm_hour && a->tm_mday == b->tm_mday &&
           a->tm_mon == b->tm_mon && a->tm_year == b->tm_year && a->tm_wday == b->tm_wday &&
           a->tm_yday == b->tm_yday && a->tm_isdst == b->tm_isdst;
}

static void fail(const char *what, const char *zone, time_t t, const struct tm *got, const struct tm *want)
{
    if (++failures > 20)
        return;
    printf("FAIL %s in %s at %lld: got %04d-%02d-%02d %02d:%02d:%02d wday %d yday %d dst %d offset %ld zone %s%s%s, "
           "want %04d-%02d-%02d %02d:%02d:%02d wday %d yday %d dst %d offset %ld\n",
           what, zone, (long long)t, got->tm_year + 1900, got->tm_mon + 1, got->tm_mday, got->tm_hour, got->tm_min,
           got->tm_sec, got->tm_wday, got->tm_yday, got->tm_isdst, got->tm_gmtoff, got->tm_zone ? "\"" : "",
           got->tm_zone ? got->tm_zone : "NULL", got->tm_zone ? "\"" : "", want->tm_year + 1900, want->tm_mon + 1,
           want->tm_mday, want->tm_hour, want->tm_min, want->tm_sec, want->tm_wday, want->tm_yday, want->tm_isdst,
           want->tm_gmtoff);
}

/* the name the time zone database gives a zone without an abbreviation, worked out here on its own */
static void expected_zone(long offset, char *name, size_t size)
{
    const long east = offset < 0 ? -offset : offset;
    if (!offset)
        snprintf(name, size, "UTC");
    else if (offset % 900 || east > 14 * 3600)
        snprintf(name, size, "%s", "");
    else if (east % 3600)
        snprintf(name, size, "%c%02ld%02ld", offset < 0 ? '-' : '+', east / 3600, east % 3600 / 60);
    else
        snprintf(name, size, "%c%02ld", offset < 0 ? '-' : '+', east / 3600);
}

static void check_utc(time_t t, long *checked)
{
    struct tm got, want;
    if (!gmtime_r(&t, &want))
        return;
    memset(&got, 0xA5, sizeof(got));
    utc_time_of(t, &got);
    ++*checked;
    if (!same_fields(&got, &want) || got.tm_gmtoff != 0 || !got.tm_zone || strcmp(got.tm_zone, "UTC"))
        fail("gmtime_r", "UTC", t, &got, &want);
}

static void check_local(const char *zone, time_t t, long *checked)
{
    struct tm want, console, got;
    if (!localtime_r(&t, &want))
        return;
    /* a console's struct tm: the nine fields, and nothing set past them */
    memset(&console, 0xA5, sizeof(console));
    console.tm_sec = want.tm_sec;
    console.tm_min = want.tm_min;
    console.tm_hour = want.tm_hour;
    console.tm_mday = want.tm_mday;
    console.tm_mon = want.tm_mon;
    console.tm_year = want.tm_year;
    console.tm_wday = want.tm_wday;
    console.tm_yday = want.tm_yday;
    console.tm_isdst = want.tm_isdst;
    memset(&got, 0xA5, sizeof(got));
    local_time_of(t, &console, &got);
    char name[16];
    expected_zone(want.tm_gmtoff, name, sizeof(name));
    ++*checked;
    if (!same_fields(&got, &want) || got.tm_gmtoff != want.tm_gmtoff || !got.tm_zone || strcmp(got.tm_zone, name))
        fail("localtime_r", zone, t, &got, &want);
}

/* text both decode alike, as a count (no out), into room for all of it, and into room for less */
static void check_utf8(const char *text, long *checked)
{
    wchar_t got[32], want[32];
    const size_t count = utf8_mbstowcs(NULL, text, 0), host = mbstowcs(NULL, text, 0);
    ++*checked;
    if (count != host)
    {
        ++failures;
        printf("FAIL mbstowcs counts \"%s\" as %zu characters, the build machine as %zu\n", text, count, host);
        return;
    }
    for (size_t room = 0; room <= count + 1 && room <= 32; ++room)
    {
        wmemset(got, L'#', 32);
        wmemset(want, L'#', 32);
        const size_t written = utf8_mbstowcs(got, text, room), host_written = mbstowcs(want, text, room);
        if (written != host_written || wmemcmp(got, want, 32))
        {
            ++failures;
            printf("FAIL mbstowcs of \"%s\" into room for %zu: %zu written, the build machine %zu\n", text, room,
                   written, host_written);
        }
    }
}

static void check_not_utf8(const char *text, long *checked)
{
    wchar_t out[32];
    ++*checked;
    errno = 0;
    if (utf8_mbstowcs(NULL, text, 0) != (size_t)-1 || errno != EILSEQ ||
        utf8_mbstowcs(out, text, 32) != (size_t)-1)
    {
        ++failures;
        printf("FAIL mbstowcs took text that is not UTF-8:");
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
            printf(" %02x", *p);
        printf("\n");
    }
}

int main(void)
{
    long checked = 0;
    if (setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "en_US.UTF-8"))
    {
        static const char *const TEXTS[] = {"", "UTC", "+0530", "No such file or directory", "caf\xc3\xa9",
                                            "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "\xf0\x9f\x8e\xae game",
                                            "\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"};
        for (size_t i = 0; i < sizeof(TEXTS) / sizeof(TEXTS[0]); ++i)
            check_utf8(TEXTS[i], &checked);
    }
    else
        printf("(no UTF-8 locale here: mbstowcs is checked against its own expectations only)\n");
    setlocale(LC_CTYPE, "C");
    /* a stray continuation byte, a missing one, overlong forms, a surrogate, beyond U+10FFFF, bytes UTF-8 never has */
    static const char *const NOT_UTF8[] = {"\x80", "a\xbf", "\xc3", "\xe6\x97", "\xc3(", "\xc0\x80", "\xc1\xbf",
                                           "\xe0\x80\x80", "\xf0\x80\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                                           "\xf8\x88\x80\x80\x80", "\xfe", "\xff"};
    for (size_t i = 0; i < sizeof(NOT_UTF8) / sizeof(NOT_UTF8[0]); ++i)
        check_not_utf8(NOT_UTF8[i], &checked);
    {
        wchar_t out[8];
        ++checked;
        if (utf8_mbstowcs(out, "\xe6\x97\xa5 \xf0\x9f\x8e\xae", 8) != 3 || out[0] != 0x65E5 || out[1] != L' ' ||
            out[2] != 0x1F3AE || out[3] != L'\0')
        {
            ++failures;
            printf("FAIL mbstowcs of a CJK character, a space and an emoji\n");
        }
    }
    static const long long MOMENTS[] = {
        -2208988800LL, /* 1900-01-01 */
        -86401, -1, 0, 1, 86399, 86400,
        951782400,  /* 2000-02-29 */
        1709164800, /* 2024-02-29 */
        1761868800, /* 2025-10-31 */
        2147483647, 2147483648LL,
        4102444800LL,   /* 2100-01-01, not a leap year */
        253402300799LL, /* 9999-12-31 23:59:59 */
    };
    setenv("TZ", "UTC", 1);
    tzset();
    for (size_t i = 0; i < sizeof(MOMENTS) / sizeof(MOMENTS[0]); ++i)
        check_utc((time_t)MOMENTS[i], &checked);
    for (long long t = -2208988800LL; t < 4102444800LL; t += 86400LL * 13 + 3607)
        check_utc((time_t)t, &checked);
    /* UTC, west and east on the hour, on half and three-quarter hours, and with summer time either side */
    static const char *const ZONES[] = {"UTC", "America/Los_Angeles", "America/St_Johns", "Europe/Berlin",
                                        "Asia/Kolkata", "Asia/Kathmandu", "Australia/Adelaide", "Pacific/Chatham",
                                        "Pacific/Kiritimati", "Pacific/Pago_Pago"};
    for (size_t z = 0; z < sizeof(ZONES) / sizeof(ZONES[0]); ++z)
    {
        setenv("TZ", ZONES[z], 1);
        tzset();
        for (long long t = 946684800LL; t < 1924992000LL; t += 86400LL * 3 + 1801) /* 2000 to 2031 */
            check_local(ZONES[z], (time_t)t, &checked);
    }
    if (failures)
    {
        printf("%d of %ld checks differ\n", failures, checked);
        return 1;
    }
    printf("ok %ld\n", checked);
    return 0;
}
