/*
 *
 *      fb_sysfs.h
 *      Linux-compatible framebuffer class topology and attributes.
 *
 *      2026/8/2 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FB_SYSFS_H_
#define INCLUDE_FB_SYSFS_H_

/* Register the framebuffer class device on the platform bus. */
void fb_sysfs_init(void);

#endif // INCLUDE_FB_SYSFS_H_
