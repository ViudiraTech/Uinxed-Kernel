/*
 *
 *      drm_crtc.c
 *      DRM CRTC management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_device.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_idr.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <alloc.h>
#include <errno.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Internal helper from drm_mode_object.c */
extern int  drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

/*
 * drm_crtc_init_with_planes - Initialise a new CRTC object with primary and cursor planes.
 * @dev: DRM device
 * @crtc: CRTC object to initialise
 * @primary: primary plane to attach (may be NULL)
 * @cursor: cursor plane to attach (may be NULL)
 * @funcs: CRTC helper funcs pointer (stored in helper_private)
 * @name: name of the CRTC (unused in MVP, kept for API compatibility)
 *
 * Allocates a mode-object ID, initialises the mutex and spinlocks,
 * inserts the CRTC into the device's crtc_list, and sets defaults.
 * Returns 0 on success or a negative errno on failure.
 */
int drm_crtc_init_with_planes(struct drm_device *dev, struct drm_crtc *crtc,
                              struct drm_plane *primary, struct drm_plane *cursor,
                              void *funcs, const char *name)
{
    int ret;

    (void)name;

    if (!dev || !crtc) {
        return -EINVAL;
    }

    ret = drm_mode_object_idr_alloc(dev, &crtc->base, DRM_MODE_OBJECT_CRTC);
    if (ret) {
        return ret;
    }

    drm_modeset_lock_init(&crtc->mutex);

    memset(&crtc->commit_lock, 0, sizeof(crtc->commit_lock));
    memset(&crtc->spinlock, 0, sizeof(crtc->spinlock));

    crtc->dev          = dev;
    crtc->primary      = primary;
    crtc->cursor       = cursor;
    crtc->legacy_cursor = NULL;
    crtc->mode_config  = &dev->mode_config;
    crtc->index        = dev->mode_config.num_crtc++;
    crtc->enabled      = false;
    crtc->gamma_size   = 256;
    crtc->gamma_store  = NULL;
    crtc->state        = NULL;
    crtc->commit_state = NULL;
    crtc->helper_private = funcs;
    crtc->x            = 0;
    crtc->y            = 0;

    memset(&crtc->mode, 0, sizeof(crtc->mode));
    memset(&crtc->saved_mode, 0, sizeof(crtc->saved_mode));

    ilist_insert_after(&dev->mode_config.crtc_list, &crtc->head);

    return 0;
}

/*
 * drm_crtc_create_properties - Create the standard CRTC KMS properties.
 * @dev: DRM device
 *
 * Creates ACTIVE, MODE_ID, and OUT_FENCE_PTR properties for all registered
 * CRTCs. In the MVP this is a stub; returns 0.
 */
int drm_crtc_create_properties(struct drm_device *dev)
{
    if (!dev) {
        return -EINVAL;
    }

    /* MVP stub: standard properties will be created by a future
     * drm_property_create_range / drm_property_create_object call. */
    return 0;
}

/*
 * drm_crtc_set_mode_prop_for_crtc - Set the current mode and enable the CRTC.
 * @crtc: CRTC to update
 * @mode: display mode to apply
 *
 * Copies the mode into crtc->mode and marks the CRTC as enabled.
 */
void drm_crtc_set_mode_prop_for_crtc(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
    if (!crtc || !mode) {
        return;
    }

    memcpy(&crtc->mode, mode, sizeof(crtc->mode));
    crtc->enabled = true;
}

/*
 * drm_mode_getcrtc - Handle DRM_IOCTL_MODE_GETCRTC.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_crtc (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up the CRTC by id, fills the drm_mode_crtc struct with the
 * current CRTC state (fb_id, position, mode, gamma_size), and returns
 * the mode_valid flag. Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_getcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc *crtc_req = (struct drm_mode_crtc *)data;
    struct drm_mode_object *obj;
    struct drm_crtc *crtc;

    if (!dev || !crtc_req) {
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, file_priv, crtc_req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!obj) {
        return -ENOENT;
    }
    crtc = container_of(obj, struct drm_crtc, base);

    crtc_req->fb_id      = 0; /* MVP: fb_id tracking not yet wired */
    crtc_req->x          = (__u32)crtc->x;
    crtc_req->y          = (__u32)crtc->y;
    crtc_req->gamma_size = crtc->gamma_size;
    crtc_req->mode_valid = crtc->enabled ? 1 : 0;

    /* Convert the internal display mode to UAPI modeinfo */
    crtc_req->mode.clock     = (__u32)crtc->mode.clock;
    crtc_req->mode.hdisplay  = (__u16)crtc->mode.hdisplay;
    crtc_req->mode.hsync_start = (__u16)crtc->mode.hsync_start;
    crtc_req->mode.hsync_end   = (__u16)crtc->mode.hsync_end;
    crtc_req->mode.htotal    = (__u16)crtc->mode.htotal;
    crtc_req->mode.hskew     = (__u16)crtc->mode.hskew;
    crtc_req->mode.vdisplay  = (__u16)crtc->mode.vdisplay;
    crtc_req->mode.vsync_start = (__u16)crtc->mode.vsync_start;
    crtc_req->mode.vsync_end   = (__u16)crtc->mode.vsync_end;
    crtc_req->mode.vtotal    = (__u16)crtc->mode.vtotal;
    crtc_req->mode.vscan     = (__u16)crtc->mode.vscan;
    crtc_req->mode.vrefresh  = (__u32)crtc->mode.vrefresh;
    crtc_req->mode.flags     = crtc->mode.flags;
    crtc_req->mode.type      = crtc->mode.type;
    strncpy(crtc_req->mode.name, crtc->mode.name, DRM_DISPLAY_MODE_LEN - 1);
    crtc_req->mode.name[DRM_DISPLAY_MODE_LEN - 1] = '\0';

    drm_mode_object_put(obj);
    return 0;
}

/*
 * drm_mode_setcrtc - Handle DRM_IOCTL_MODE_SETCRTC.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_crtc (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up the CRTC. If fb_id is non-zero, looks up the framebuffer.
 * Sets the CRTC's position and applies the incoming mode if mode_valid.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc *crtc_req = (struct drm_mode_crtc *)data;
    struct drm_mode_object *obj;
    struct drm_crtc *crtc;

    if (!dev || !crtc_req) {
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, file_priv, crtc_req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!obj) {
        return -ENOENT;
    }
    crtc = container_of(obj, struct drm_crtc, base);

    crtc->x = (int)crtc_req->x;
    crtc->y = (int)crtc_req->y;

    if (crtc_req->mode_valid) {
        /* Convert UAPI modeinfo to internal display mode */
        crtc->mode.clock        = (int)crtc_req->mode.clock;
        crtc->mode.hdisplay     = (int)crtc_req->mode.hdisplay;
        crtc->mode.hsync_start  = (int)crtc_req->mode.hsync_start;
        crtc->mode.hsync_end    = (int)crtc_req->mode.hsync_end;
        crtc->mode.htotal       = (int)crtc_req->mode.htotal;
        crtc->mode.hskew        = (int)crtc_req->mode.hskew;
        crtc->mode.vdisplay     = (int)crtc_req->mode.vdisplay;
        crtc->mode.vsync_start  = (int)crtc_req->mode.vsync_start;
        crtc->mode.vsync_end    = (int)crtc_req->mode.vsync_end;
        crtc->mode.vtotal       = (int)crtc_req->mode.vtotal;
        crtc->mode.vscan        = (int)crtc_req->mode.vscan;
        crtc->mode.vrefresh     = (int)crtc_req->mode.vrefresh;
        crtc->mode.flags        = crtc_req->mode.flags;
        crtc->mode.type         = crtc_req->mode.type;
        strncpy(crtc->mode.name, crtc_req->mode.name, DRM_DISPLAY_MODE_LEN - 1);
        crtc->mode.name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
        crtc->enabled = true;
    }

    drm_mode_object_put(obj);
    return 0;
}

/*
 * drm_crtc_cleanup - Tear down a CRTC and release its resources.
 * @crtc: CRTC to clean up
 *
 * Removes the CRTC from the device CRTC list, removes it from the
 * global IDR, frees the gamma store, and decrements num_crtc.
 */
void drm_crtc_cleanup(struct drm_crtc *crtc)
{
    struct drm_device *dev;

    if (!crtc) {
        return;
    }

    dev = crtc->dev;

    ilist_remove(&crtc->head);

    if (dev) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, crtc->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_crtc > 0) {
            dev->mode_config.num_crtc--;
        }
    }

    free(crtc->gamma_store);
    crtc->gamma_store = NULL;
}