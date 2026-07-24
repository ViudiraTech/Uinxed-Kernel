/*
 *
 *      drm_drv.c
 *      DRM device lifecycle
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_hashtab.h>
#include <drivers/drm/drm_print.h>
#include <fs/devtmpfs.h>
#include <fs/tmpfs.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* Forward: DRM class (registered once by drm_init) */
extern struct class drm_class;
extern int drm_class_registered;

/* ------------------------------------------------------------------ */
/* Minor allocator — per-type bitmaps for indices 0..DRM_MAX_MINOR-1  */
/* ------------------------------------------------------------------ */

static uint64_t   drm_minor_bitmap_primary;
static uint64_t   drm_minor_bitmap_render;
static uint64_t   drm_minor_bitmap_accel;
static spinlock_t drm_minor_lock = {.lock = 0, .rflags = 0};

int drm_minor_alloc(int type)
{
    uint64_t *bm;

    switch (type) {
        case DRM_MINOR_PRIMARY :
            bm = &drm_minor_bitmap_primary;
            break;
        case DRM_MINOR_RENDER :
            bm = &drm_minor_bitmap_render;
            break;
        case DRM_MINOR_ACCEL :
            bm = &drm_minor_bitmap_accel;
            break;
        default :
            return -EINVAL;
    }

    spin_lock(&drm_minor_lock);
    for (int i = 0; i < DRM_MAX_MINOR; i++) {
        if (!(*bm & (1ULL << i))) {
            *bm |= (1ULL << i);
            spin_unlock(&drm_minor_lock);
            return i;
        }
    }
    spin_unlock(&drm_minor_lock);
    return -ENOSPC;
}

void drm_minor_free(int type, int index)
{
    uint64_t *bm;

    if (index < 0 || index >= DRM_MAX_MINOR) return;

    switch (type) {
        case DRM_MINOR_PRIMARY :
            bm = &drm_minor_bitmap_primary;
            break;
        case DRM_MINOR_RENDER :
            bm = &drm_minor_bitmap_render;
            break;
        case DRM_MINOR_ACCEL :
            bm = &drm_minor_bitmap_accel;
            break;
        default :
            return;
    }

    spin_lock(&drm_minor_lock);
    *bm &= ~(1ULL << index);
    spin_unlock(&drm_minor_lock);
}

/* ------------------------------------------------------------------ */
/* drm_master — full definition (forward-declared in drm_device.h)    */
/* ------------------------------------------------------------------ */

struct drm_master {
        struct drm_device   *dev;
        spinlock_t           lock;
        int                  unique_len;
        char                *unique;
        struct drm_open_hash magiclist;
        ilist_node_t         magicfree;
        int                  refcount;
};

/* ------------------------------------------------------------------ */
/* Forward declarations for cross-file helpers (defined in drm_file.c) */
/* ------------------------------------------------------------------ */

struct drm_file *drm_file_alloc(struct drm_device *dev);
void             drm_file_free(struct drm_file *file);

/* ------------------------------------------------------------------ */
/* drm_dev_alloc — allocate and zero-initialize a drm_device           */
/* ------------------------------------------------------------------ */

struct drm_device *drm_dev_alloc(struct drm_driver *driver)
{
    struct drm_device *dev;
    struct drm_minor  *minor;

    if (!driver) { return NULL; }

    dev = malloc(sizeof(*dev));
    if (!dev) { return NULL; }
    memset(dev, 0, sizeof(*dev));

    dev->driver                 = driver;
    dev->num_crtc               = 0;
    dev->vblank_disable_allowed = true;
    dev->refcount               = 1; /* caller's reference */

    /* All spinlocks are zero-initialized by memset above (unlocked state). */

    ilist_init(&dev->filelist);
    ilist_init(&dev->mode_config.fb_list);
    ilist_init(&dev->mode_config.crtc_list);
    ilist_init(&dev->mode_config.connector_list);
    ilist_init(&dev->mode_config.encoder_list);
    ilist_init(&dev->mode_config.plane_list);
    ilist_init(&dev->mode_config.property_list);
    ilist_init(&dev->mode_config.property_blob_list);

    drm_idr_init(&dev->mode_config.object_idr);
    drm_idr_init(&dev->mode_config.fb_idr);

    /* Allocate primary minor (dynamic index). */
    int primary_idx = drm_minor_alloc(DRM_MINOR_PRIMARY);
    if (primary_idx < 0) {
        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev);
        return NULL;
    }
    minor = malloc(sizeof(*minor));
    if (!minor) {
        drm_minor_free(DRM_MINOR_PRIMARY, primary_idx);
        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev);
        return NULL;
    }
    memset(minor, 0, sizeof(*minor));
    minor->index = primary_idx;
    minor->type  = DRM_MINOR_PRIMARY;
    minor->dev   = dev;
    {
        char name[32];
        snprintf(name, sizeof(name), "card%d", primary_idx);
        minor->device_node_name = strdup(name);
    }
    dev->primary = minor;

    /* Allocate render minor (dynamic index). */
    int render_idx = drm_minor_alloc(DRM_MINOR_RENDER);
    if (render_idx < 0) {
        free(dev->primary->device_node_name);
        free(dev->primary);
        drm_minor_free(DRM_MINOR_PRIMARY, primary_idx);
        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev);
        return NULL;
    }
    minor = malloc(sizeof(*minor));
    if (!minor) {
        drm_minor_free(DRM_MINOR_RENDER, render_idx);
        free(dev->primary->device_node_name);
        free(dev->primary);
        drm_minor_free(DRM_MINOR_PRIMARY, primary_idx);
        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev);
        return NULL;
    }
    memset(minor, 0, sizeof(*minor));
    minor->index = render_idx;
    minor->type  = DRM_MINOR_RENDER;
    minor->dev   = dev;
    {
        char name[32];
        snprintf(name, sizeof(name), "renderD%d", 128 + render_idx);
        minor->device_node_name = strdup(name);
    }
    dev->render = minor;

    return dev;
}

/* ------------------------------------------------------------------ */
/* drm_dev_register — register device, expose KMS defaults             */
/* ------------------------------------------------------------------ */

int drm_dev_register(struct drm_device *dev, uint64_t flags)
{
    (void)flags;
    if (!dev) { return -EINVAL; }

    dev->mode_config.min_width  = 0;
    dev->mode_config.min_height = 0;
    dev->mode_config.max_width  = 8192;
    dev->mode_config.max_height = 8192;

    if (dev->driver && (dev->driver->driver_features & DRIVER_MODESET)) {
        dev->mode_config.cursor_width               = 64;
        dev->mode_config.cursor_height              = 64;
        dev->mode_config.async_page_flip            = false;
        dev->mode_config.fb_modifiers_not_supported = false;
        dev->mode_config.normalize_zpos             = true;
        dev->mode_config.poll_enabled               = true;
    }

    DRM_INFO("Initialized %s %d.%d.%d %s\n", dev->driver->name, dev->driver->major, dev->driver->minor, dev->driver->patchlevel,
             dev->driver->date);

    /* Register under /sys/class/drm/ (one entry per GPU) */
    if (drm_class_registered && dev->primary) {
        struct device *ddev = device_create(&drm_class, NULL, MKDEV(226, dev->primary->index), dev, "card%d", dev->primary->index);
        if (ddev) { DRM_INFO("Created /sys/class/drm/%s\n", kobject_name(&ddev->kobj)); }
    }

    /* Register /dev/dri/cardN via devtmpfs. */
    if (dev->primary) {
        char               path[64];
        tmpfs_device_ops_t drm_ops;

        memset(&drm_ops, 0, sizeof(drm_ops));
        drm_ops.ctx = dev;

        snprintf(path, sizeof(path), "/dev/dri/%s", dev->primary->device_node_name);
        int ret = devtmpfs_register_char_device(path, MKDEV(226, dev->primary->index), dev->primary->index, file_stream, &drm_ops);
        if (ret) {
            DRM_ERROR("Failed to register %s: %d\n", path, ret);
        } else {
            dev->dev_node_card0 = (void *)(uintptr_t)1; /* marker */
        }
    }

    /* Register /dev/dri/renderDN if the driver supports rendering. */
    if (dev->render && (dev->driver->driver_features & DRIVER_RENDER)) {
        char               path[64];
        tmpfs_device_ops_t render_ops;

        memset(&render_ops, 0, sizeof(render_ops));
        render_ops.ctx = dev;

        snprintf(path, sizeof(path), "/dev/dri/%s", dev->render->device_node_name);
        int ret = devtmpfs_register_char_device(path, MKDEV(226, dev->render->index), dev->render->index, file_stream, &render_ops);
        if (ret) {
            DRM_ERROR("Failed to register %s: %d\n", path, ret);
        } else {
            dev->dev_node_renderD_unused = (void *)(uintptr_t)1; /* marker */
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_dev_unregister — unregister a device (drop reference)           */
/* ------------------------------------------------------------------ */

void drm_dev_unregister(struct drm_device *dev)
{
    if (!dev) { return; }
    drm_dev_put(dev);
}

/* ------------------------------------------------------------------ */
/* drm_dev_get — acquire a reference to the device                     */
/* ------------------------------------------------------------------ */

struct drm_device *drm_dev_get(struct drm_device *dev)
{
    if (!dev) return NULL;

    spin_lock(&dev->ref_lock);
    if (dev->unplugged) {
        spin_unlock(&dev->ref_lock);
        return NULL;
    }
    dev->refcount++;
    spin_unlock(&dev->ref_lock);
    return dev;
}

/* ------------------------------------------------------------------ */
/* drm_dev_put — release a reference; free when refcount hits zero     */
/* ------------------------------------------------------------------ */

void drm_dev_put(struct drm_device *dev)
{
    int new_ref;

    if (!dev) return;

    spin_lock(&dev->ref_lock);
    new_ref = --dev->refcount;
    spin_unlock(&dev->ref_lock);

    if (new_ref == 0) {
        /* Remove from global device list (defined in drm_init.c). */
        extern void drm_device_list_remove(struct drm_device * d);
        drm_device_list_remove(dev);

        /* Call driver release hook. */
        if (dev->driver && dev->driver->release) dev->driver->release(dev);

        /* Free minors and their indices. */
        if (dev->primary) {
            drm_minor_free(dev->primary->type, dev->primary->index);
            free(dev->primary->device_node_name);
            free(dev->primary);
            dev->primary = NULL;
        }
        if (dev->render) {
            drm_minor_free(dev->render->type, dev->render->index);
            free(dev->render->device_node_name);
            free(dev->render);
            dev->render = NULL;
        }

        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev->unique);
        free(dev->busid_str);
        free(dev);
    }
}

/* ------------------------------------------------------------------ */
/* drm_dev_unplug — mark device as removed, prevent new opens          */
/* ------------------------------------------------------------------ */

void drm_dev_unplug(struct drm_device *dev)
{
    if (!dev) return;

    spin_lock(&dev->ref_lock);
    dev->unplugged = 1;
    spin_unlock(&dev->ref_lock);
}

/* ------------------------------------------------------------------ */
/* drm_open — open a /dev/dri file; allocate and init drm_file         */
/* ------------------------------------------------------------------ */

int drm_open(struct drm_device *dev, struct drm_file *file)
{
    int ret;

    if (!dev || !file) { return -EINVAL; }

    /* Acquire a reference to the device for the lifetime of this
     * open file. This prevents the device from being freed while
     * the file is still open. */
    if (!drm_dev_get(dev)) return -ENODEV;

    /* Zero-initialize the pre-allocated file struct. */
    memset(file, 0, sizeof(*file));

    drm_idr_init(&file->object_idr);
    ilist_init(&file->fbs_head);
    ilist_init(&file->object_list);

    ret = drm_ht_create(&file->magiclist, 4);
    if (ret) {
        drm_idr_destroy(&file->object_idr);
        drm_dev_put(dev);
        return ret;
    }

    file->authenticated        = false;
    file->universal_planes     = false;
    file->atomic               = false;
    file->aspect_ratio_allowed = false;

    /* Store back-pointer to device for use in drm_release. */
    file->minor_unused = dev;

    spin_lock(&dev->filelist_lock);
    ilist_insert_after(&dev->filelist, &file->head);
    dev->open_count++;
    spin_unlock(&dev->filelist_lock);

    if (dev->driver && dev->driver->open) {
        ret = dev->driver->open(dev, file);
        if (ret) {
            spin_lock(&dev->filelist_lock);
            ilist_remove(&file->head);
            dev->open_count--;
            spin_unlock(&dev->filelist_lock);
            drm_ht_destroy(&file->magiclist);
            drm_idr_destroy(&file->object_idr);
            drm_dev_put(dev);
            return ret;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_release — close a /dev/dri file; cleanup and free drm_file      */
/* ------------------------------------------------------------------ */

void drm_release(struct drm_file *file)
{
    struct drm_device *dev;

    if (!file) { return; }

    dev = (struct drm_device *)file->minor_unused;

    if (dev) {
        spin_lock(&dev->filelist_lock);
        ilist_remove(&file->head);
        dev->open_count--;
        spin_unlock(&dev->filelist_lock);

        if (dev->driver && dev->driver->postclose) { dev->driver->postclose(dev, file); }

        if (dev->open_count == 0 && dev->driver && dev->driver->lastclose) { dev->driver->lastclose(dev); }
    } else {
        ilist_remove(&file->head);
    }

    /* Release any GEM handles still held by this file. */
    {
        ilist_node_t *node = file->object_list.next;
        while (node && node != &file->object_list) {
            struct drm_gem_object *obj = container_of(node, struct drm_gem_object, handle_list_node);
            node                       = node->next;
            ilist_remove(&obj->handle_list_node);
            obj->handle_count--;
            drm_gem_object_put(obj);
        }
    }

    drm_file_free(file);

    /* Release the device reference acquired in drm_open. */
    if (dev) drm_dev_put(dev);
}