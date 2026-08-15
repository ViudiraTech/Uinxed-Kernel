/*
 *
 *      usb_sysfs.h
 *      USB bus and device sysfs integration header
 *
 *      2026/8/15 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_USB_SYSFS_H_
#define INCLUDE_USB_SYSFS_H_

struct attribute_group;

/* USB device attribute groups, attached to every usb device at registration. */
extern const struct attribute_group *usb_device_groups[];

/* Register /sys/bus/usb with the device model. */
void usb_sysfs_init(void);

#endif // INCLUDE_USB_SYSFS_H_
