/*
 *
 *      drm_init.c
 *      DRM subsystem initialization entry point
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Creates a singleton DRM device, registers it, and exposes
 *  /dev/dri/card0 via devtmpfs. Designed to be called once from
 *  kernel_entry() after VFS/devtmpfs are available.
 *
 */

#include <alloc.h>
#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_init.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>
#include <errno.h>
#include <printk.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vfs.h>

/* ------------------------------------------------------------------ */
/* Singleton device                                                    */
/* ------------------------------------------------------------------ */

static struct drm_device *drm_singleton;

struct drm_device *drm_get_singleton(void)
{
    return drm_singleton;
}

/* ------------------------------------------------------------------ */
/* Dummy driver for the built-in DRM node                              */
/* ------------------------------------------------------------------ */

static int drm_dummy_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

static void drm_dummy_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

static void drm_dummy_lastclose(struct drm_device *dev)
{
    (void)dev;
}

static void drm_dummy_gem_free_object(struct drm_gem_object *obj)
{
    if (obj) {
        free(obj->backing);
        obj->backing = NULL;
        free(obj->dma_buf);
        obj->dma_buf = NULL;
    }
}

static struct drm_gem_object *drm_dummy_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    /* For the dummy driver, we can only import buffers that were
     * exported by ourselves. The dma_buf pointer is actually a
     * drm_gem_object pointer. */
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    (void)dev;
    if (!obj) {
        return NULL;
    }
    drm_gem_object_get(obj);
    return obj;
}

static struct drm_driver drm_dummy_driver = {
    .name          = "drm",
    .desc          = "Uinxed DRM",
    .date          = "20260722",
    .major         = 1,
    .minor         = 0,
    .patchlevel    = 0,
    .driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_PRIME | DRIVER_RENDER,
    .open          = drm_dummy_open,
    .postclose     = drm_dummy_postclose,
    .lastclose     = drm_dummy_lastclose,
    .gem_free_object = drm_dummy_gem_free_object,
    .gem_prime_import = drm_dummy_gem_prime_import,
    .dumb_create   = drm_gem_dumb_create,
    .dumb_map_offset = drm_gem_dumb_map_offset,
    .dumb_destroy  = drm_gem_dumb_destroy,
    .ioctls        = NULL,
    .num_ioctls    = 0,
};

/* ------------------------------------------------------------------ */
/* DRM VFS ioctl wrapper                                               */
/* ------------------------------------------------------------------ */

static size_t __attribute__((unused)) drm_dev_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

static size_t __attribute__((unused)) drm_dev_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

static int __attribute__((unused)) drm_dev_ioctl(void *file, size_t req, void *arg)
{
    /* file is the drm_file pointer stored as ctx */
    struct drm_file *file_priv = (struct drm_file *)file;

    if (!file_priv || !drm_singleton) {
        return -ENODEV;
    }

    return drm_ioctl(drm_singleton, (unsigned int)req, arg, file_priv);
}

static int __attribute__((unused)) drm_dev_poll(void *file, size_t events)
{
    (void)file;
    (void)events;
    return 0;
}

/* ------------------------------------------------------------------ */
/* DRM open / release callbacks for devtmpfs                           */
/* ------------------------------------------------------------------ */

/*
 * When userspace opens /dev/dri/card0, tmpfs calls this open callback.
 * We allocate a drm_file and bind it to the VFS node's handle.
 */
void drm_vfs_open_cb(void *parent, const char *name, void *node_ptr)
{
    (void)parent;
    (void)name;

    if (!drm_singleton) {
        return;
    }

    vfs_node_t node = (vfs_node_t)node_ptr;
    if (!node) {
        return;
    }

    struct drm_file *file = malloc(sizeof(*file));
    if (!file) {
        return;
    }

    memset(file, 0, sizeof(*file));
    int ret = drm_open(drm_singleton, file);
    if (ret != 0) {
        free(file);
        return;
    }

    /* Store the drm_file as the node's file-private handle */
    node->handle = file;
}

void drm_vfs_close_cb(void *current)
{
    vfs_node_t node = (vfs_node_t)current;

    if (!node || !node->handle) {
        return;
    }

    struct drm_file *file = (struct drm_file *)node->handle;

    drm_release(file);
    node->handle = NULL;
}

/* ------------------------------------------------------------------ */
/* Public init                                                         */
/* ------------------------------------------------------------------ */

int drm_init(void)
{
    struct drm_device *dev;

    dev = drm_dev_alloc(&drm_dummy_driver);
    if (!dev) {
        DRM_ERROR("Failed to allocate DRM device\n");
        return -ENOMEM;
    }

    int ret = drm_dev_register(dev, 0);
    if (ret != 0) {
        DRM_ERROR("Failed to register DRM device: %d\n", ret);
        free(dev);
        return ret;
    }

    drm_singleton = dev;

    DRM_INFO("DRM subsystem initialized (device: /dev/dri/card0)\n");
    return 0;
}