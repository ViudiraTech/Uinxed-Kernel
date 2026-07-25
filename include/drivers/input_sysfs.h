/*
 *
 *      input_sysfs.h
 *      sysfs class support for evdev input devices
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_INPUT_SYSFS_H_
#define INCLUDE_DRIVERS_INPUT_SYSFS_H_

#include <drivers/evdev.h>

void input_sysfs_init(void);
int  input_sysfs_register_evdev(evdev_t *evdev);
void input_sysfs_unregister_evdev(evdev_t *evdev);

#endif /* INCLUDE_DRIVERS_INPUT_SYSFS_H_ */
