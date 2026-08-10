/*
 *
 *      drm_crtc.c
 *      DRM CRTC management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_idr.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_modeset_lock.h>
#include <drivers/gpu/drm/drm_print.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define DRM_S32_MAX ((int32_t)0x7fffffff)

/* Internal helper from drm_mode_object.c */

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
int drm_crtc_init_with_planes(struct drm_device *dev, struct drm_crtc *crtc, struct drm_plane *primary, struct drm_plane *cursor, void *funcs,
                              const char *name)
{
    int ret;

    (void)name;

    if (!dev || !crtc) {
        plogk("drm_crtc: Init_with_planes with NULL dev or crtc.\n");
        return -EINVAL;
    }

    ret = drm_mode_object_idr_alloc(dev, &crtc->base, DRM_MODE_OBJECT_CRTC);
    if (ret) {
        plogk("drm_crtc: Failed to allocate object id (ret=%d)\n", ret);
        return ret;
    }

    drm_modeset_lock_init(&crtc->mutex);

    memset(&crtc->commit_lock, 0, sizeof(crtc->commit_lock));
    memset(&crtc->spinlock, 0, sizeof(crtc->spinlock));

    crtc->dev            = dev;
    crtc->primary        = primary;
    crtc->cursor         = cursor;
    crtc->legacy_cursor  = NULL;
    crtc->cursor_obj     = NULL;
    crtc->mode_config    = &dev->mode_config;
    crtc->index          = dev->mode_config.num_crtc++;
    crtc->enabled        = false;
    crtc->gamma_size     = 256;
    crtc->gamma_store    = NULL;
    crtc->state          = NULL;
    crtc->commit_state   = NULL;
    crtc->helper_private = funcs;
    crtc->x              = 0;
    crtc->y              = 0;

    memset(&crtc->mode, 0, sizeof(crtc->mode));
    memset(&crtc->saved_mode, 0, sizeof(crtc->saved_mode));

    ilist_insert_after(&dev->mode_config.crtc_list, &crtc->head);

    ret = drm_object_attach_property(&crtc->base, dev->mode_config.prop_active, 0);
    if (!ret) ret = drm_object_attach_property(&crtc->base, dev->mode_config.prop_mode_id, 0);
    if (ret) {
        plogk("drm_crtc: Failed to attach properties (ret=%d)\n", ret);
        drm_crtc_cleanup(crtc);
        return ret;
    }

    return 0;
}

/*
 * drm_crtc_create_properties - Create the standard CRTC KMS properties.
 * @dev: DRM device
 *
 * Placeholder for creating ACTIVE, MODE_ID, and OUT_FENCE_PTR properties
 * on all registered CRTCs. Property creation is deferred until the
 * drm_property_create_* infrastructure is wired in.
 */
static int drm_crtc_create_properties(struct drm_device *dev)
{
    if (!dev) {
        plogk("drm_crtc: Create_properties with NULL dev.\n");
        return -EINVAL;
    }

    return 0;
}

/*
 * drm_crtc_set_mode_prop_for_crtc - Set the current mode and enable the CRTC.
 * @crtc: CRTC to update
 * @mode: display mode to apply
 *
 * Copies the mode into crtc->mode and marks the CRTC as enabled.
 */
static void drm_crtc_set_mode_prop_for_crtc(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
    if (!crtc || !mode) {
        plogk("drm_crtc: Set_mode_prop with NULL crtc or mode.\n");
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
    struct drm_mode_crtc   *crtc_req = (struct drm_mode_crtc *)data;
    struct drm_mode_object *obj;
    struct drm_crtc        *crtc;

    if (!dev || !crtc_req) {
        plogk("drm_crtc: Getcrtc with invalid args.\n");
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, file_priv, crtc_req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!obj) {
        plogk("drm_crtc: Crtc %u not found.\n", crtc_req->crtc_id);
        return -ENOENT;
    }
    crtc = container_of(obj, struct drm_crtc, base);

    crtc_req->fb_id      = crtc->primary ? crtc->primary->fb_id : 0;
    crtc_req->x          = (__u32)crtc->x;
    crtc_req->y          = (__u32)crtc->y;
    crtc_req->gamma_size = crtc->gamma_size;
    crtc_req->mode_valid = crtc->enabled ? 1 : 0;

    /* Convert the internal display mode to UAPI modeinfo */
    crtc_req->mode.clock       = (__u32)crtc->mode.clock;
    crtc_req->mode.hdisplay    = (__u16)crtc->mode.hdisplay;
    crtc_req->mode.hsync_start = (__u16)crtc->mode.hsync_start;
    crtc_req->mode.hsync_end   = (__u16)crtc->mode.hsync_end;
    crtc_req->mode.htotal      = (__u16)crtc->mode.htotal;
    crtc_req->mode.hskew       = (__u16)crtc->mode.hskew;
    crtc_req->mode.vdisplay    = (__u16)crtc->mode.vdisplay;
    crtc_req->mode.vsync_start = (__u16)crtc->mode.vsync_start;
    crtc_req->mode.vsync_end   = (__u16)crtc->mode.vsync_end;
    crtc_req->mode.vtotal      = (__u16)crtc->mode.vtotal;
    crtc_req->mode.vscan       = (__u16)crtc->mode.vscan;
    crtc_req->mode.vrefresh    = (__u32)crtc->mode.vrefresh;
    crtc_req->mode.flags       = crtc->mode.flags;
    crtc_req->mode.type        = crtc->mode.type;
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
    struct drm_mode_crtc    *crtc_req = (struct drm_mode_crtc *)data;
    struct drm_mode_object  *obj;
    struct drm_crtc         *crtc;
    struct drm_framebuffer  *fb    = NULL;
    struct drm_atomic_state *state = NULL;
    struct drm_crtc_state   *crtc_state;
    struct drm_plane_state  *plane_state = NULL;
    struct drm_display_mode  mode;
    uint32_t                *connector_ids = NULL;
    int                      ret           = 0;

    if (!dev || !crtc_req) {
        plogk("drm_crtc: Setcrtc with invalid args.\n");
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, file_priv, crtc_req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!obj) {
        plogk("drm_crtc: Crtc %u not found.\n", crtc_req->crtc_id);
        return -ENOENT;
    }
    crtc = container_of(obj, struct drm_crtc, base);

    /* Look up the framebuffer if specified */
    if (crtc_req->fb_id != 0) {
        fb = drm_framebuffer_lookup(dev, file_priv, crtc_req->fb_id);
        if (!fb) {
            plogk("drm_crtc: Fb %u not found.\n", crtc_req->fb_id);
            drm_mode_object_put(obj);
            return -ENOENT;
        }
    }

    if (crtc_req->mode_valid) {
        /* Validate mode parameters */
        if (!fb || crtc_req->mode.clock == 0 || crtc_req->mode.hdisplay == 0 || crtc_req->mode.vdisplay == 0) {
            plogk("drm_crtc: Invalid mode parameters.\n");
            ret = -EINVAL;
            goto out;
        }

        /* Validate sync ranges: hsync_start <= hsync_end <= htotal */
        if (crtc_req->mode.hsync_start > crtc_req->mode.hsync_end || crtc_req->mode.hsync_end > crtc_req->mode.htotal) {
            plogk("drm_crtc: Invalid hsync range.\n");
            ret = -EINVAL;
            goto out;
        }

        /* Validate sync ranges: vsync_start <= vsync_end <= vtotal */
        if (crtc_req->mode.vsync_start > crtc_req->mode.vsync_end || crtc_req->mode.vsync_end > crtc_req->mode.vtotal) {
            plogk("drm_crtc: Invalid vsync range.\n");
            ret = -EINVAL;
            goto out;
        }

        /* Validate htotal/vtotal are non-zero */
        if (crtc_req->mode.htotal == 0 || crtc_req->mode.vtotal == 0) {
            plogk("drm_crtc: Invalid htotal/vtotal.\n");
            ret = -EINVAL;
            goto out;
        }

        /* Validate dimensions against mode_config limits */
        if (crtc_req->mode.hdisplay > dev->mode_config.max_width || crtc_req->mode.vdisplay > dev->mode_config.max_height) {
            plogk("drm_crtc: Mode exceeds limits (%ux%u)\n", crtc_req->mode.hdisplay, crtc_req->mode.vdisplay);
            ret = -EINVAL;
            goto out;
        }
        if (crtc_req->x > DRM_S32_MAX || crtc_req->y > DRM_S32_MAX || (uint64_t)crtc_req->x + crtc_req->mode.hdisplay > DRM_S32_MAX
            || (uint64_t)crtc_req->y + crtc_req->mode.vdisplay > DRM_S32_MAX) {
            plogk("drm_crtc: Invalid crtc position.\n");
            ret = -EINVAL;
            goto out;
        }

        if (crtc_req->count_connectors > (uint32_t)dev->mode_config.num_connector
            || (crtc_req->count_connectors && !crtc_req->set_connectors_ptr)) {
            plogk("drm_crtc: Invalid connector count %u\n", crtc_req->count_connectors);
            ret = -EINVAL;
            goto out;
        }
        if (crtc_req->count_connectors) {
            connector_ids = malloc((size_t)crtc_req->count_connectors * sizeof(*connector_ids));
            if (!connector_ids) {
                plogk("drm_crtc: Failed to allocate connector id array (%u entries)\n", crtc_req->count_connectors);
                ret = -ENOMEM;
                goto out;
            }
            if (copy_from_user(connector_ids, (const void *)(uintptr_t)crtc_req->set_connectors_ptr,
                               (size_t)crtc_req->count_connectors * sizeof(*connector_ids))) {
                plogk("drm_crtc: Failed to copy connector ids from user.\n");
                ret = -EFAULT;
                goto out;
            }
        }

        /* Convert UAPI modeinfo to internal display mode */
        memset(&mode, 0, sizeof(mode));
        mode.clock       = (int)crtc_req->mode.clock;
        mode.hdisplay    = (int)crtc_req->mode.hdisplay;
        mode.hsync_start = (int)crtc_req->mode.hsync_start;
        mode.hsync_end   = (int)crtc_req->mode.hsync_end;
        mode.htotal      = (int)crtc_req->mode.htotal;
        mode.hskew       = (int)crtc_req->mode.hskew;
        mode.vdisplay    = (int)crtc_req->mode.vdisplay;
        mode.vsync_start = (int)crtc_req->mode.vsync_start;
        mode.vsync_end   = (int)crtc_req->mode.vsync_end;
        mode.vtotal      = (int)crtc_req->mode.vtotal;
        mode.vscan       = (int)crtc_req->mode.vscan;
        mode.vrefresh    = (int)crtc_req->mode.vrefresh;
        mode.flags       = crtc_req->mode.flags;
        mode.type        = crtc_req->mode.type;
        mode.status      = MODE_OK;
        strncpy(mode.name, crtc_req->mode.name, DRM_DISPLAY_MODE_LEN - 1);
    } else if (crtc_req->count_connectors) {
        plogk("drm_crtc: Connectors specified without mode.\n");
        ret = -EINVAL;
        goto out;
    }

    state = drm_atomic_state_alloc(dev);
    if (!state) {
        ret = -ENOMEM;
        goto out;
    }
    state->allow_modeset = 1;
    state->file_priv     = file_priv;
    crtc_state           = drm_atomic_get_crtc_state(state, crtc);
    if (!crtc_state) {
        ret = -ENOMEM;
        goto out;
    }
    crtc_state->active         = crtc_req->mode_valid;
    crtc_state->enable         = crtc_req->mode_valid;
    crtc_state->active_changed = crtc_state->active != crtc->enabled;
    crtc_state->mode_changed   = true;
    if (crtc_req->mode_valid) crtc_state->mode = mode;

    if (crtc->primary) {
        plane_state = drm_atomic_get_plane_state(state, crtc->primary);
        if (!plane_state) {
            ret = -ENOMEM;
            goto out;
        }
        plane_state->crtc = crtc_req->mode_valid ? crtc : NULL;
        plane_state->fb   = crtc_req->mode_valid ? fb : NULL;
        plane_state->src  = (struct drm_rect) {0, 0, crtc_req->mode_valid ? (int32_t)(fb->width << 16) : 0,
                                              crtc_req->mode_valid ? (int32_t)(fb->height << 16) : 0};
        plane_state->dst
            = (struct drm_rect) {(int32_t)crtc_req->x, (int32_t)crtc_req->y, crtc_req->mode_valid ? (int32_t)(crtc_req->x + mode.hdisplay) : 0,
                                 crtc_req->mode_valid ? (int32_t)(crtc_req->y + mode.vdisplay) : 0};
        crtc_state->planes_changed = true;
    }

    for (uint32_t i = 0; i < crtc_req->count_connectors; i++) {
        struct drm_mode_object *conn_obj;
        for (uint32_t j = 0; j < i; j++)
            if (connector_ids[i] == connector_ids[j]) {
                plogk("drm_crtc: Duplicate connector id %u\n", connector_ids[i]);
                ret = -EINVAL;
                goto out;
            }
        conn_obj = drm_mode_object_find(dev, file_priv, connector_ids[i], DRM_MODE_OBJECT_CONNECTOR);
        if (!conn_obj) {
            plogk("drm_crtc: Connector %u not found.\n", connector_ids[i]);
            ret = -ENOENT;
            goto out;
        }
        drm_mode_object_put(conn_obj);
    }
    for (ilist_node_t *node = dev->mode_config.connector_list.next; node != &dev->mode_config.connector_list; node = node->next) {
        struct drm_connector       *connector = container_of(node, struct drm_connector, head);
        struct drm_connector_state *conn_state;
        bool                        selected = false;
        for (uint32_t i = 0; i < crtc_req->count_connectors; i++)
            if (connector_ids[i] == connector->base.id) {
                selected = true;
                break;
            }
        if (!selected && (!connector->state || connector->state->crtc != crtc)) continue;
        conn_state = drm_atomic_get_connector_state(state, connector);
        if (!conn_state) {
            ret = -ENOMEM;
            goto out;
        }
        conn_state->crtc               = selected ? crtc : NULL;
        conn_state->crtc_changed       = true;
        crtc_state->connectors_changed = true;
    }

    ret = drm_atomic_commit(state);
    if (!ret) state = NULL;
out:
    if (state) drm_atomic_state_free(state);
    free(connector_ids);
    drm_mode_object_put(obj);
    return ret;
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

    if (!crtc) return;

    dev = crtc->dev;

    ilist_remove(&crtc->head);

    if (dev) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, crtc->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_crtc > 0) dev->mode_config.num_crtc--;
    }

    free(crtc->gamma_store);
    crtc->gamma_store = NULL;
    if (crtc->cursor_obj) {
        drm_gem_object_put(crtc->cursor_obj);
        crtc->cursor_obj = NULL;
    }
    if (crtc->base.properties) {
        drm_property_set_destroy(crtc->base.properties);
        free(crtc->base.properties);
        crtc->base.properties = NULL;
    }
}
