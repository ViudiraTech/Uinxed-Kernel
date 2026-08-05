/*
 *
 *      drm_plane.c
 *      DRM plane management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm_device.h>
#include <drivers/gpu/drm_fourcc.h>
#include <drivers/gpu/drm_idr.h>
#include <drivers/gpu/drm_mode.h>
#include <drivers/gpu/drm_modeset_lock.h>
#include <drivers/gpu/drm_print.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define DRM_S32_MAX ((int32_t)0x7fffffff)

/* Internal helpers from drm_mode_object.c */

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

    ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_fb_id, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_id, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_x, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_y, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_w, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_h, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_x, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_y, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_w, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_h, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_zpos, 0);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_alpha, UINT16_MAX);
    if (!ret) ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_plane_type, type);
    if (ret) {
        drm_plane_cleanup(plane);
        return ret;
    }

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

    uint32_t  user_count = plane_res->count_planes;
    uint32_t  count      = (uint32_t)dev->mode_config.num_total_plane;
    uint32_t  copy_count = user_count < count ? user_count : count;
    uint32_t *ids        = NULL;
    uint32_t  n          = 0;

    if (copy_count) {
        ids = malloc((size_t)count * sizeof(*ids));
        if (!ids) return -ENOMEM;
        for (ilist_node_t *node = dev->mode_config.plane_list.next; node != &dev->mode_config.plane_list; node = node->next)
            ids[n++] = container_of(node, struct drm_plane, head)->base.id;
        if (!plane_res->plane_id_ptr || copy_to_user((void *)(uintptr_t)plane_res->plane_id_ptr, ids, (size_t)copy_count * sizeof(*ids))) {
            free(ids);
            return -EFAULT;
        }
        free(ids);
    }
    plane_res->count_planes = count;

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
    uint32_t                   user_format_count;

    if (!dev || !plane_req) { return -EINVAL; }

    user_format_count = plane_req->count_format_types;
    obj               = drm_mode_object_find(dev, file_priv, plane_req->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!obj) { return -ENOENT; }
    plane = container_of(obj, struct drm_plane, base);

    plane_req->possible_crtcs = plane->possible_crtcs;
    plane_req->crtc_id        = plane->crtc_id;
    plane_req->fb_id          = plane->fb_id;
    plane_req->gamma_size     = 0;

    if (user_format_count) {
        uint32_t count = user_format_count < plane->format_count ? user_format_count : plane->format_count;
        if (!plane_req->format_type_ptr
            || copy_to_user((void *)(uintptr_t)plane_req->format_type_ptr, plane->format_types, (size_t)count * sizeof(*plane->format_types))) {
            drm_mode_object_put(obj);
            return -EFAULT;
        }
    }
    plane_req->count_format_types = plane->format_count;

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
    struct drm_crtc           *crtc = NULL;
    struct drm_framebuffer    *fb   = NULL;
    struct drm_atomic_state   *state;
    struct drm_plane_state    *plane_state;
    struct drm_crtc_state     *crtc_state = NULL;
    int                        ret;

    if (!dev || !plane_req) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, plane_req->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!obj) { return -ENOENT; }
    plane = container_of(obj, struct drm_plane, base);
    if (!!plane_req->crtc_id != !!plane_req->fb_id) {
        ret = -EINVAL;
        goto out;
    }
    if (plane_req->fb_id) {
        struct drm_mode_object *crtc_obj = drm_mode_object_find(dev, file_priv, plane_req->crtc_id, DRM_MODE_OBJECT_CRTC);
        if (!crtc_obj) {
            ret = -ENOENT;
            goto out;
        }
        crtc = container_of(crtc_obj, struct drm_crtc, base);
        drm_mode_object_put(crtc_obj);
        fb = drm_framebuffer_lookup(dev, file_priv, plane_req->fb_id);
        if (!fb) {
            ret = -ENOENT;
            goto out;
        }
        if (plane_req->src_w > DRM_S32_MAX || plane_req->src_h > DRM_S32_MAX || plane_req->crtc_w > DRM_S32_MAX
            || plane_req->crtc_h > DRM_S32_MAX || (int64_t)(int32_t)plane_req->src_x + plane_req->src_w > DRM_S32_MAX
            || (int64_t)(int32_t)plane_req->src_y + plane_req->src_h > DRM_S32_MAX
            || (int64_t)plane_req->crtc_x + plane_req->crtc_w > DRM_S32_MAX || (int64_t)plane_req->crtc_y + plane_req->crtc_h > DRM_S32_MAX) {
            ret = -EINVAL;
            goto out;
        }
    }
    state = drm_atomic_state_alloc(dev);
    if (!state) {
        ret = -ENOMEM;
        goto out;
    }
    state->file_priv = file_priv;
    plane_state      = drm_atomic_get_plane_state(state, plane);
    if (!plane_state) {
        drm_atomic_state_free(state);
        ret = -ENOMEM;
        goto out;
    }
    plane_state->crtc = crtc;
    plane_state->fb   = fb;
    plane_state->src  = (struct drm_rect) {(int32_t)plane_req->src_x, (int32_t)plane_req->src_y, (int32_t)(plane_req->src_x + plane_req->src_w),
                                           (int32_t)(plane_req->src_y + plane_req->src_h)};
    plane_state->dst  = (struct drm_rect) {plane_req->crtc_x, plane_req->crtc_y, plane_req->crtc_x + (int32_t)plane_req->crtc_w,
                                           plane_req->crtc_y + (int32_t)plane_req->crtc_h};
    if (crtc) {
        crtc_state = drm_atomic_get_crtc_state(state, crtc);
        if (!crtc_state) {
            drm_atomic_state_free(state);
            ret = -ENOMEM;
            goto out;
        }
        crtc_state->planes_changed = true;
    } else if (plane->state && plane->state->crtc) {
        crtc_state = drm_atomic_get_crtc_state(state, plane->state->crtc);
        if (!crtc_state) {
            drm_atomic_state_free(state);
            ret = -ENOMEM;
            goto out;
        }
        crtc_state->planes_changed = true;
    }
    ret = drm_atomic_commit(state);
    if (ret) drm_atomic_state_free(state);
out:
    drm_mode_object_put(obj);
    return ret;
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
    if (plane->base.properties) {
        drm_property_set_destroy(plane->base.properties);
        free(plane->base.properties);
        plane->base.properties = NULL;
    }
}
