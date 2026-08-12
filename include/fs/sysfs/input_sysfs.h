/*
 *
 *      input_sysfs.h
 *      sysfs class support for evdev input devices
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_INPUT_SYSFS_H_
#define INCLUDE_INPUT_SYSFS_H_

#include <drivers/input/evdev/evdev.h>

/* Register the input class and publish every evdev device. */
void input_sysfs_init(void);

/* Publish an evdev device as inputN with an eventN child. */
int input_sysfs_register_evdev(evdev_t *evdev);

/* Remove an evdev device's sysfs devices. */
void input_sysfs_unregister_evdev(evdev_t *evdev);

#endif // INCLUDE_INPUT_SYSFS_H_
