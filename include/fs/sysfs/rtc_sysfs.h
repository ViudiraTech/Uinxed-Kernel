/*
 *
 *      rtc_sysfs.h
 *      RTC sysfs class integration header
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RTC_SYSFS_H_
#define INCLUDE_RTC_SYSFS_H_

/* Register /sys/class/rtc/rtc0 with the standard date/time attributes. */
void rtc_sysfs_init(void);

#endif // INCLUDE_RTC_SYSFS_H_
