/*
 *
 *      drm_modes.c
 *      DRM display mode helpers
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_fourcc.h>
#include <drivers/drm/drm_idr.h>
#include <drivers/drm/drm_mode.h>
#include <drivers/drm/drm_modeset_lock.h>
#include <drivers/drm/drm_print.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Internal helper from drm_mode_object.c */
extern int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

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

    if (!dev) { return NULL; }

    mode = malloc(sizeof(*mode));
    if (!mode) { return NULL; }
    memset(mode, 0, sizeof(*mode));

    if (drm_mode_object_idr_alloc(dev, &mode->base, DRM_MODE_OBJECT_MODE)) {
        free(mode);
        return NULL;
    }

    return mode;
}

/*
 * drm_mode_destroy - Unregister and free a display mode object.
 * @dev: DRM device
 * @mode: display mode to destroy
 *
 * Removes the mode from the global IDR, unlinks it from any list it
 * is on, and frees the struct.
 */
void drm_mode_destroy(struct drm_device *dev, struct drm_display_mode *mode)
{
    if (!dev || !mode) { return; }

    ilist_remove(&mode->head);

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, mode->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    free(mode);
}

/*
 * drm_mode_probed_add - Add a probed display mode to a connector's mode list.
 * @connector: connector
 * @mode: display mode to add
 *
 * Inserts the mode into the connector's modes list and increments the
 * mode's connector_count. The mode must have been allocated with
 * drm_mode_create() or drm_mode_duplicate().
 */
void drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode)
{
    if (!connector || !mode) { return; }

    ilist_insert_after(&connector->modes, &mode->head);
    mode->connector_count++;
}

/*
 * drm_mode_copy - Copy a display mode (shallow struct copy).
 * @dst: destination mode
 * @src: source mode
 *
 * Copies all fields of the display mode from src to dst using memcpy.
 */
void drm_mode_copy(struct drm_display_mode *dst, const struct drm_display_mode *src)
{
    if (!dst || !src) { return; }

    memcpy(dst, src, sizeof(*dst));
}

/*
 * drm_mode_equal - Compare two display modes for equality.
 * @mode1: first mode
 * @mode2: second mode
 *
 * Compares clock, hdisplay, vdisplay, flags, type, and the mode name.
 * Returns true if the modes are equal, false otherwise.
 */
bool drm_mode_equal(const struct drm_display_mode *mode1, const struct drm_display_mode *mode2)
{
    if (!mode1 || !mode2) { return false; }

    if (mode1->clock != mode2->clock || mode1->hdisplay != mode2->hdisplay || mode1->vdisplay != mode2->vdisplay || mode1->flags != mode2->flags
        || mode1->type != mode2->type) {
        return false;
    }

    return true;
}

/*
 * drm_convert_umode - Convert a UAPI drm_mode_modeinfo to a kernel drm_display_mode.
 * @umode: pointer to userspace drm_mode_modeinfo
 *
 * Allocates a new drm_display_mode and fills it from the UAPI struct.
 * Note: the caller is responsible for registering the mode object via
 * drm_mode_object_idr_alloc if the mode needs an ID. This function does
 * NOT allocate an ID — it returns a raw struct suitable for probing.
 * Returns the new mode or NULL on allocation failure.
 */
struct drm_display_mode *drm_convert_umode(const struct drm_mode_modeinfo *umode)
{
    struct drm_display_mode *mode;

    if (!umode) { return NULL; }

    mode = malloc(sizeof(*mode));
    if (!mode) { return NULL; }
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

/*
 * drm_convert_to_umode - Convert a kernel drm_display_mode to a UAPI drm_mode_modeinfo.
 * @out: destination UAPI struct
 * @in: source kernel display mode
 *
 * Fills the UAPI struct fields from the kernel display mode.
 */
void drm_convert_to_umode(struct drm_mode_modeinfo *out, const struct drm_display_mode *in)
{
    if (!out || !in) { return; }

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
 * drm_mode_debug_printmodeline - Print a display mode in modeline format.
 * @mode: display mode to print
 *
 * Outputs the mode via DRM_DEBUG_KMS in the format:
 *   "name" clock hdisp hsync-start hsync-end htotal vdisp vsync-start vsync-end vtotal flags type
 */
void drm_mode_debug_printmodeline(const struct drm_display_mode *mode)
{
    if (!mode) {
        DRM_DEBUG_KMS("modeline: (null)\n");
        return;
    }

    DRM_DEBUG_KMS("modeline \"%s\": %d %d %d %d %d %d %d %d %d 0x%x 0x%x\n", mode->name, mode->clock, mode->hdisplay, mode->hsync_start,
                  mode->hsync_end, mode->htotal, mode->vdisplay, mode->vsync_start, mode->vsync_end, mode->vtotal, mode->flags, mode->type);
}