/*
 *
 *      tty_sysfs.h
 *      TTY class sysfs integration header
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TTY_SYSFS_H_
#define INCLUDE_TTY_SYSFS_H_

/* Register /sys/class/tty/ class and per-device attributes. */
#if CONFIG_SYSFS
void tty_sysfs_init(void);
#else
static inline void tty_sysfs_init(void) {}
#endif

#endif // INCLUDE_TTY_SYSFS_H_
