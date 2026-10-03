/*
 *
 *      net_sysfs.h
 *      Network device sysfs interface header
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_SYSFS_H_
#define INCLUDE_NET_SYSFS_H_

/* Export every registered network device to /sys/class/net/. */
#if CONFIG_SYSFS && CONFIG_NET
void net_sysfs_init(void);
#else
static inline void net_sysfs_init(void) {}
#endif

#endif // INCLUDE_NET_SYSFS_H_
