/*
 *
 *      drm_mode_object.c
 *      DRM mode object lifecycle and ID management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_idr.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>

/* External helper from drm_property.c */

/* Initial backing-array capacity for a freshly attached property set. */
#define DRM_OBJECT_PROP_INITIAL_CAPACITY 16u

/* ID allocation and reference counting */

/*
 * Allocate a new mode-object ID from the device IDR and initialise the
 * common mode object header. The object is published in the global IDR
 * with an initial reference count of one. Returns 0 on success (with
 * obj->id set) or a negative errno on failure.
 *
 * Internal helper shared with drm_property.c; not declared in any header.
 */
int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type)
{
    uint32_t id = 0;
    int      ret;

    spin_lock(&dev->mode_config.idr_mutex);
    ret = drm_idr_alloc(&dev->mode_config.object_idr, obj, 1, 0, &id);
    spin_unlock(&dev->mode_config.idr_mutex);
    if (ret) {
        plogk("drm: Mode object IDR allocation failed (dev=%p, type=%u, ret=%d)\n", dev, type, ret);
        return ret;
    }

    obj->id       = id;
    obj->type     = type;
    obj->dev      = dev;
    obj->refcount = 1;
    memset(&obj->ref_lock, 0, sizeof(obj->ref_lock));
    obj->properties = NULL;
    return 0;
}

/* Acquire a reference on a mode object. */
void drm_mode_object_get(struct drm_mode_object *obj)
{
    if (!obj) return;
    spin_lock(&obj->ref_lock);
    obj->refcount++;
    spin_unlock(&obj->ref_lock);
}

/*
 * Decrement the mode-object reference count under the ref-lock and report
 * whether it reached zero. Making the "last reference" decision atomically
 * with the decrement avoids the lost-wakeup / double-free race that a
 * separate post-put check would introduce. Used by drm_mode_object_put()
 * and by blob destruction in drm_property.c.
 */
bool drm_mode_object_put_dec_and_test(struct drm_mode_object *obj)
{
    bool zero;

    if (!obj) return false;
    spin_lock(&obj->ref_lock);
    zero = (--obj->refcount == 0);
    spin_unlock(&obj->ref_lock);
    return zero;
}

/* Release a reference on a mode object; the caller owns finalisation. */
void drm_mode_object_put(struct drm_mode_object *obj)
{
    (void)drm_mode_object_put_dec_and_test(obj);
}

/*
 * Look up a mode object by userspace ID. If @type is DRM_MODE_OBJECT_ANY
 * the type check is skipped. When @file_priv is non-NULL the per-file
 * handle IDR is consulted as a fallback for objects not present in the
 * global IDR. Returns the object with an extra reference, or NULL.
 */
struct drm_mode_object *drm_mode_object_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id, uint32_t type)
{
    struct drm_mode_object *obj;

    spin_lock(&dev->mode_config.idr_mutex);
    obj = drm_idr_find(&dev->mode_config.object_idr, id);
    if (obj && (type == DRM_MODE_OBJECT_ANY || obj->type == type)) {
        drm_mode_object_get(obj);
        spin_unlock(&dev->mode_config.idr_mutex);
        return obj;
    }
    spin_unlock(&dev->mode_config.idr_mutex);

    if (file_priv) {
        spin_lock(&file_priv->table_lock);
        obj = drm_idr_find(&file_priv->object_idr, id);
        if (obj && (type == DRM_MODE_OBJECT_ANY || obj->type == type)) {
            drm_mode_object_get(obj);
            spin_unlock(&file_priv->table_lock);
            return obj;
        }
        spin_unlock(&file_priv->table_lock);
    }
    return NULL;
}

/* Per-object property storage */

/*
 * Store or update @property's value on @obj. If the property is already
 * present its value is replaced; otherwise a new slot is appended,
 * growing the backing arrays (doubling capacity) when full.
 * Returns 0 on success or -EINVAL / -ENOMEM.
 */
int drm_object_property_set_value(struct drm_mode_object *obj, struct drm_property *property, uint64_t val)
{
    struct drm_property_set *set;
    uint32_t                 i;

    if (!obj || !property) {
        plogk("drm: Property set value with invalid args (obj=%p, property=%p), returning -EINVAL.\n", obj, property);
        return -EINVAL;
    }
    set = obj->properties;
    if (!set) {
        plogk("drm: Object %p has no property set; cannot set value, returning -EINVAL.\n", obj);
        return -EINVAL;
    }

    spin_lock(&set->lock);
    for (i = 0; i < set->count; i++) {
        if (set->ids[i] == property->base.id) {
            set->values[i] = val;
            spin_unlock(&set->lock);
            return 0;
        }
    }

    if (set->count >= set->capacity) {
        uint32_t  new_cap = set->capacity ? set->capacity * 2u : DRM_OBJECT_PROP_INITIAL_CAPACITY;
        uint32_t *new_ids = realloc(set->ids, (size_t)new_cap * sizeof(*new_ids));
        uint64_t *new_vals;

        if (!new_ids) {
            spin_unlock(&set->lock);
            plogk("drm: Property set grow failed (realloc ids) for object %p, returning -ENOMEM.\n", obj);
            return -ENOMEM;
        }
        set->ids = new_ids; // realloc() may have freed the old buffer
        new_vals = realloc(set->values, (size_t)new_cap * sizeof(*new_vals));
        if (!new_vals) {
            /*
             * ids grew but values did not; leave `capacity` unchanged so
             * indexing stays bounded by values' real size. The extra ids
             * headroom is harmless and will be reused on the next grow.
             */
            spin_unlock(&set->lock);
            plogk("drm: Property set grow failed (realloc values) for object %p, returning -ENOMEM.\n", obj);
            return -ENOMEM;
        }
        set->values   = new_vals;
        set->capacity = new_cap;
    }

    set->ids[set->count]    = property->base.id;
    set->values[set->count] = val;
    set->count++;
    spin_unlock(&set->lock);
    return 0;
}

/*
 * Read @property's stored value on @obj into *@val_out.
 * Returns 0 on success or -EINVAL when the property is not attached.
 */
int drm_object_property_get_value(struct drm_mode_object *obj, struct drm_property *property, uint64_t *val_out)
{
    struct drm_property_set *set;
    uint32_t                 i;

    if (!obj || !property || !val_out) {
        plogk("drm: Property get value with invalid args (obj=%p, property=%p, val_out=%p), returning -EINVAL.\n", obj, property, val_out);
        return -EINVAL;
    }
    set = obj->properties;
    if (!set) {
        plogk("drm: Object %p has no property set; cannot get value, returning -EINVAL.\n", obj);
        return -EINVAL;
    }

    spin_lock(&set->lock);
    for (i = 0; i < set->count; i++) {
        if (set->ids[i] == property->base.id) {
            *val_out = set->values[i];
            spin_unlock(&set->lock);
            return 0;
        }
    }
    spin_unlock(&set->lock);
    plogk("drm: Property %p not attached to object %p, returning -EINVAL.\n", property, obj);
    return -EINVAL;
}

/*
 * Attach @property to @obj with an initial value, allocating the per-object
 * property set on first use with DRM_OBJECT_PROP_INITIAL_CAPACITY slots.
 * Must be called before @obj becomes visible to concurrent lookups (i.e.
 * during object construction).
 * Returns 0 on success or -ENOMEM.
 */
int drm_object_attach_property(struct drm_mode_object *obj, struct drm_property *property, uint64_t init_val)
{
    if (!obj || !property) {
        plogk("drm: Attach property with invalid args (obj=%p, property=%p), returning -EINVAL.\n", obj, property);
        return -EINVAL;
    }

    if (!obj->properties) {
        struct drm_property_set *set;
        uint32_t                *ids;
        uint64_t                *vals;

        set = malloc(sizeof(*set));
        if (!set) {
            plogk("drm: Property set allocation failed for object %p, returning -ENOMEM.\n", obj);
            return -ENOMEM;
        }
        ids = malloc((size_t)DRM_OBJECT_PROP_INITIAL_CAPACITY * sizeof(*ids));
        if (!ids) {
            free(set);
            plogk("drm: Property set ids allocation failed for object %p, returning -ENOMEM.\n", obj);
            return -ENOMEM;
        }
        vals = malloc((size_t)DRM_OBJECT_PROP_INITIAL_CAPACITY * sizeof(*vals));
        if (!vals) {
            free(ids);
            free(set);
            plogk("drm: Property set values allocation failed for object %p, returning -ENOMEM.\n", obj);
            return -ENOMEM;
        }
        memset(set, 0, sizeof(*set));
        set->count      = 0;
        set->capacity   = DRM_OBJECT_PROP_INITIAL_CAPACITY;
        set->ids        = ids;
        set->values     = vals;
        obj->properties = set;
    }

    return drm_object_property_set_value(obj, property, init_val);
}

/*
 * drm_mode_obj_getproperties_ioctl - Handle DRM_IOCTL_MODE_OBJ_GETPROPERTIES.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_obj_get_properties (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up a mode object by ID and returns its attached property IDs and values.
 */
int drm_mode_obj_getproperties_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_obj_get_properties *req = (struct drm_mode_obj_get_properties *)data;
    struct drm_mode_object             *obj;

    if (!dev || !req) {
        plogk("drm: OBJ_GETPROPERTIES with invalid args (dev=%p, req=%p)\n", dev, req);
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, file_priv, req->obj_id, req->obj_type);
    if (!obj) {
        plogk("drm: OBJ_GETPROPERTIES: object %u (type %u) not found, returning -ENOENT.\n", req->obj_id, req->obj_type);
        return -ENOENT;
    }

    if (obj->properties) {
        struct drm_property_set *set        = obj->properties;
        uint32_t                 user_count = req->count_props;
        uint32_t                 copy_count;
        uint32_t                *ids    = NULL;
        uint64_t                *values = NULL;

        spin_lock(&set->lock);
        req->count_props = set->count;
        copy_count       = user_count < set->count ? user_count : set->count;
        if (copy_count) {
            ids    = malloc((size_t)copy_count * sizeof(*ids));
            values = malloc((size_t)copy_count * sizeof(*values));
            if (ids && values) {
                memcpy(ids, set->ids, (size_t)copy_count * sizeof(*ids));
                memcpy(values, set->values, (size_t)copy_count * sizeof(*values));
            }
        }
        spin_unlock(&set->lock);
        if (copy_count && (!ids || !values)) {
            free(ids);
            free(values);
            drm_mode_object_put(obj);
            plogk("drm: OBJ_GETPROPERTIES: copy buffer allocation failed (count=%u), returning -ENOMEM.\n", copy_count);
            return -ENOMEM;
        }
        /* Report DPMS state-aware: self-refresh forces ON (Linux drm_atomic_connector_get_property). */
        if (obj->type == DRM_MODE_OBJECT_CONNECTOR && dev->mode_config.prop_dpms && copy_count) {
            uint32_t dpms_id = dev->mode_config.prop_dpms->base.id;
            for (uint32_t k = 0; k < copy_count; k++) {
                if (ids[k] == dpms_id) {
                    values[k] = (uint64_t)drm_connector_dpms_get(container_of(obj, struct drm_connector, base));
                    break;
                }
            }
        }
        if (copy_count
            && (!req->props_ptr || !req->prop_values_ptr || copy_to_user((void *)(uintptr_t)req->props_ptr, ids, (size_t)copy_count * sizeof(*ids))
                || copy_to_user((void *)(uintptr_t)req->prop_values_ptr, values, (size_t)copy_count * sizeof(*values)))) {
            free(ids);
            free(values);
            drm_mode_object_put(obj);
            plogk("drm: OBJ_GETPROPERTIES: copy_to_user failed (count=%u), returning -EFAULT.\n", copy_count);
            return -EFAULT;
        }
        free(ids);
        free(values);
    } else {
        req->count_props = 0;
    }

    drm_mode_object_put(obj);
    return 0;
}

/*
 * drm_mode_obj_setproperty_ioctl - Handle DRM_IOCTL_MODE_OBJ_SETPROPERTY.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_obj_set_property (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up a mode object and property, then sets the property value on the object.
 */
int drm_mode_obj_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_obj_set_property *req = (struct drm_mode_obj_set_property *)data;
    struct drm_mode_object           *obj;
    struct drm_property              *prop;

    (void)file_priv;

    if (!dev || !req) {
        plogk("drm: OBJ_SETPROPERTY with invalid args (dev=%p, req=%p)\n", dev, req);
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, NULL, req->obj_id, req->obj_type);
    if (!obj) {
        plogk("drm: OBJ_SETPROPERTY: object %u (type %u) not found, returning -ENOENT.\n", req->obj_id, req->obj_type);
        return -ENOENT;
    }

    prop = drm_property_find(dev, NULL, req->prop_id);
    if (!prop) {
        drm_mode_object_put(obj);
        plogk("drm: OBJ_SETPROPERTY: property %u not found, returning -ENOENT.\n", req->prop_id);
        return -ENOENT;
    }

    /* Atomic properties must be changed transactionally through MODE_ATOMIC. */
    if ((prop->flags & DRM_MODE_PROP_ATOMIC) || !obj->properties) {
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        plogk("drm: OBJ_SETPROPERTY: object %u is atomic or has no properties, returning -EINVAL.\n", req->obj_id);
        return -EINVAL;
    }
    {
        uint64_t ignored;
        if (drm_object_property_get_value(obj, prop, &ignored)) {
            drm_mode_object_put(&prop->base);
            drm_mode_object_put(obj);
            return -ENOENT;
        }
    }
    if (prop->flags & DRM_MODE_PROP_IMMUTABLE) {
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        plogk("drm: OBJ_SETPROPERTY: property %u is immutable, returning -EINVAL.\n", req->prop_id);
        return -EINVAL;
    }
    if ((prop->flags & DRM_MODE_PROP_RANGE) && (req->value < prop->values[0] || req->value > prop->values[1])) {
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        plogk("drm: OBJ_SETPROPERTY: value %llu out of range for property %u, returning -EINVAL.\n", (unsigned long long)req->value, req->prop_id);
        return -EINVAL;
    }
    /* DPMS is handled by the DPMS core (atomic commit or legacy dpms hook), never stored blindly. */
    if (obj->type == DRM_MODE_OBJECT_CONNECTOR && prop == dev->mode_config.prop_dpms) {
        struct drm_connector *connector = container_of(obj, struct drm_connector, base);
        int                   ret       = drm_connector_set_dpms(connector, (int)req->value);
        if (ret) {
            drm_mode_object_put(&prop->base);
            drm_mode_object_put(obj);
            return ret;
        }
        ret = drm_object_property_set_value(obj, prop, req->value);
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        return ret;
    }
    {
        int ret = drm_object_property_set_value(obj, prop, req->value);
        if (ret) {
            drm_mode_object_put(&prop->base);
            drm_mode_object_put(obj);
            return ret;
        }
    }

    drm_mode_object_put(&prop->base);
    drm_mode_object_put(obj);
    return 0;
}

/* Release backing storage of a property set and zero the struct. */
void drm_property_set_destroy(struct drm_property_set *set)
{
    if (!set) return;
    free(set->ids);
    free(set->values);
    memset(set, 0, sizeof(*set));
}
