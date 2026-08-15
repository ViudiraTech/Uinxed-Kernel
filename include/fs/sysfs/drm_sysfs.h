/*
 *
 *      drm_sysfs.h
 *      DRM class sysfs integration header
 *
 *      2026/8/15 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRM_SYSFS_H_
#define INCLUDE_DRM_SYSFS_H_

struct drm_device;
struct drm_connector;

/* Register /sys/class/drm/ with the device model. */
void drm_sysfs_init(void);

/* Publish a DRM device under /sys/class/drm/ (card%d). */
void drm_sysfs_register_device(struct drm_device *dev);

/* Publish a connector under /sys/class/drm/ (cardN-<type>-<id>). */
void drm_sysfs_connector_add(struct drm_connector *connector);

#endif // INCLUDE_DRM_SYSFS_H_
