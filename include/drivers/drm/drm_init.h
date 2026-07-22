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

#include <drivers/drm/drm_device.h>

/* Initialize the DRM subsystem and create /dev/dri/card0. */
int drm_init(void);

/* Run the DRM subsystem functional self-test. */
void drm_run_test(void);

/* Return the singleton DRM device, or NULL before init. */
struct drm_device *drm_get_singleton(void);

/* VFS callback wrappers used by devtmpfs to bind /dev/dri/card0. */
void drm_vfs_open_cb(void *parent, const char *name, void *node);
void drm_vfs_close_cb(void *current);

/* DRM VFS operation callbacks (registered with devtmpfs at node creation). */
size_t drm_dev_read(void *file, void *addr, size_t offset, size_t size);
size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size);
int    drm_dev_ioctl(void *file, size_t req, void *arg);
int    drm_dev_poll(void *file, size_t events);
void  *drm_dev_mmap(void *file, size_t offset, size_t size, int flags);

#endif /* INCLUDE_DRM_DRM_INIT_H_ */