/*
 *
 *      drm_init.h
 *      DRM subsystem initialization entry point
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRM_INIT_H_
#define INCLUDE_DRM_INIT_H_

#include <drivers/gpu/drm/drm_device.h>
#include <libs/std/stddef.h>

struct vm_area;

/* Initialize the DRM subsystem and create /dev/dri/card0. */
int drm_init(void);
int drm_init_fallback(void);

/* Run the DRM subsystem functional self-test. */
void drm_run_test(void);

/* Return the singleton DRM device, or NULL before init. */
struct drm_device *drm_get_singleton(void);
struct drm_device *drm_get_device_by_minor(int type, int index);
void               drm_device_list_add(struct drm_device *dev);
void               drm_device_list_remove(struct drm_device *dev);

/* DRM device-class registration state (defined in drm_init.c). */
struct class;
extern struct class drm_class;
extern int drm_class_registered;

/* VFS callback wrappers used by devtmpfs to bind /dev/dri/card0. */
void drm_vfs_open_cb(void *parent, const char *name, void *node);
void drm_vfs_close_cb(void *current);

/*
 * Per-open callbacks used by tmpfs/devtmpfs. DRM state is attached to each
 * file descriptor, never to the shared directory node.
 */
int     drm_dev_open(void *node, uint64_t flags, void **private_data);
void    drm_dev_release(void *node, void *private_data);
int     drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg);
int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
int     drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events);
void   *drm_dev_file_mmap(void *ctx, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma);

/* DRM VFS operation callbacks (registered with devtmpfs at node creation). */
size_t drm_dev_read(void *file, void *addr, size_t offset, size_t size);
size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size);
int    drm_dev_ioctl(void *file, size_t req, void *arg);
int    drm_dev_poll(void *file, size_t events);
void  *drm_dev_mmap(void *file, size_t offset, size_t size, int flags);

#endif // INCLUDE_DRM_INIT_H_
