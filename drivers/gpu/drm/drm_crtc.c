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
extern struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev,
                                                       struct drm_file *file_priv,
                                                       uint32_t id);

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
 * Looks up the CRTC and framebuffer. Validates the mode parameters
 * (clock, hdisplay, vdisplay, sync ranges). Programs the CRTC with
 * the new mode and binds the framebuffer to the primary plane.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc *crtc_req = (struct drm_mode_crtc *)data;
    struct drm_mode_object *obj;
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb = NULL;

    if (!dev || !crtc_req) {
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, file_priv, crtc_req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!obj) {
        return -ENOENT;
    }
    crtc = container_of(obj, struct drm_crtc, base);

    /* Look up the framebuffer if specified */
    if (crtc_req->fb_id != 0) {
        fb = drm_framebuffer_lookup(dev, file_priv, crtc_req->fb_id);
        if (!fb) {
            drm_mode_object_put(obj);
            return -ENOENT;
        }
    }

    crtc->x = (int)crtc_req->x;
    crtc->y = (int)crtc_req->y;

    if (crtc_req->mode_valid) {
        struct drm_display_mode *mode = &crtc->mode;

        /* Validate mode parameters */
        if (crtc_req->mode.clock == 0 ||
            crtc_req->mode.hdisplay == 0 ||
            crtc_req->mode.vdisplay == 0) {
            drm_mode_object_put(obj);
            return -EINVAL;
        }

        /* Validate sync ranges: hsync_start <= hsync_end <= htotal */
        if (crtc_req->mode.hsync_start > crtc_req->mode.hsync_end ||
            crtc_req->mode.hsync_end > crtc_req->mode.htotal) {
            drm_mode_object_put(obj);
            return -EINVAL;
        }

        /* Validate sync ranges: vsync_start <= vsync_end <= vtotal */
        if (crtc_req->mode.vsync_start > crtc_req->mode.vsync_end ||
            crtc_req->mode.vsync_end > crtc_req->mode.vtotal) {
            drm_mode_object_put(obj);
            return -EINVAL;
        }

        /* Validate htotal/vtotal are non-zero */
        if (crtc_req->mode.htotal == 0 || crtc_req->mode.vtotal == 0) {
            drm_mode_object_put(obj);
            return -EINVAL;
        }

        /* Validate dimensions against mode_config limits */
        if (crtc_req->mode.hdisplay > dev->mode_config.max_width ||
            crtc_req->mode.vdisplay > dev->mode_config.max_height) {
            drm_mode_object_put(obj);
            return -EINVAL;
        }

        /* Convert UAPI modeinfo to internal display mode */
        mode->clock        = (int)crtc_req->mode.clock;
        mode->hdisplay     = (int)crtc_req->mode.hdisplay;
        mode->hsync_start  = (int)crtc_req->mode.hsync_start;
        mode->hsync_end    = (int)crtc_req->mode.hsync_end;
        mode->htotal       = (int)crtc_req->mode.htotal;
        mode->hskew        = (int)crtc_req->mode.hskew;
        mode->vdisplay     = (int)crtc_req->mode.vdisplay;
        mode->vsync_start  = (int)crtc_req->mode.vsync_start;
        mode->vsync_end    = (int)crtc_req->mode.vsync_end;
        mode->vtotal       = (int)crtc_req->mode.vtotal;
        mode->vscan        = (int)crtc_req->mode.vscan;
        mode->vrefresh     = (int)crtc_req->mode.vrefresh;
        mode->flags        = crtc_req->mode.flags;
        mode->type         = crtc_req->mode.type;
        strncpy(mode->name, crtc_req->mode.name, DRM_DISPLAY_MODE_LEN - 1);
        mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
        mode->status       = MODE_OK;

        /* Bind the framebuffer to the primary plane */
        if (fb && crtc->primary) {
            crtc->primary->fb_id = crtc_req->fb_id;
            if (crtc->primary->state) {
                crtc->primary->state->fb = fb;
            }
        }

        crtc->enabled = true;

        DRM_DEBUG_KMS("CRTC %u: mode %s %dx%d@%d clock=%dkHz\n",
                      crtc->base.id, mode->name,
                      mode->hdisplay, mode->vdisplay,
                      mode->vrefresh, mode->clock);
    } else {
        /* Mode is being disabled */
        crtc->enabled = false;
        if (crtc->primary) {
            crtc->primary->fb_id = 0;
            if (crtc->primary->state) {
                crtc->primary->state->fb = NULL;
            }
        }
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