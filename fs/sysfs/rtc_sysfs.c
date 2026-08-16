/*
 *
 *      rtc_sysfs.c
 *      RTC sysfs class integration (/sys/class/rtc)
 *
 *      2026/8/6 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/time/rtc.h>
#include <fs/sysfs/rtc_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

/* Show the current RTC date as YYYY-MM-DD. */
static ssize_t rtc_date_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    rtc_time_t t;
    (void)dev;
    (void)attr;
    rtc_get_time(&t);
    return sysfs_emit(buf, "%04d-%02d-%02d\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

/* Show the current RTC time as HH:MM:SS. */
static ssize_t rtc_time_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    rtc_time_t t;
    (void)dev;
    (void)attr;
    rtc_get_time(&t);
    return sysfs_emit(buf, "%02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
}

/* Show the RTC time as seconds since the Unix epoch. */
static ssize_t rtc_since_epoch_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "%llu\n", (unsigned long long)rtc_since_epoch());
}

/* Show the fixed rtc0 device name. */
static ssize_t rtc_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "rtc0\n");
}

static DEVICE_ATTR(date, 0444, rtc_date_show, NULL);
static DEVICE_ATTR(time, 0444, rtc_time_show, NULL);
static DEVICE_ATTR(since_epoch, 0444, rtc_since_epoch_show, NULL);
static DEVICE_ATTR(name, 0444, rtc_name_show, NULL);

static struct attribute *rtc_attributes[] = {
    &dev_attr_date.attr, &dev_attr_time.attr, &dev_attr_since_epoch.attr, &dev_attr_name.attr, NULL,
};

static struct attribute_group rtc_group = {
    .attrs = rtc_attributes,
};

static const struct attribute_group *rtc_groups[] = {
    &rtc_group,
    NULL,
};

static struct class rtc_class = {.name = "rtc", .dev_groups = rtc_groups};

/* Register the RTC class and its rtc0 device. */
void rtc_sysfs_init(void)
{
#if CONFIG_SYSFS
    if (class_register(&rtc_class) != EOK) {
        plogk("rtc_sysfs: Class_register(rtc) failed.\n");
        return;
    }
    (void)device_create(&rtc_class, NULL, MKDEV(RTC_DEV_MAJOR, RTC0_MINOR), NULL, "rtc0");
    plogk("rtc_sysfs: registered /sys/class/rtc/rtc0\n");
#endif
}
