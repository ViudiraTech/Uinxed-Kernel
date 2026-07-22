/*
 *
 *      drm_drv.c
 *      DRM device lifecycle
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_hashtab.h>
#include <drivers/drm/drm_print.h>
#include <kernel/errno.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

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

    /* Allocate primary minor. */
    minor = malloc(sizeof(*minor));
    if (!minor) {
        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev);
        return NULL;
    }
    memset(minor, 0, sizeof(*minor));
    minor->index            = 0;
    minor->type             = 0;
    minor->dev              = dev;
    minor->device_node_name = "card0";
    dev->primary            = minor;

    /* Allocate render minor. */
    minor = malloc(sizeof(*minor));
    if (!minor) {
        free(dev->primary);
        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev);
        return NULL;
    }
    memset(minor, 0, sizeof(*minor));
    minor->index            = 0;
    minor->type             = 1;
    minor->dev              = dev;
    minor->device_node_name = "renderD128";
    dev->render             = minor;

    return dev;
}

/* ------------------------------------------------------------------ */
/* drm_dev_register — register device, expose KMS defaults             */
/* ------------------------------------------------------------------ */

int drm_dev_register(struct drm_device *dev, uint64_t flags)
{
    if (!dev) { return -EINVAL; }

    dev->mode_config.min_width  = 0;
    dev->mode_config.min_height = 0;
    dev->mode_config.max_width  = 8192;
    dev->mode_config.max_height = 8192;

    if (flags & DRIVER_MODESET) {
        dev->mode_config.cursor_width               = 64;
        dev->mode_config.cursor_height              = 64;
        dev->mode_config.async_page_flip            = false;
        dev->mode_config.fb_modifiers_not_supported = false;
        dev->mode_config.normalize_zpos             = true;
        dev->mode_config.poll_enabled               = true;
    }

    DRM_INFO("Initialized %s %d.%d.%d %s\n", dev->driver->name, dev->driver->major, dev->driver->minor, dev->driver->patchlevel,
             dev->driver->date);

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
    return dev;
}

/* ------------------------------------------------------------------ */
/* drm_dev_put — release a reference; free if no open files remain     */
/* ------------------------------------------------------------------ */

void drm_dev_put(struct drm_device *dev)
{
    if (!dev) { return; }

    if (dev->open_count == 0) {
        if (dev->primary) {
            free(dev->primary);
            dev->primary = NULL;
        }
        if (dev->render) {
            free(dev->render);
            dev->render = NULL;
        }
        drm_idr_destroy(&dev->mode_config.fb_idr);
        drm_idr_destroy(&dev->mode_config.object_idr);
        free(dev);
    }
}

/* ------------------------------------------------------------------ */
/* drm_open — open a /dev/dri file; allocate and init drm_file         */
/* ------------------------------------------------------------------ */

int drm_open(struct drm_device *dev, struct drm_file *file)
{
    int ret;

    if (!dev || !file) { return -EINVAL; }

    /* Zero-initialize the pre-allocated file struct. */
    memset(file, 0, sizeof(*file));

    drm_idr_init(&file->object_idr);
    ilist_init(&file->fbs_head);
    ilist_init(&file->object_list);

    /* Spinlocks zero-initialized by memset above. */

    ret = drm_ht_create(&file->magiclist, 4);
    if (ret) {
        drm_idr_destroy(&file->object_idr);
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

    drm_ht_destroy(&file->magiclist);
    drm_idr_destroy(&file->object_idr);
    free(file);
}