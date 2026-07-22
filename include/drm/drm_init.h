/*
 *
 *      drm_init.h
 *      DRM subsystem initialization entry point
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRM_DRM_INIT_H_
#define INCLUDE_DRM_DRM_INIT_H_

#include <drm/drm_device.h>

/* Initialize the DRM subsystem and create /dev/dri/card0. */
int drm_init(void);

/* Run the DRM subsystem functional self-test. */
void drm_run_test(void);

/* Return the singleton DRM device, or NULL before init. */
struct drm_device *drm_get_singleton(void);

/* VFS callback wrappers used by devtmpfs to bind /dev/dri/card0. */
void drm_vfs_open_cb(void *parent, const char *name, void *node);
void drm_vfs_close_cb(void *current);

#endif /* INCLUDE_DRM_DRM_INIT_H_ */