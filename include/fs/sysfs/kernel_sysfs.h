/*
 *
 *      kernel_sysfs.h
 *      /sys/kernel/ attribute files integration header
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KERNEL_SYSFS_H_
#define INCLUDE_KERNEL_SYSFS_H_

/* Register /sys/kernel/{version,cmdline,hostname,...} attribute files. */
void kernel_sysfs_init(void);

#endif // INCLUDE_KERNEL_SYSFS_H_
