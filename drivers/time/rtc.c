/*
 *
 *      rtc.c
 *      Real-time clock (CMOS) character device
 *
 *      2026/8/6 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cmos.h>
#include <drivers/time/rtc.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/termios.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <process/uaccess.h>

#define RTC_RD_TIME  _IOR('p', 0x09, rtc_time_t)
#define RTC_SET_TIME _IOW('p', 0x0a, rtc_time_t)

/* Read the current time from the CMOS clock into a rtc_time_t. */
static void rtc_read_cmos_time(rtc_time_t *t)
{
    t->tm_sec   = (int)BCD_HEX(read_cmos(CMOS_CUR_SEC));
    t->tm_min   = (int)BCD_HEX(read_cmos(CMOS_CUR_MIN));
    t->tm_hour  = (int)BCD_HEX(read_cmos(CMOS_CUR_HOUR));
    t->tm_mday  = (int)BCD_HEX(read_cmos(CMOS_MON_DAY));
    t->tm_mon   = (int)BCD_HEX(read_cmos(CMOS_CUR_MON)) - 1;
    t->tm_year  = (int)BCD_HEX(read_cmos(CMOS_CUR_YEAR)) + (int)BCD_HEX(read_cmos(CMOS_CUR_CEN)) * 100 - 1900;
    t->tm_wday  = (int)BCD_HEX(read_cmos(CMOS_WEEK_DAY));
    t->tm_yday  = 0;
    t->tm_isdst = 0;
}

/* Program the CMOS clock from a validated rtc_time_t. */
static void rtc_write_cmos_time(const rtc_time_t *t)
{
    write_cmos(CMOS_CUR_SEC, HEX_BCD((uint8_t)t->tm_sec));
    write_cmos(CMOS_CUR_MIN, HEX_BCD((uint8_t)t->tm_min));
    write_cmos(CMOS_CUR_HOUR, HEX_BCD((uint8_t)t->tm_hour));
    write_cmos(CMOS_MON_DAY, HEX_BCD((uint8_t)t->tm_mday));
    write_cmos(CMOS_CUR_MON, HEX_BCD((uint8_t)(t->tm_mon + 1)));
    write_cmos(CMOS_CUR_YEAR, HEX_BCD((uint8_t)(t->tm_year % 100)));
    write_cmos(CMOS_CUR_CEN, HEX_BCD((uint8_t)((t->tm_year / 100) + 19)));
    write_cmos(CMOS_WEEK_DAY, HEX_BCD((uint8_t)t->tm_wday));
}

/* days since epoch for a civil date (Howard Hinnant days_from_civil). */
static uint64_t rtc_civil_to_epoch(const rtc_time_t *t)
{
    int      y = t->tm_year + 1900;
    int      m = t->tm_mon + 1;
    int      d = t->tm_mday;
    int64_t  era, yoe, doy, doe;
    uint64_t days;

    if (m <= 2) y -= 1;
    era  = (y >= 0 ? y : y - 399) / 400;
    yoe  = y - era * 400;
    doy  = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = (uint64_t)(era * 146097 + doe - 719468);
    return days * 86400ULL + (uint64_t)t->tm_hour * 3600ULL + (uint64_t)t->tm_min * 60ULL + (uint64_t)t->tm_sec;
}

/* Fill the caller's rtc_time_t with the current CMOS time. */
void rtc_get_time(rtc_time_t *t)
{
    if (t) rtc_read_cmos_time(t);
}

/* Return the current CMOS time as seconds since the Unix epoch. */
uint64_t rtc_since_epoch(void)
{
    rtc_time_t t;
    rtc_read_cmos_time(&t);
    return rtc_civil_to_epoch(&t);
}

/* Read the current CMOS time as a byte stream. */
int64_t rtc_dev_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    rtc_time_t t;
    uint8_t    data[8];

    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)offset;
    if (!addr) return -EINVAL;

    rtc_read_cmos_time(&t);
    data[0] = (uint8_t)(t.tm_year); // year since 1900
    data[1] = (uint8_t)(t.tm_mon + 1);
    data[2] = (uint8_t)t.tm_mday;
    data[3] = (uint8_t)t.tm_hour;
    data[4] = (uint8_t)t.tm_min;
    data[5] = (uint8_t)t.tm_sec;
    data[6] = (uint8_t)t.tm_wday;
    data[7] = (uint8_t)((t.tm_year / 100) + 19); // century

    if (size > sizeof(data)) size = sizeof(data);
    memcpy(addr, data, size);
    return (int64_t)size;
}

/* Reject byte-stream writes (the RTC device is read-only). */
int64_t rtc_dev_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    plogk("rtc: Rejected write to /dev/rtc0 (RTC is read-only as a byte stream)\n");
    return -EIO; // RTC is not writable as a byte stream
}

/* Handle RTC_RD_TIME and RTC_SET_TIME ioctls. */
int rtc_dev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *argument)
{
    rtc_time_t t;
    int        valid;

    (void)ctx;
    (void)private_data;
    (void)flags;

    switch (request) {
        case RTC_RD_TIME :
            rtc_read_cmos_time(&t);
            return copy_to_user(argument, &t, sizeof(t)) ? -EFAULT : 0;
        case RTC_SET_TIME :
            if (copy_from_user(&t, argument, sizeof(t))) return -EFAULT;
            valid = t.tm_sec >= 0 && t.tm_sec <= 59 && t.tm_min >= 0 && t.tm_min <= 59 && t.tm_hour >= 0 && t.tm_hour <= 23 && t.tm_mday >= 1 && t.tm_mday <= 31 && t.tm_mon >= 0 && t.tm_mon <= 11
                    && t.tm_year >= 70;
            if (!valid) {
                plogk("rtc: Rejected RTC_SET_TIME with invalid time (y=%d m=%d d=%d h=%d min=%d s=%d)\n", t.tm_year, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
                return -EINVAL;
            }
            rtc_write_cmos_time(&t);
            return 0;
        default :
            return -ENOTTY;
    }
}
