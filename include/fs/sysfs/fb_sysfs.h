/*
 *
 *      fb_sysfs.h
 *      Framebuffer class topology and attributes.
 *
 *      2026/8/2 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FB_SYSFS_H_
#define INCLUDE_FB_SYSFS_H_

/* Register the framebuffer class device on the platform bus. */
#if CONFIG_SYSFS
void fb_sysfs_init(void);
#else
static inline void fb_sysfs_init(void) {}
#endif

#endif // INCLUDE_FB_SYSFS_H_
