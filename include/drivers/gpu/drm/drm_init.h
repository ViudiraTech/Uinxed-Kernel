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

/*
 * GPU driver registry.  A GPU driver registers a probe callback with the
 * DRM framework; the core then drives discovery generically instead of a
 * specific accelerator being called directly from the boot path.
 */
#define DRM_MAX_GPU_DRIVERS 16
#define DRM_MAX_DEVICES     16

struct drm_gpu_driver {
        const char *name;
        int (*probe)(void);
};

/* Register a built-in GPU driver probe with the DRM framework. */
int drm_gpu_driver_register(const char *name, int (*probe)(void));

/*
 * Probe every registered GPU driver and attach any that find hardware
 * (multi-GPU machines get a node per driver).  Returns 0 if at least one
 * GPU attached, otherwise the last probe error code; the boot path is then
 * expected to call drm_init_fallback() for the software framebuffer device.
 */
int drm_gpu_probe_all(void);

/* Fallback DRM initialization when no GPU driver can be probed. */
int drm_init_fallback(void);

/* Run the DRM subsystem functional self-test. */
void drm_run_test(void);

/*
 * Look up a registered device by minor type and index; returns with a
 * reference held that the caller must drop with drm_dev_put().
 */
struct drm_device *drm_get_device_by_minor(int type, int index);

/*
 * Iterate the registered device list.  @idx starts at 0; returns the next
 * device (with a reference held) or NULL when the list is exhausted.  The
 * caller must release the reference with drm_dev_put().
 */
struct drm_device *drm_device_list_iter(int *idx);

/*
 * Collect up to @max registered devices into @out, each holding a caller
 * reference (drm_dev_put() to release).  Returns the number collected.
 * Takes the device list lock once; prefer over repeated drm_device_list_iter()
 * in per-tick paths.
 */
int drm_device_list_collect(struct drm_device **out, int max);

/* Add or remove a device from the global device list. */
void drm_device_list_add(struct drm_device *dev);
void drm_device_list_remove(struct drm_device *dev);

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
