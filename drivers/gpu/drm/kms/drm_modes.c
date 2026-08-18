/*
 *
 *      drm_modes.c
 *      DRM display mode helpers
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_idr.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_modeset_lock.h>
#include <drivers/gpu/drm/drm_print.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* Internal helper from drm_mode_object.c */

/*
 * drm_mode_create - Allocate and register a new display mode object.
 * @dev: DRM device
 *
 * Allocates a drm_display_mode, zeroes it, allocates a mode-object ID,
 * and returns the pointer. Returns NULL on failure.
 */
struct drm_display_mode *drm_mode_create(struct drm_device *dev)
{
    struct drm_display_mode *mode;

    if (!dev) return NULL;

    mode = malloc(sizeof(*mode));
    if (!mode) {
        DRM_ERROR("Mode_create: out of memory.\n");
        return NULL;
    }
    memset(mode, 0, sizeof(*mode));

    if (drm_mode_object_idr_alloc(dev, &mode->base, DRM_MODE_OBJECT_MODE)) {
        DRM_ERROR("Mode_create: mode object ID allocation failed.\n");
        free(mode);
        return NULL;
    }

    return mode;
}

/* Unregister and free a display mode object. */
void drm_mode_destroy(struct drm_device *dev, struct drm_display_mode *mode)
{
    if (!dev || !mode) return;

    ilist_remove(&mode->head);

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, mode->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    free(mode);
}

/* Add a probed display mode to a connector's mode list. */
void drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode)
{
    if (!connector || !mode) return;

    ilist_insert_after(&connector->modes, &mode->head);
    mode->connector_count++;
}

/* Copy a display mode (shallow struct copy). */
void drm_mode_copy(struct drm_display_mode *dst, const struct drm_display_mode *src)
{
    struct drm_mode_object base = dst->base;
    ilist_node_t           head = dst->head;

    if (!dst || !src) return;

    memcpy(dst, src, sizeof(*dst));
    dst->base = base;
    dst->head = head;
}

/*
 * drm_convert_umode - Convert a UAPI drm_mode_modeinfo to a kernel drm_display_mode.
 * @umode: pointer to userspace drm_mode_modeinfo
 *
 * Allocates a new drm_display_mode and fills it from the UAPI struct.
 * Note: the caller is responsible for registering the mode object via
 * drm_mode_object_idr_alloc if the mode needs an ID. This function does
 * NOT allocate an ID - it returns a raw struct suitable for probing.
 * Returns the new mode or NULL on allocation failure.
 */
struct drm_display_mode *drm_convert_umode(const struct drm_mode_modeinfo *umode)
{
    struct drm_display_mode *mode;

    if (!umode) return NULL;

    mode = malloc(sizeof(*mode));
    if (!mode) {
        DRM_ERROR("Convert_umode: out of memory.\n");
        return NULL;
    }
    memset(mode, 0, sizeof(*mode));

    mode->clock           = (int)umode->clock;
    mode->hdisplay        = (int)umode->hdisplay;
    mode->hsync_start     = (int)umode->hsync_start;
    mode->hsync_end       = (int)umode->hsync_end;
    mode->htotal          = (int)umode->htotal;
    mode->hskew           = (int)umode->hskew;
    mode->vdisplay        = (int)umode->vdisplay;
    mode->vsync_start     = (int)umode->vsync_start;
    mode->vsync_end       = (int)umode->vsync_end;
    mode->vtotal          = (int)umode->vtotal;
    mode->vscan           = (int)umode->vscan;
    mode->vrefresh        = (int)umode->vrefresh;
    mode->flags           = umode->flags;
    mode->type            = umode->type;
    mode->status          = MODE_OK;
    mode->connector_count = 0;

    strncpy(mode->name, umode->name, DRM_DISPLAY_MODE_LEN - 1);
    mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';

    return mode;
}

/* Convert a kernel drm_display_mode to a UAPI drm_mode_modeinfo. */
void drm_convert_to_umode(struct drm_mode_modeinfo *out, const struct drm_display_mode *in)
{
    if (!out || !in) return;

    memset(out, 0, sizeof(*out));

    out->clock       = (__u32)in->clock;
    out->hdisplay    = (__u16)in->hdisplay;
    out->hsync_start = (__u16)in->hsync_start;
    out->hsync_end   = (__u16)in->hsync_end;
    out->htotal      = (__u16)in->htotal;
    out->hskew       = (__u16)in->hskew;
    out->vdisplay    = (__u16)in->vdisplay;
    out->vsync_start = (__u16)in->vsync_start;
    out->vsync_end   = (__u16)in->vsync_end;
    out->vtotal      = (__u16)in->vtotal;
    out->vscan       = (__u16)in->vscan;
    out->vrefresh    = (__u32)in->vrefresh;
    out->flags       = in->flags;
    out->type        = in->type;

    strncpy(out->name, in->name, DRM_DISPLAY_MODE_LEN - 1);
    out->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
}

/*
 * drm_mode_vrefresh - Get the vrefresh of a mode.
 * @mode: mode
 *
 * Returns the mode's vrefresh rate in Hz, rounded to the nearest integer.
 */
int drm_mode_vrefresh(const struct drm_display_mode *mode)
{
    unsigned int num = 1, den = 1;
    uint64_t     clock, total;

    if (mode->htotal == 0 || mode->vtotal == 0) return 0;

    if (mode->flags & DRM_MODE_FLAG_INTERLACE) num *= 2;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN) den *= 2;
    if (mode->vscan > 1) den *= (unsigned int)mode->vscan;

    clock = (uint64_t)(unsigned int)mode->clock * num;
    total = (uint64_t)(unsigned int)mode->htotal * (unsigned int)mode->vtotal * den;

    if (!total) return 0;

    return (int)((clock * 1000 + total / 2) / total);
}

/*
 * drm_mode_set_name - Set the name on a mode.
 * @mode: name will be set in this mode
 *
 * Set the name of @mode to a standard format which is <hdisplay>x<vdisplay>
 * with an optional 'i' suffix for interlaced modes.
 */
void drm_mode_set_name(struct drm_display_mode *mode)
{
    bool interlaced = !!(mode->flags & DRM_MODE_FLAG_INTERLACE);

    (void)snprintf(mode->name, DRM_DISPLAY_MODE_LEN, "%dx%d%s", mode->hdisplay, mode->vdisplay, interlaced ? "i" : "");
}

/* Allocate and duplicate an existing mode. */
struct drm_display_mode *drm_mode_duplicate(struct drm_device *dev, const struct drm_display_mode *mode)
{
    struct drm_display_mode *nmode;

    nmode = drm_mode_create(dev);
    if (!nmode) return NULL;

    drm_mode_copy(nmode, mode);

    return nmode;
}
