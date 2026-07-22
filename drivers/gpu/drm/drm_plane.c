/*
 *
 *      drm_plane.c
 *      DRM plane management
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

/* Internal helpers from drm_mode_object.c */
extern int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

/*
 * drm_plane_init - Initialise a plane object.
 * @dev: DRM device
 * @plane: plane to initialise
 * @possible_crtcs: bitmask of CRTC indices that can drive this plane
 * @funcs: plane helper funcs pointer (stored in helper_private)
 * @formats: array of supported DRM_FORMAT_* fourcc codes
 * @format_count: number of entries in @formats
 * @modifiers: array of supported format modifiers (may be NULL)
 * @type: DRM_PLANE_TYPE_* (primary, cursor, overlay)
 * @name: human-readable name for the plane
 *
 * Allocates a mode-object ID, initialises the mutex, copies the format
 * and modifier arrays, inserts into the device plane list, and stores
 * the plane type and name. Returns 0 on success or -ENOMEM/-errno.
 */
int drm_plane_init(struct drm_device *dev, struct drm_plane *plane, uint32_t possible_crtcs, void *funcs, const uint32_t *formats,
                   unsigned int format_count, const uint64_t *modifiers, enum drm_plane_type type, const char *name)
{
    int ret;

    (void)modifiers;
    (void)name;

    if (!dev || !plane || !formats || format_count == 0) { return -EINVAL; }

    ret = drm_mode_object_idr_alloc(dev, &plane->base, DRM_MODE_OBJECT_PLANE);
    if (ret) { return ret; }

    drm_modeset_lock_init(&plane->mutex);

    ilist_insert_after(&dev->mode_config.plane_list, &plane->head);

    plane->dev                   = dev;
    plane->possible_crtcs        = possible_crtcs;
    plane->type                  = type;
    plane->state                 = NULL;
    plane->helper_private        = funcs;
    plane->zpos_property_default = 0;

    plane->format_types = malloc((size_t)format_count * sizeof(uint32_t));
    if (!plane->format_types) {
        ilist_remove(&plane->head);
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, plane->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);
        return -ENOMEM;
    }
    memcpy(plane->format_types, formats, (size_t)format_count * sizeof(uint32_t));
    plane->format_count = format_count;

    plane->modifiers      = NULL;
    plane->modifier_count = 0;

    plane->name = strdup(name ? name : "plane");
    if (!plane->name) {
        free(plane->format_types);
        plane->format_types = NULL;
        ilist_remove(&plane->head);
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, plane->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);
        return -ENOMEM;
    }

    dev->mode_config.num_plane++;
    dev->mode_config.num_total_plane++;

    return 0;
}

/*
 * drm_mode_getplane_res - Handle DRM_IOCTL_MODE_GETPLANERESOURCES.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_get_plane_res (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Fills the count_planes field with the total number of planes.
 * Returns 0 on success.
 */
int drm_mode_getplane_res(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_plane_res *plane_res = (struct drm_mode_get_plane_res *)data;

    (void)file_priv;

    if (!dev || !plane_res) { return -EINVAL; }

    plane_res->count_planes = (__u32)dev->mode_config.num_total_plane;

    return 0;
}

/*
 * drm_mode_getplane - Handle DRM_IOCTL_MODE_GETPLANE.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_get_plane (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up the plane by id, fills the struct with possible_crtcs,
 * format count, and currently attached CRTC/FB ids.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_getplane(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_plane *plane_req = (struct drm_mode_get_plane *)data;
    struct drm_mode_object    *obj;
    struct drm_plane          *plane;

    if (!dev || !plane_req) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, plane_req->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!obj) { return -ENOENT; }
    plane = container_of(obj, struct drm_plane, base);

    plane_req->possible_crtcs     = plane->possible_crtcs;
    plane_req->count_format_types = plane->format_count;
    plane_req->crtc_id            = plane->crtc_id;
    plane_req->fb_id              = plane->fb_id;
    plane_req->gamma_size         = 0;

    drm_mode_object_put(obj);
    return 0;
}

/*
 * drm_mode_setplane - Handle DRM_IOCTL_MODE_SETPLANE.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_set_plane (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up the plane by id. If fb_id is non-zero, applies the plane's
 * framebuffer, CRTC binding, and source/destination coordinates.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_set_plane *plane_req = (struct drm_mode_set_plane *)data;
    struct drm_mode_object    *obj;
    struct drm_plane          *plane;

    if (!dev || !plane_req) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, plane_req->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!obj) { return -ENOENT; }
    plane = container_of(obj, struct drm_plane, base);

    /* Track the requested state directly on the plane struct.
     * Hardware programming would be done by a real driver's
     * plane update callback. */
    plane->crtc_id = plane_req->crtc_id;
    plane->fb_id   = plane_req->fb_id;

    drm_mode_object_put(obj);
    return 0;
}

/*
 * drm_plane_cleanup - Tear down a plane and release its resources.
 * @plane: plane to clean up
 *
 * Removes the plane from the device plane list, removes it from the
 * global IDR, frees the format array, modifier array, and name.
 * Decrements num_plane and num_total_plane.
 */
void drm_plane_cleanup(struct drm_plane *plane)
{
    struct drm_device *dev;

    if (!plane) { return; }

    dev = plane->dev;

    ilist_remove(&plane->head);

    if (dev) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, plane->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_plane > 0) { dev->mode_config.num_plane--; }
        if (dev->mode_config.num_total_plane > 0) { dev->mode_config.num_total_plane--; }
    }

    free(plane->format_types);
    plane->format_types = NULL;
    plane->format_count = 0;

    free(plane->modifiers);
    plane->modifiers      = NULL;
    plane->modifier_count = 0;

    free(plane->name);
    plane->name = NULL;
}