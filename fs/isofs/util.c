/*
 *
 *      util.c
 *      ISO 9660 date/time conversion
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/isofs/isofs.h>
#include <libs/std/stdint.h>

static int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint64_t mktime64(int year, int month, int day, int hour, int min, int sec)
{
    uint64_t total = 0;
    int      y;

    for (y = 1970; y < year; y++) total += (uint64_t)(is_leap_year(y) ? 366 : 365) * 86400;

    for (int m = 1; m < month; m++) {
        int dim = days_in_month[m - 1];
        if (m == 2 && is_leap_year(year)) dim = 29;
        total += (uint64_t)dim * 86400;
    }

    total += (uint64_t)(day - 1) * 86400;
    total += (uint64_t)hour * 3600;
    total += (uint64_t)min * 60;
    total += (uint64_t)sec;

    return total;
}

uint64_t iso_date_to_unix(const uint8_t *p, int flags)
{
    int      year, month, day, hour, minute, second, tz;
    uint64_t ts;

    if (flags & ISO_DATE_LONG_FORM) {
        year   = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0') - 1900;
        month  = (p[4] - '0') * 10 + (p[5] - '0');
        day    = (p[6] - '0') * 10 + (p[7] - '0');
        hour   = (p[8] - '0') * 10 + (p[9] - '0');
        minute = (p[10] - '0') * 10 + (p[11] - '0');
        second = (p[12] - '0') * 10 + (p[13] - '0');
        tz     = (int8_t)p[16];
    } else {
        year   = p[0];
        month  = p[1];
        day    = p[2];
        hour   = p[3];
        minute = p[4];
        second = p[5];
        tz     = (flags & ISO_DATE_HIGH_SIERRA) ? 0 : (int8_t)p[6];
    }

    if (year < 0) return 0;

    ts = mktime64(year + 1900, month, day, hour, minute, second);

    if (tz >= -52 && tz <= 52) ts -= (int64_t)tz * 15 * 60;

    return ts;
}
