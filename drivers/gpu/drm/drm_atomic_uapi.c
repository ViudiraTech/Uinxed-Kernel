/*
 *
 *      drm_atomic_uapi.c
 *      DRM atomic UAPI entry points
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm_device.h>
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

/* ------------------------------------------------------------------ */
/* Helper: container_of                                                */
/* ------------------------------------------------------------------ */

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* ------------------------------------------------------------------ */
/* Cross-file forward declarations                                     */
/* ------------------------------------------------------------------ */

extern struct drm_atomic_state    *drm_atomic_state_alloc(struct drm_device *dev);
extern struct drm_crtc_state      *drm_atomic_get_crtc_state(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern struct drm_plane_state     *drm_atomic_get_plane_state(struct drm_atomic_state *state, struct drm_plane *plane);
extern struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state, struct drm_connector *connector);
extern int                         drm_atomic_check_only(struct drm_atomic_state *state);
extern int                         drm_atomic_commit(struct drm_atomic_state *state);
extern void                        drm_atomic_state_free(struct drm_atomic_state *state);
extern struct drm_mode_object     *drm_mode_object_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id, uint32_t type);
extern struct drm_framebuffer     *drm_framebuffer_lookup(struct drm_device *dev, struct drm_file *file_priv, uint32_t id);
extern void                        drm_crtc_arm_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e);
extern void                        drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e);
extern void                        drm_handle_vblank(struct drm_device *dev, unsigned int pipe);

/* ------------------------------------------------------------------ */
/* drm_mode_atomic_ioctl: handle DRM_IOCTL_MODE_ATOMIC                  */
/* ------------------------------------------------------------------ */

int drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_atomic  *atomic = (struct drm_mode_atomic *)data;
    struct drm_atomic_state *state;
    uint32_t                *objs        = NULL;
    uint32_t                *count_props = NULL;
    uint32_t                *props       = NULL;
    uint64_t                *prop_values = NULL;
    int                      ret         = 0;
    uint32_t                 i;

    if (!atomic->count_objs || atomic->count_objs > 256) { return -EINVAL; }

    /* Allocate atomic state */
    state = drm_atomic_state_alloc(dev);
    if (!state) { return -ENOMEM; }

    if (atomic->flags & DRM_MODE_ATOMIC_ALLOW_MODESET) { state->allow_modeset = 1; }

    /* For MVP, we trust the inline data pointers are valid kernel addresses
     * (UAPI pointers are opaque uint64_t in this kernel).
     */
    objs        = (uint32_t *)(uintptr_t)atomic->objs_ptr;
    count_props = (uint32_t *)(uintptr_t)atomic->count_props_ptr;
    props       = (uint32_t *)(uintptr_t)atomic->props_ptr;
    prop_values = (uint64_t *)(uintptr_t)atomic->prop_values_ptr;

    if (!objs || !count_props || !props || !prop_values) {
        drm_atomic_state_free(state);
        return -EFAULT;
    }

    /* Walk objects and gather their states */
    {
        uint32_t prop_offset = 0;

        for (i = 0; i < atomic->count_objs; i++) {
            uint32_t obj_id    = objs[i];
            uint32_t obj_type  = obj_id & 0xFF000000U;
            uint32_t obj_count = count_props[i];
            uint32_t j;

            /* Get the state for each object type */
            if (obj_type == DRM_MODE_OBJECT_CRTC) {
                struct drm_crtc       *crtc;
                struct drm_crtc_state *crtc_state;

                crtc = (struct drm_crtc *)drm_mode_object_find(dev, file_priv, obj_id, DRM_MODE_OBJECT_CRTC);
                if (!crtc) {
                    DRM_ERROR("Atomic: CRTC %u not found\n", obj_id);
                    ret = -ENOENT;
                    break;
                }

                crtc_state = drm_atomic_get_crtc_state(state, crtc);
                if (!crtc_state) {
                    ret = -ENOMEM;
                    break;
                }

                /* Set properties */
                for (j = 0; j < obj_count; j++) {
                    uint32_t prop_id  = props[prop_offset + j];
                    uint64_t prop_val = prop_values[prop_offset + j];

                    /* ACTIVE property */
                    if (prop_id == 0) {
                        /* Simplified: assume first prop is ACTIVE */
                        crtc_state->active         = (prop_val != 0);
                        crtc_state->active_changed = true;
                    }
                    /* MODE_ID property */
                    else if (prop_id == 1) {
                        /* Simplified: mode blob reference */
                        crtc_state->mode_changed = true;
                    }
                }
                prop_offset += obj_count;
            } else if (obj_type == DRM_MODE_OBJECT_PLANE) {
                struct drm_plane       *plane;
                struct drm_plane_state *plane_state;

                plane = (struct drm_plane *)drm_mode_object_find(dev, file_priv, obj_id, DRM_MODE_OBJECT_PLANE);
                if (!plane) {
                    DRM_ERROR("Atomic: Plane %u not found\n", obj_id);
                    ret = -ENOENT;
                    break;
                }

                plane_state = drm_atomic_get_plane_state(state, plane);
                if (!plane_state) {
                    ret = -ENOMEM;
                    break;
                }

                /* Set properties */
                for (j = 0; j < obj_count; j++) {
                    uint32_t prop_id  = props[prop_offset + j];
                    uint64_t prop_val = prop_values[prop_offset + j];

                    /* FB_ID */
                    if (prop_id == 0) {
                        uint32_t fb_id = (uint32_t)prop_val;
                        if (fb_id != 0) {
                            struct drm_framebuffer *fb;

                            fb = (struct drm_framebuffer *)drm_mode_object_find(dev, file_priv, fb_id, DRM_MODE_OBJECT_FB);
                            if (fb) { plane_state->fb = fb; }
                        } else {
                            plane_state->fb = NULL;
                        }
                    }
                    /* CRTC_ID */
                    else if (prop_id == 1) {
                        uint32_t crtc_id = (uint32_t)prop_val;
                        if (crtc_id != 0) {
                            struct drm_crtc *crtc;

                            crtc = (struct drm_crtc *)drm_mode_object_find(dev, file_priv, crtc_id, DRM_MODE_OBJECT_CRTC);
                            if (crtc) { plane_state->crtc = crtc; }
                        } else {
                            plane_state->crtc = NULL;
                        }
                    }
                    /* SRC_X / SRC_Y / SRC_W / SRC_H */
                    else if (prop_id == 2) {
                        plane_state->src.x1 = (int32_t)prop_val;
                    } else if (prop_id == 3) {
                        plane_state->src.y1 = (int32_t)prop_val;
                    } else if (prop_id == 4) {
                        plane_state->src.x2 = plane_state->src.x1 + (int32_t)prop_val;
                    } else if (prop_id == 5) {
                        plane_state->src.y2 = plane_state->src.y1 + (int32_t)prop_val;
                    }
                    /* CRTC_X / CRTC_Y / CRTC_W / CRTC_H */
                    else if (prop_id == 6) {
                        plane_state->dst.x1 = (int32_t)prop_val;
                    } else if (prop_id == 7) {
                        plane_state->dst.y1 = (int32_t)prop_val;
                    } else if (prop_id == 8) {
                        plane_state->dst.x2 = plane_state->dst.x1 + (int32_t)prop_val;
                    } else if (prop_id == 9) {
                        plane_state->dst.y2 = plane_state->dst.y1 + (int32_t)prop_val;
                    }
                    /* ZPOS */
                    else if (prop_id == 10) {
                        plane_state->zpos         = (int)prop_val;
                        plane_state->zpos_changed = true;
                    }
                }
                prop_offset += obj_count;
            } else if (obj_type == DRM_MODE_OBJECT_CONNECTOR) {
                struct drm_connector       *connector;
                struct drm_connector_state *conn_state;

                connector = (struct drm_connector *)drm_mode_object_find(dev, file_priv, obj_id, DRM_MODE_OBJECT_CONNECTOR);
                if (!connector) {
                    DRM_ERROR("Atomic: Connector %u not found\n", obj_id);
                    ret = -ENOENT;
                    break;
                }

                conn_state = drm_atomic_get_connector_state(state, connector);
                if (!conn_state) {
                    ret = -ENOMEM;
                    break;
                }

                /* Set properties */
                for (j = 0; j < obj_count; j++) {
                    uint32_t prop_id  = props[prop_offset + j];
                    uint64_t prop_val = prop_values[prop_offset + j];

                    /* CRTC_ID */
                    if (prop_id == 0) {
                        uint32_t crtc_id = (uint32_t)prop_val;
                        if (crtc_id != 0) {
                            struct drm_crtc *crtc;

                            crtc = (struct drm_crtc *)drm_mode_object_find(dev, file_priv, crtc_id, DRM_MODE_OBJECT_CRTC);
                            if (crtc) {
                                conn_state->crtc         = crtc;
                                conn_state->crtc_changed = true;
                            }
                        } else {
                            conn_state->crtc         = NULL;
                            conn_state->crtc_changed = true;
                        }
                    }
                }
                prop_offset += obj_count;
            } else {
                DRM_ERROR("Atomic: unknown object type 0x%x for id %u\n", obj_type, obj_id);
                ret = -EINVAL;
                break;
            }
        }
    }

    if (ret < 0) {
        drm_atomic_state_free(state);
        return ret;
    }

    /* Test-only or commit */
    if (atomic->flags & DRM_MODE_ATOMIC_TEST_ONLY) {
        ret = drm_atomic_check_only(state);
        drm_atomic_state_free(state);
        return ret;
    }

    if (atomic->flags & DRM_MODE_ATOMIC_NONBLOCK) { return drm_atomic_commit(state); }

    return drm_atomic_commit(state);
}

/* ------------------------------------------------------------------ */
/* drm_mode_page_flip_ioctl: handle DRM_IOCTL_MODE_PAGE_FLIP            */
/* ------------------------------------------------------------------ */

int drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc_page_flip  *page_flip = (struct drm_mode_crtc_page_flip *)data;
    struct drm_crtc                 *crtc;
    struct drm_framebuffer          *fb;
    struct drm_pending_vblank_event *e;
    int                              ret = 0;

    if (!dev || !page_flip) { return -EINVAL; }

    crtc = (struct drm_crtc *)drm_mode_object_find(dev, file_priv, page_flip->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!crtc) {
        DRM_ERROR("Page flip: CRTC %u not found\n", page_flip->crtc_id);
        return -ENOENT;
    }

    fb = drm_framebuffer_lookup(dev, file_priv, page_flip->fb_id);
    if (!fb) {
        drm_mode_object_put(&crtc->base);
        DRM_ERROR("Page flip: FB %u not found\n", page_flip->fb_id);
        return -ENOENT;
    }

    /* Validate that the framebuffer dimensions match the current mode */
    if (crtc->enabled && crtc->mode.hdisplay > 0 && crtc->mode.vdisplay > 0) {
        if (fb->width != (unsigned int)crtc->mode.hdisplay || fb->height != (unsigned int)crtc->mode.vdisplay) {
            DRM_ERROR("Page flip: FB %ux%u does not match mode %ux%u\n", fb->width, fb->height, crtc->mode.hdisplay, crtc->mode.vdisplay);
            drm_mode_object_put(&crtc->base);
            return -EINVAL;
        }
    }

    /* If EVENT flag is set, queue a vblank event for delivery at next vblank */
    if (page_flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        e = malloc(sizeof(*e));
        if (!e) {
            drm_mode_object_put(&crtc->base);
            return -ENOMEM;
        }
        memset(e, 0, sizeof(*e));

        e->dev               = dev;
        e->pipe              = crtc->index;
        e->sequence          = 0; /* filled in by drm_handle_vblank */
        e->event.base.type   = 1; /* DRM_EVENT_VBLANK */
        e->event.base.length = sizeof(e->event);
        e->event.crtc_id     = crtc->base.id;
        e->event.seq         = 0;
        e->event.time        = 0;
        e->destroy           = NULL;
        e->next              = NULL;

        /* Arm the event for delivery at the target vblank sequence */
        if (page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) {
            /* Async: deliver immediately without waiting for vblank */
            drm_crtc_send_vblank_event(crtc, e);
        } else {
            /* Synchronous: arm for next vblank */
            drm_crtc_arm_vblank_event(crtc, e);
        }
    }

    /* Perform the actual flip: bind the new framebuffer to the primary plane */
    if (crtc->primary) {
        crtc->primary->fb_id = page_flip->fb_id;
        if (crtc->primary->state) { crtc->primary->state->fb = fb; }
    }

    /* If not async, trigger vblank processing immediately to deliver events */
    if (!(page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC)) { drm_handle_vblank(dev, crtc->index); }

    drm_mode_object_put(&crtc->base);

    DRM_DEBUG_KMS("Page flip: CRTC %u -> FB %u (flags=0x%x)\n", page_flip->crtc_id, page_flip->fb_id, page_flip->flags);

    return ret;
}

/* ------------------------------------------------------------------ */
/* drm_mode_cursor_ioctl: handle DRM_IOCTL_MODE_CURSOR                  */
/* ------------------------------------------------------------------ */

int drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_cursor *cursor = (struct drm_mode_cursor *)data;
    struct drm_crtc        *crtc;

    (void)file_priv;

    crtc = (struct drm_crtc *)drm_mode_object_find(dev, file_priv, cursor->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!crtc) {
        DRM_ERROR("Cursor: CRTC %u not found\n", cursor->crtc_id);
        return -ENOENT;
    }

    if (cursor->flags & DRM_MODE_CURSOR_MOVE) {
        crtc->x = cursor->x;
        crtc->y = cursor->y;
    }

    if (cursor->flags & DRM_MODE_CURSOR_BO) {
        /* BO-based cursor set: handle assignment */
        if (cursor->handle == 0) {
            /* Disable cursor */
            crtc->x = 0;
            crtc->y = 0;
        }
    }

    DRM_DEBUG_KMS("Cursor: CRTC %u move to (%d, %d)\n", cursor->crtc_id, cursor->x, cursor->y);

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_mode_cursor2_ioctl: handle DRM_IOCTL_MODE_CURSOR2               */
/* ------------------------------------------------------------------ */

int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_cursor2 *cursor2 = (struct drm_mode_cursor2 *)data;
    struct drm_crtc         *crtc;

    (void)file_priv;

    crtc = (struct drm_crtc *)drm_mode_object_find(dev, file_priv, cursor2->req.crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!crtc) {
        DRM_ERROR("Cursor2: CRTC %u not found\n", cursor2->req.crtc_id);
        return -ENOENT;
    }

    if (cursor2->req.flags & DRM_MODE_CURSOR_MOVE) {
        crtc->x = cursor2->req.x;
        crtc->y = cursor2->req.y;
    }

    if (cursor2->req.flags & DRM_MODE_CURSOR_BO) {
        if (cursor2->req.handle == 0) {
            crtc->x = 0;
            crtc->y = 0;
        }
    }

    DRM_DEBUG_KMS("Cursor2: CRTC %u move to (%d, %d) hot=(%d, %d)\n", cursor2->req.crtc_id, cursor2->req.x, cursor2->req.y, cursor2->hot_x,
                  cursor2->hot_y);

    return 0;
}