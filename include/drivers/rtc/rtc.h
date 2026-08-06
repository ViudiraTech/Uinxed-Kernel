/*
 *
 *      rtc.h
 *      Real-time clock (CMOS) device header
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RTC_H_
#define INCLUDE_RTC_H_

#include <libs/std/stdint.h>

/* Linux rtc_time ABI (x86-64). */
typedef struct {
        int tm_sec;
        int tm_min;
        int tm_hour;
        int tm_mday;
        int tm_mon;  /* 0-11 */
        int tm_year; /* years since 1900 */
        int tm_wday; /* 0-6, Sunday=0 */
        int tm_yday; /* 0-365 */
        int tm_isdst;
} rtc_time_t;

/* Read the current time from the CMOS RTC. */
void rtc_get_time(rtc_time_t *t);

/* Seconds since the Unix epoch for the current CMOS time. */
uint64_t rtc_since_epoch(void);

/* Register /dev/rtc0 (character device).  The sysfs side (/sys/class/rtc)
 * lives in fs/sysfs/rtc_sysfs.c.  Must be called after devtmpfs is ready. */
void rtc_vfs_init(void);

#endif // INCLUDE_RTC_H_
