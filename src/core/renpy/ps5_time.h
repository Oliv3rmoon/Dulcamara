/*
 * Dulcamara - the Ren'Py cores' times on the PS5 (ps5_libc.c's gmtime_r and localtime_r): a struct tm by arithmetic
 * alone, with the offset and the zone name Python reads from it. Python is built for the SDK's struct tm, which has
 * both (HAVE_STRUCT_TM_TM_ZONE), and decodes tm_zone without checking it: time.gmtime() and time.localtime() call
 * strlen on it. Plain C, so tests/ps5_libc_check.c checks it against the build machine's libc.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_PS5_TIME_H
#define DULCAMARA_PS5_TIME_H

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* the days from 1970-01-01 to a civil date (Howard Hinnant's days_from_civil) */
static long long days_of(long long year, int month, int day)
{
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400, yoe = year - era * 400;
    const long long doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* zone names for whole quarter hours from UTC-14 to UTC+14, as the time zone database names a zone without an
 * abbreviation ("+0530", "-08"); UTC's is "UTC" */
#define ZONE_QUARTERS 56
static char zone_names[2 * ZONE_QUARTERS + 1][8];
static pthread_once_t zone_names_once = PTHREAD_ONCE_INIT;

static void name_zones(void)
{
    for (int i = 0; i <= 2 * ZONE_QUARTERS; ++i)
    {
        const int minutes = (i - ZONE_QUARTERS) * 15, east = minutes < 0 ? -minutes : minutes;
        const char sign = minutes < 0 ? '-' : '+';
        if (!minutes)
            snprintf(zone_names[i], sizeof(zone_names[i]), "UTC");
        else if (east % 60)
            snprintf(zone_names[i], sizeof(zone_names[i]), "%c%02d%02d", sign, east / 60, east % 60);
        else
            snprintf(zone_names[i], sizeof(zone_names[i]), "%c%02d", sign, east / 60);
    }
}

/* the name of a zone offset seconds east of UTC; "" for one that is not a whole quarter hour within 14 hours */
static const char *zone_of(long offset)
{
    pthread_once(&zone_names_once, name_zones);
    if (offset % 900 || offset < -ZONE_QUARTERS * 900L || offset > ZONE_QUARTERS * 900L)
        return "";
    return zone_names[offset / 900 + ZONE_QUARTERS];
}

/* UTC: the civil date of the days since 1970 (Howard Hinnant's civil_from_days), the zone "UTC" */
static struct tm *utc_time_of(time_t t, struct tm *out)
{
    long long days = (long long)t / 86400, seconds = (long long)t % 86400;
    if (seconds < 0)
        seconds += 86400, --days;
    memset(out, 0, sizeof(*out));
    out->tm_hour = (int)(seconds / 3600);
    out->tm_min = (int)(seconds / 60 % 60);
    out->tm_sec = (int)(seconds % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */
    const long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const int day = (int)(doy - (153 * mp + 2) / 5 + 1), month = (int)(mp < 10 ? mp + 3 : mp - 9);
    const long long year = yoe + era * 400 + (month <= 2);
    static const int BEFORE[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    out->tm_year = (int)(year - 1900);
    out->tm_mon = month - 1;
    out->tm_mday = day;
    out->tm_yday = BEFORE[month - 1] + day - 1 + (leap && month > 2);
    out->tm_zone = (char *)zone_of(0);
    return out;
}

/* local time: the console's fields alone (the C standard's nine, which any libc's struct tm has), the offset worked
 * out from them, and its zone name */
static struct tm *local_time_of(time_t t, const struct tm *console, struct tm *out)
{
    memset(out, 0, sizeof(*out));
    out->tm_sec = console->tm_sec;
    out->tm_min = console->tm_min;
    out->tm_hour = console->tm_hour;
    out->tm_mday = console->tm_mday;
    out->tm_mon = console->tm_mon;
    out->tm_year = console->tm_year;
    out->tm_wday = console->tm_wday;
    out->tm_yday = console->tm_yday;
    out->tm_isdst = console->tm_isdst;
    const long long local = days_of(out->tm_year + 1900LL, out->tm_mon + 1, out->tm_mday) * 86400 +
                            out->tm_hour * 3600LL + out->tm_min * 60LL + out->tm_sec;
    out->tm_gmtoff = (long)(local - (long long)t);
    out->tm_zone = (char *)zone_of(out->tm_gmtoff);
    return out;
}

#endif
