/*
 *
 *      drm_atomic_uapi.c
 *      DRM atomic UAPI entry points
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
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
#include <proc/uaccess.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* Helper: container_of                                                */
/* ------------------------------------------------------------------ */

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define DRM_S32_MAX                     ((int32_t)0x7fffffff)
#define DRM_S32_MIN                     (-DRM_S32_MAX - 1)

/*
 * MODE_ID is a blob property, but changing (or merely resubmitting) the
 * blob object does not necessarily change the hardware mode.  Atomic
 * modeset permission is based on the decoded timing, not the blob ID.
 */
static bool drm_atomic_modes_equal(const struct drm_display_mode *a, const struct drm_display_mode *b)
{
    if (!a || !b) return false;

    return a->clock == b->clock && a->hdisplay == b->hdisplay && a->hsync_start == b->hsync_start && a->hsync_end == b->hsync_end
           && a->htotal == b->htotal && a->hskew == b->hskew && a->vdisplay == b->vdisplay && a->vsync_start == b->vsync_start
           && a->vsync_end == b->vsync_end && a->vtotal == b->vtotal && a->vscan == b->vscan && a->flags == b->flags;
}

/* ------------------------------------------------------------------ */
/* Cross-file forward declarations                                     */
/* ------------------------------------------------------------------ */

static bool drm_atomic_object_has_property(struct drm_mode_object *obj, uint32_t property_id, uint64_t *current)
{
    struct drm_property_set *set   = obj ? obj->properties : NULL;
    bool                     found = false;

    if (!set) return false;
    spin_lock(&set->lock);
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->ids[i] == property_id) {
            if (current) *current = set->values[i];
            found = true;
            break;
        }
    }
    spin_unlock(&set->lock);
    return found;
}

static int drm_atomic_validate_property(struct drm_device *dev, struct drm_mode_object *obj, struct drm_property *prop, uint64_t value)
{
    uint64_t current;

    if (!drm_atomic_object_has_property(obj, prop->base.id, &current)) {
        plogk("drm_atomic: Object has no property %u\n", prop->base.id);
        return -ENOENT;
    }
    if ((prop->flags & DRM_MODE_PROP_IMMUTABLE) && current != value) {
        plogk("drm_atomic: Immutable property %u value 0x%llx rejected.\n", prop->base.id, (unsigned long long)value);
        return -EINVAL;
    }

    if (prop->flags & DRM_MODE_PROP_RANGE) {
        if (prop->num_values != 2 || value < prop->values[0] || value > prop->values[1]) {
            plogk("drm_atomic: Property %u value 0x%llx out of range.\n", prop->base.id, (unsigned long long)value);
            return -EINVAL;
        }
    } else if (prop->flags & DRM_MODE_PROP_SIGNED_RANGE) {
        int64_t signed_value = (int64_t)value;
        if (prop->num_values != 2 || signed_value < (int64_t)prop->values[0] || signed_value > (int64_t)prop->values[1]) {
            plogk("drm_atomic: Property %u signed value %lld out of range.\n", prop->base.id, (long long)signed_value);
            return -EINVAL;
        }
    } else if (prop->flags & DRM_MODE_PROP_ENUM) {
        ilist_node_t *node;
        bool          found = false;
        for (node = prop->enum_list.next; node != &prop->enum_list; node = node->next) {
            struct drm_property_enum *entry = container_of(node, struct drm_property_enum, head);
            if (entry->value == value) {
                found = true;
                break;
            }
        }
        if (!found) {
            plogk("drm_atomic: Property %u invalid enum value 0x%llx\n", prop->base.id, (unsigned long long)value);
            return -EINVAL;
        }
    } else if ((prop->flags & DRM_MODE_PROP_OBJECT) && value) {
        struct drm_mode_object *target;
        if (!prop->num_values || value > UINT32_MAX) {
            plogk("drm_atomic: Property %u invalid object value 0x%llx\n", prop->base.id, (unsigned long long)value);
            return -EINVAL;
        }
        target = drm_mode_object_find(dev, NULL, (uint32_t)value, (uint32_t)prop->values[0]);
        if (!target) {
            plogk("drm_atomic: Property %u object 0x%llx not found.\n", prop->base.id, (unsigned long long)value);
            return -ENOENT;
        }
        drm_mode_object_put(target);
    } else if ((prop->flags & DRM_MODE_PROP_BLOB) && value) {
        struct drm_property_blob *blob;
        if (value > UINT32_MAX) {
            plogk("drm_atomic: Property %u blob id 0x%llx out of range.\n", prop->base.id, (unsigned long long)value);
            return -EINVAL;
        }
        blob = drm_property_lookup_blob(dev, (uint32_t)value);
        if (!blob) {
            plogk("drm_atomic: Property %u blob 0x%llx not found.\n", prop->base.id, (unsigned long long)value);
            return -ENOENT;
        }
        drm_property_blob_put(blob);
    }
    return 0;
}

static int drm_atomic_set_uapi_property(struct drm_atomic_state *state, struct drm_file *file_priv, struct drm_mode_object *obj,
                                        struct drm_property *prop, uint64_t value)
{
    struct drm_mode_config *config = &state->dev->mode_config;

    if (obj->type == DRM_MODE_OBJECT_CRTC) {
        struct drm_crtc       *crtc = container_of(obj, struct drm_crtc, base);
        struct drm_crtc_state *s    = drm_atomic_get_crtc_state(state, crtc);
        if (!s) return -ENOMEM;
        if (prop == config->prop_active) {
            s->active         = !!value;
            s->enable         = !!value;
            s->active_changed = crtc->state ? s->active != crtc->state->active : true;
            return 0;
        }
        if (prop == config->prop_mode_id) {
            struct drm_display_mode new_mode;

            memset(&new_mode, 0, sizeof(new_mode));
            if (value) {
                struct drm_property_blob *blob = drm_property_lookup_blob(state->dev, (uint32_t)value);
                struct drm_display_mode  *mode;
                if (!blob) {
                    plogk("drm_atomic: Mode_id blob %u not found.\n", (uint32_t)value);
                    return -ENOENT;
                }
                if (blob->length != sizeof(struct drm_mode_modeinfo)) {
                    plogk("drm_atomic: Mode_id blob length %zu invalid.\n", blob->length);
                    drm_property_blob_put(blob);
                    return -EINVAL;
                }
                mode = drm_convert_umode((const struct drm_mode_modeinfo *)blob->data);
                drm_property_blob_put(blob);
                if (!mode) {
                    plogk("drm_atomic: Failed to convert mode blob.\n");
                    return -ENOMEM;
                }
                memcpy(&new_mode, mode, sizeof(new_mode));
                free(mode);
            }
            s->mode_changed = !drm_atomic_modes_equal(&s->mode, &new_mode);
            memcpy(&s->mode, &new_mode, sizeof(s->mode));
            return 0;
        }
    } else if (obj->type == DRM_MODE_OBJECT_PLANE) {
        struct drm_plane       *plane = container_of(obj, struct drm_plane, base);
        struct drm_plane_state *s     = drm_atomic_get_plane_state(state, plane);
        int32_t                 extent;
        if (!s) return -ENOMEM;
        if (prop == config->prop_fb_id) {
            s->fb = value ? drm_framebuffer_lookup(state->dev, file_priv, (uint32_t)value) : NULL;
            if (value && !s->fb) {
                plogk("drm_atomic: Plane fb %u not found.\n", (uint32_t)value);
                return -ENOENT;
            }
            if (s->crtc) {
                struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, s->crtc);
                if (!cs) return -ENOMEM;
                cs->planes_changed = true;
            }
            return 0;
        }
        if (prop == config->prop_crtc_id) {
            struct drm_crtc        *old_crtc = s->crtc;
            struct drm_mode_object *target   = value ? drm_mode_object_find(state->dev, file_priv, (uint32_t)value, DRM_MODE_OBJECT_CRTC) : NULL;
            if (value && !target) {
                plogk("drm_atomic: Plane crtc %u not found.\n", (uint32_t)value);
                return -ENOENT;
            }
            s->crtc = target ? container_of(target, struct drm_crtc, base) : NULL;
            if (target) drm_mode_object_put(target);
            if (old_crtc) {
                struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, old_crtc);
                if (!cs) return -ENOMEM;
                cs->planes_changed = true;
            }
            if (s->crtc && s->crtc != old_crtc) {
                struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, s->crtc);
                if (!cs) return -ENOMEM;
                cs->planes_changed = true;
            }
            return 0;
        }
        if (prop == config->prop_src_x) {
            extent = s->src.x2 - s->src.x1;
            if (value > DRM_S32_MAX || (int64_t)value + extent > DRM_S32_MAX) {
                plogk("drm_atomic: Prop src_x value 0x%llx out of range.\n", (unsigned long long)value);
                return -EINVAL;
            }
            s->src.x1 = (int32_t)value;
            s->src.x2 = s->src.x1 + extent;
            return 0;
        }
        if (prop == config->prop_src_y) {
            extent = s->src.y2 - s->src.y1;
            if (value > DRM_S32_MAX || (int64_t)value + extent > DRM_S32_MAX) {
                plogk("drm_atomic: Prop src_y value 0x%llx out of range.\n", (unsigned long long)value);
                return -EINVAL;
            }
            s->src.y1 = (int32_t)value;
            s->src.y2 = s->src.y1 + extent;
            return 0;
        }
        if (prop == config->prop_src_w) {
            if (value > DRM_S32_MAX || (int64_t)s->src.x1 + value > DRM_S32_MAX) {
                plogk("drm_atomic: Prop src_w value 0x%llx out of range.\n", (unsigned long long)value);
                return -EINVAL;
            }
            s->src.x2 = s->src.x1 + (int32_t)value;
            return 0;
        }
        if (prop == config->prop_src_h) {
            if (value > DRM_S32_MAX || (int64_t)s->src.y1 + value > DRM_S32_MAX) {
                plogk("drm_atomic: Prop src_h value 0x%llx out of range.\n", (unsigned long long)value);
                return -EINVAL;
            }
            s->src.y2 = s->src.y1 + (int32_t)value;
            return 0;
        }
        if (prop == config->prop_crtc_x) {
            int32_t v = (int32_t)value;
            extent    = s->dst.x2 - s->dst.x1;
            if ((int64_t)v + extent > DRM_S32_MAX || (int64_t)v + extent < DRM_S32_MIN) {
                plogk("drm_atomic: Prop crtc_x value %lld out of range.\n", (long long)v);
                return -EINVAL;
            }
            s->dst.x1 = v;
            s->dst.x2 = v + extent;
            return 0;
        }
        if (prop == config->prop_crtc_y) {
            int32_t v = (int32_t)value;
            extent    = s->dst.y2 - s->dst.y1;
            if ((int64_t)v + extent > DRM_S32_MAX || (int64_t)v + extent < DRM_S32_MIN) {
                plogk("drm_atomic: Prop crtc_y value %lld out of range.\n", (long long)v);
                return -EINVAL;
            }
            s->dst.y1 = v;
            s->dst.y2 = v + extent;
            return 0;
        }
        if (prop == config->prop_crtc_w) {
            if (value > DRM_S32_MAX || (int64_t)s->dst.x1 + value > DRM_S32_MAX) {
                plogk("drm_atomic: Prop crtc_w value 0x%llx out of range.\n", (unsigned long long)value);
                return -EINVAL;
            }
            s->dst.x2 = s->dst.x1 + (int32_t)value;
            return 0;
        }
        if (prop == config->prop_crtc_h) {
            if (value > DRM_S32_MAX || (int64_t)s->dst.y1 + value > DRM_S32_MAX) {
                plogk("drm_atomic: Prop crtc_h value 0x%llx out of range.\n", (unsigned long long)value);
                return -EINVAL;
            }
            s->dst.y2 = s->dst.y1 + (int32_t)value;
            return 0;
        }
        if (prop == config->prop_zpos) {
            s->zpos         = (int)value;
            s->zpos_changed = true;
            return 0;
        }
        if (prop == config->prop_alpha) {
            s->alpha = (unsigned int)value;
            return 0;
        }
        if (prop == config->prop_plane_type) return 0;
    } else if (obj->type == DRM_MODE_OBJECT_CONNECTOR) {
        struct drm_connector       *connector = container_of(obj, struct drm_connector, base);
        struct drm_connector_state *s         = drm_atomic_get_connector_state(state, connector);
        if (!s) return -ENOMEM;
        if (prop == config->prop_crtc_id) {
            struct drm_crtc        *old_crtc = s->crtc;
            struct drm_mode_object *target   = value ? drm_mode_object_find(state->dev, file_priv, (uint32_t)value, DRM_MODE_OBJECT_CRTC) : NULL;
            if (value && !target) {
                plogk("drm_atomic: Connector crtc %u not found.\n", (uint32_t)value);
                return -ENOENT;
            }
            s->crtc = target ? container_of(target, struct drm_crtc, base) : NULL;
            if (target) drm_mode_object_put(target);
            s->crtc_changed = true;
            if (old_crtc) {
                struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, old_crtc);
                if (!cs) return -ENOMEM;
                cs->connectors_changed = true;
            }
            if (s->crtc && s->crtc != old_crtc) {
                struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, s->crtc);
                if (!cs) return -ENOMEM;
                cs->connectors_changed = true;
            }
            return 0;
        }
    }
    plogk("drm_atomic: Unsupported property %u for object type %u\n", prop->base.id, obj->type);
    return -EINVAL;
}

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
    uint32_t                 total_props = 0;
    int                      ret         = 0;
    uint32_t                 i;

    if (!dev || !atomic || !file_priv || atomic->count_objs > 256 || atomic->reserved) {
        plogk("drm_atomic: Invalid atomic ioctl args.\n");
        return -EINVAL;
    }
    if (!(dev->driver->driver_features & DRIVER_ATOMIC)) {
        plogk("drm_atomic: Device does not support atomic modeset.\n");
        return -EOPNOTSUPP;
    }
    if (!file_priv->atomic) {
        plogk("drm_atomic: Client is not atomic-capable.\n");
        return -EINVAL;
    }
    if (atomic->flags & ~DRM_MODE_ATOMIC_FLAGS) {
        plogk("drm_atomic: Unsupported atomic flags 0x%x\n", atomic->flags);
        return -EINVAL;
    }
    if ((atomic->flags & DRM_MODE_ATOMIC_TEST_ONLY) && (atomic->flags & DRM_MODE_PAGE_FLIP_EVENT)) {
        plogk("drm_atomic: TEST_ONLY with page flip event invalid.\n");
        return -EINVAL;
    }
    if (atomic->flags & DRM_MODE_PAGE_FLIP_ASYNC) {
        plogk("drm_atomic: Async page flip not supported.\n");
        return -EINVAL;
    }

    /* Allocate atomic state */
    state = drm_atomic_state_alloc(dev);
    if (!state) return -ENOMEM;

    if (atomic->flags & DRM_MODE_ATOMIC_ALLOW_MODESET) state->allow_modeset = 1;
    state->file_priv       = file_priv;
    state->user_data       = atomic->user_data;
    state->page_flip_event = !!(atomic->flags & DRM_MODE_PAGE_FLIP_EVENT);

    if (atomic->count_objs) {
        if (!atomic->objs_ptr || !atomic->count_props_ptr) {
            plogk("drm_atomic: Missing objs/count_props pointers.\n");
            ret = -EFAULT;
            goto out;
        }
        objs        = malloc((size_t)atomic->count_objs * sizeof(*objs));
        count_props = malloc((size_t)atomic->count_objs * sizeof(*count_props));
        if (!objs || !count_props) {
            plogk("drm_atomic: Failed to allocate object arrays (%u objs)\n", atomic->count_objs);
            ret = -ENOMEM;
            goto out;
        }
        if (copy_from_user(objs, (const void *)(uintptr_t)atomic->objs_ptr, (size_t)atomic->count_objs * sizeof(*objs))
            || copy_from_user(count_props, (const void *)(uintptr_t)atomic->count_props_ptr,
                              (size_t)atomic->count_objs * sizeof(*count_props))) {
            plogk("drm_atomic: Failed to copy object arrays from user.\n");
            ret = -EFAULT;
            goto out;
        }
        for (i = 0; i < atomic->count_objs; i++) {
            if (count_props[i] > 4096 - total_props) {
                plogk("drm_atomic: Too many props for object %u (count=%u)\n", objs[i], count_props[i]);
                ret = -E2BIG;
                goto out;
            }
            total_props += count_props[i];
        }
    }
    if (total_props) {
        if (!atomic->props_ptr || !atomic->prop_values_ptr) {
            plogk("drm_atomic: Missing props/prop_values pointers.\n");
            ret = -EFAULT;
            goto out;
        }
        props       = malloc((size_t)total_props * sizeof(*props));
        prop_values = malloc((size_t)total_props * sizeof(*prop_values));
        if (!props || !prop_values) {
            plogk("drm_atomic: Failed to allocate prop arrays (%u props)\n", total_props);
            ret = -ENOMEM;
            goto out;
        }
        if (copy_from_user(props, (const void *)(uintptr_t)atomic->props_ptr, (size_t)total_props * sizeof(*props))
            || copy_from_user(prop_values, (const void *)(uintptr_t)atomic->prop_values_ptr, (size_t)total_props * sizeof(*prop_values))) {
            plogk("drm_atomic: Failed to copy prop arrays from user.\n");
            ret = -EFAULT;
            goto out;
        }
    }

    /* Walk objects and gather their states */
    {
        uint32_t prop_offset = 0;

        for (i = 0; i < atomic->count_objs; i++) {
            uint32_t                obj_id    = objs[i];
            uint32_t                obj_count = count_props[i];
            uint32_t                j;
            struct drm_mode_object *obj = drm_mode_object_find(dev, file_priv, obj_id, DRM_MODE_OBJECT_ANY);
            if (!obj) {
                plogk("drm_atomic: Object %u not found.\n", obj_id);
                ret = -ENOENT;
                break;
            }
            if (obj->type != DRM_MODE_OBJECT_CRTC && obj->type != DRM_MODE_OBJECT_PLANE && obj->type != DRM_MODE_OBJECT_CONNECTOR) {
                plogk("drm_atomic: Object %u has unsupported type %u\n", obj_id, obj->type);
                drm_mode_object_put(obj);
                ret = -EINVAL;
                break;
            }
            for (j = 0; props && j < obj_count; j++) {
                struct drm_property *prop = drm_property_find(dev, file_priv, props[prop_offset + j]);
                if (!prop) {
                    plogk("drm_atomic: Property %u not found.\n", props[prop_offset + j]);
                    ret = -ENOENT;
                    break;
                }
                ret = drm_atomic_validate_property(dev, obj, prop, prop_values[prop_offset + j]);
                if (!ret) ret = drm_atomic_set_uapi_property(state, file_priv, obj, prop, prop_values[prop_offset + j]);
                drm_mode_object_put(&prop->base);
                if (ret) break;
            }
            drm_mode_object_put(obj);
            prop_offset += obj_count;
            if (ret) break;
        }
    }

    if (ret < 0) goto out;

    /* Test-only or commit */
    if (atomic->flags & DRM_MODE_ATOMIC_TEST_ONLY) {
        ret = drm_atomic_check_only(state);
        drm_atomic_state_free(state);
        state = NULL;
        goto out;
    }
    ret = (atomic->flags & DRM_MODE_ATOMIC_NONBLOCK) ? drm_atomic_nonblocking_commit(state) : drm_atomic_commit(state);
    if (!ret) state = NULL;
    if (!ret) {
        uint32_t offset = 0;
        for (i = 0; i < atomic->count_objs; i++) {
            struct drm_mode_object *obj = drm_mode_object_find(dev, file_priv, objs[i], DRM_MODE_OBJECT_ANY);
            if (!obj) {
                offset += count_props[i];
                continue;
            }
            for (uint32_t j = 0; props && j < count_props[i]; j++) {
                struct drm_property *prop = drm_property_find(dev, file_priv, props[offset + j]);
                if (prop) {
                    drm_object_property_set_value(obj, prop, prop_values[offset + j]);
                    drm_mode_object_put(&prop->base);
                }
            }
            drm_mode_object_put(obj);
            offset += count_props[i];
        }
    }

out:
    if (state) drm_atomic_state_free(state);
    free(objs);
    free(count_props);
    free(props);
    free(prop_values);
    return ret;
}

/* ------------------------------------------------------------------ */
/* drm_mode_page_flip_ioctl: handle DRM_IOCTL_MODE_PAGE_FLIP            */
/* ------------------------------------------------------------------ */

int drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc_page_flip  *page_flip = (struct drm_mode_crtc_page_flip *)data;
    struct drm_crtc                 *crtc;
    struct drm_framebuffer          *fb;
    struct drm_pending_vblank_event *e   = NULL;
    int                              ret = 0;
    struct drm_mode_object          *crtc_obj;

    if (!dev || !page_flip) {
        plogk("drm_atomic: Page flip with invalid args.\n");
        return -EINVAL;
    }
    if (page_flip->flags & ~(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC)) {
        plogk("drm_atomic: Page flip invalid flags 0x%x\n", page_flip->flags);
        return -EINVAL;
    }

    crtc_obj = drm_mode_object_find(dev, file_priv, page_flip->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!crtc_obj) {
        DRM_ERROR("Page flip: CRTC %u not found\n", page_flip->crtc_id);
        return -ENOENT;
    }

    fb = drm_framebuffer_lookup(dev, file_priv, page_flip->fb_id);
    if (!fb) {
        drm_mode_object_put(crtc_obj);
        DRM_ERROR("Page flip: FB %u not found\n", page_flip->fb_id);
        return -ENOENT;
    }
    crtc = container_of(crtc_obj, struct drm_crtc, base);

    if (!crtc->enabled) {
        plogk("drm_atomic: Page flip on disabled crtc %u\n", page_flip->crtc_id);
        drm_mode_object_put(&crtc->base);
        return -EINVAL;
    }
    if ((page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) && !dev->mode_config.async_page_flip) {
        plogk("drm_atomic: Async page flip not supported on crtc %u\n", page_flip->crtc_id);
        drm_mode_object_put(&crtc->base);
        return -EINVAL;
    }

    /* Validate that the framebuffer dimensions match the current mode */
    if (crtc->enabled && crtc->mode.hdisplay > 0 && crtc->mode.vdisplay > 0) {
        if (fb->width != (unsigned int)crtc->mode.hdisplay || fb->height != (unsigned int)crtc->mode.vdisplay) {
            DRM_ERROR("Page flip: FB %ux%u does not match mode %ux%u\n", fb->width, fb->height, crtc->mode.hdisplay, crtc->mode.vdisplay);
            drm_mode_object_put(&crtc->base);
            return -EINVAL;
        }
    }

    spin_lock(&crtc->commit_lock);
    if (crtc->page_flip_pending) {
        spin_unlock(&crtc->commit_lock);
        plogk("drm_atomic: Page flip busy on crtc %u\n", page_flip->crtc_id);
        drm_mode_object_put(&crtc->base);
        return -EBUSY;
    }
    crtc->page_flip_pending = true;
    crtc->page_flip_target  = 0;
    spin_unlock(&crtc->commit_lock);

    /*
     * Build the event now, but do not arm it until the driver accepted the
     * flip.  This prevents a failed flip from leaking a completion event.
     */
    if (page_flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        e = malloc(sizeof(*e));
        if (!e) {
            plogk("drm_atomic: Failed to allocate flip event for crtc %u\n", page_flip->crtc_id);
            spin_lock(&crtc->commit_lock);
            crtc->page_flip_pending = false;
            spin_unlock(&crtc->commit_lock);
            drm_mode_object_put(&crtc->base);
            return -ENOMEM;
        }
        memset(e, 0, sizeof(*e));

        e->dev               = dev;
        e->file_priv         = file_priv;
        e->crtc              = crtc;
        e->pipe              = crtc->index;
        e->event.base.type   = DRM_EVENT_FLIP_COMPLETE;
        e->event.base.length = sizeof(e->event);
        e->event.user_data   = page_flip->user_data;
        e->event.crtc_id     = crtc->base.id;
        e->destroy           = NULL;
        e->next              = NULL;
    }

    bool synchronous_flip = dev->driver && (dev->driver->driver_features & DRIVER_SYNCHRONOUS_FLIP);
    if (!(page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) && !synchronous_flip) {
        ret = drm_crtc_vblank_get(crtc);
        if (ret) {
            plogk("drm_atomic: Failed to get vblank for crtc %u (ret=%d)\n", page_flip->crtc_id, ret);
            goto err_flip;
        }
        spin_lock(&crtc->commit_lock);
        crtc->page_flip_target = (uint64_t)drm_crtc_vblank_count(crtc) + 1;
        spin_unlock(&crtc->commit_lock);
        if (e) e->sequence = crtc->page_flip_target;
    }

    /*
     * Call the driver's page_flip hook FIRST to push the new FB to hardware.
     * Only update the plane state after the hardware flip succeeds.
     */
    if (crtc->helper_private) {
        struct drm_crtc_helper_funcs *h = (struct drm_crtc_helper_funcs *)crtc->helper_private;
        if (h->page_flip) {
            ret = h->page_flip(crtc, fb, e, page_flip->flags);
            if (ret) {
                plogk("drm_atomic: Crtc %u page_flip failed (ret=%d)\n", page_flip->crtc_id, ret);
                if (!(page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) && !synchronous_flip) drm_crtc_vblank_put(crtc);
                goto err_flip;
            }
        } else {
            plogk("drm_atomic: Crtc %u has no page_flip helper.\n", page_flip->crtc_id);
            ret = -ENOSYS;
            if (!(page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) && !synchronous_flip) drm_crtc_vblank_put(crtc);
            goto err_flip;
        }
    } else {
        plogk("drm_atomic: Crtc %u has no helper funcs.\n", page_flip->crtc_id);
        ret = -ENOSYS;
        if (!(page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) && !synchronous_flip) drm_crtc_vblank_put(crtc);
        goto err_flip;
    }

    if ((page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) || synchronous_flip) {
        spin_lock(&crtc->commit_lock);
        crtc->page_flip_pending = false;
        spin_unlock(&crtc->commit_lock);
        if (e) {
            e->sequence = drm_crtc_vblank_count(crtc);
            drm_crtc_send_vblank_event(crtc, e);
        }
    } else if (e) {
        drm_crtc_arm_vblank_event(crtc, e);
    }

    drm_mode_object_put(&crtc->base);

    return ret;

err_flip:
    spin_lock(&crtc->commit_lock);
    crtc->page_flip_pending = false;
    crtc->page_flip_target  = 0;
    spin_unlock(&crtc->commit_lock);
    if (e) free(e);
    drm_mode_object_put(&crtc->base);
    return ret ? ret : -EINVAL;
}

/* ------------------------------------------------------------------ */
/* drm_mode_cursor_ioctl: handle DRM_IOCTL_MODE_CURSOR                  */
/* ------------------------------------------------------------------ */

static int drm_mode_cursor_common(struct drm_device *dev, struct drm_file *file_priv, struct drm_mode_cursor *cursor, int32_t hot_x,
                                  int32_t hot_y)
{
    struct drm_mode_object       *base;
    struct drm_crtc              *crtc;
    struct drm_crtc_helper_funcs *helpers;
    struct drm_gem_object        *new_obj = NULL, *old_obj = NULL;
    int                           ret = 0;

    if (!dev || !file_priv || !cursor || !cursor->flags || (cursor->flags & ~(DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE))) {
        plogk("drm_atomic: Cursor with invalid args.\n");
        return -EINVAL;
    }
    base = drm_mode_object_find(dev, file_priv, cursor->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!base) {
        plogk("drm_atomic: Cursor crtc %u not found.\n", cursor->crtc_id);
        return -ENOENT;
    }
    crtc    = container_of(base, struct drm_crtc, base);
    helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;

    if (cursor->flags & DRM_MODE_CURSOR_BO) {
        if (!helpers || !helpers->cursor_set) {
            plogk("drm_atomic: Crtc %u has no cursor_set helper.\n", cursor->crtc_id);
            ret = -ENOSYS;
            goto out;
        }
        if (cursor->handle) {
            if (!cursor->width || !cursor->height || hot_x < 0 || hot_y < 0 || (uint32_t)hot_x >= cursor->width
                || (uint32_t)hot_y >= cursor->height) {
                plogk("drm_atomic: Invalid cursor size %ux%u hot %dx%d\n", cursor->width, cursor->height, hot_x, hot_y);
                ret = -EINVAL;
                goto out;
            }
            new_obj = drm_gem_object_lookup(file_priv, cursor->handle);
            if (!new_obj) {
                plogk("drm_atomic: Cursor gem handle %u not found.\n", cursor->handle);
                ret = -ENOENT;
                goto out;
            }
            if (new_obj->size < (size_t)cursor->width * cursor->height * 4) {
                plogk("drm_atomic: Cursor gem too small for %ux%u\n", cursor->width, cursor->height);
                ret = -EINVAL;
                goto out;
            }
        }
        ret = helpers->cursor_set(crtc, new_obj, cursor->width, cursor->height, hot_x, hot_y);
        if (ret) {
            plogk("drm_atomic: Crtc %u cursor_set failed (ret=%d)\n", cursor->crtc_id, ret);
            goto out;
        }
        spin_lock(&crtc->spinlock);
        old_obj            = crtc->cursor_obj;
        crtc->cursor_obj   = new_obj;
        crtc->cursor_hot_x = hot_x;
        crtc->cursor_hot_y = hot_y;
        new_obj            = NULL;
        spin_unlock(&crtc->spinlock);
        if (old_obj) drm_gem_object_put(old_obj);
    }
    if (cursor->flags & DRM_MODE_CURSOR_MOVE) {
        if (!helpers || !helpers->cursor_move) {
            plogk("drm_atomic: Crtc %u has no cursor_move helper.\n", cursor->crtc_id);
            ret = -ENOSYS;
            goto out;
        }
        ret = helpers->cursor_move(crtc, cursor->x, cursor->y);
        if (ret) {
            plogk("drm_atomic: Crtc %u cursor_move failed (ret=%d)\n", cursor->crtc_id, ret);
            goto out;
        }
        crtc->x = cursor->x;
        crtc->y = cursor->y;
    }

out:
    if (new_obj) drm_gem_object_put(new_obj);
    drm_mode_object_put(base);
    return ret;
}

int drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_cursor *cursor = (struct drm_mode_cursor *)data;
    return drm_mode_cursor_common(dev, file_priv, cursor, 0, 0);
}

/* ------------------------------------------------------------------ */
/* drm_mode_cursor2_ioctl: handle DRM_IOCTL_MODE_CURSOR2               */
/* ------------------------------------------------------------------ */

int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_cursor2 *cursor2 = (struct drm_mode_cursor2 *)data;
    return drm_mode_cursor_common(dev, file_priv, &cursor2->req, cursor2->hot_x, cursor2->hot_y);
}
