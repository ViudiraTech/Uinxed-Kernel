/*
 *
 *      drm_mode_object.c
 *      DRM mode object lifecycle and ID management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_device.h>
#include <drm/drm_idr.h>
#include <drm/drm_mode.h>
#include <alloc.h>
#include <errno.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Initial backing-array capacity for a freshly attached property set. */
#define DRM_OBJECT_PROP_INITIAL_CAPACITY 16u

/* ------------------------------------------------------------------ */
/* ID allocation and reference counting                               */
/* ------------------------------------------------------------------ */

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
    if (ret)
        return ret;

    obj->id         = id;
    obj->type       = type;
    obj->dev        = dev;
    obj->refcount   = 1;
    memset(&obj->ref_lock, 0, sizeof(obj->ref_lock));
    obj->properties = NULL;
    return 0;
}

/* Acquire a reference on a mode object. */
void drm_mode_object_get(struct drm_mode_object *obj)
{
    if (!obj)
        return;
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

    if (!obj)
        return false;
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
struct drm_mode_object *drm_mode_object_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id,
                                             uint32_t type)
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

/* ------------------------------------------------------------------ */
/* Per-object property storage                                        */
/* ------------------------------------------------------------------ */

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

    if (!obj || !property)
        return -EINVAL;
    set = obj->properties;
    if (!set)
        return -EINVAL;

    spin_lock(&set->lock);
    for (i = 0; i < set->count; i++) {
        if (set->ids[i] == property->base.id) {
            set->values[i] = val;
            spin_unlock(&set->lock);
            return 0;
        }
    }

    if (set->count >= set->capacity) {
        uint32_t  new_cap  = set->capacity ? set->capacity * 2u : DRM_OBJECT_PROP_INITIAL_CAPACITY;
        uint32_t *new_ids  = realloc(set->ids, (size_t)new_cap * sizeof(*new_ids));
        uint64_t *new_vals;

        if (!new_ids) {
            spin_unlock(&set->lock);
            return -ENOMEM;
        }
        set->ids = new_ids; /* realloc() may have freed the old buffer */
        new_vals = realloc(set->values, (size_t)new_cap * sizeof(*new_vals));
        if (!new_vals) {
            /* ids grew but values did not; leave `capacity` unchanged so
               indexing stays bounded by values' real size. The extra ids
               headroom is harmless and will be reused on the next grow. */
            spin_unlock(&set->lock);
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

    if (!obj || !property || !val_out)
        return -EINVAL;
    set = obj->properties;
    if (!set)
        return -EINVAL;

    spin_lock(&set->lock);
    for (i = 0; i < set->count; i++) {
        if (set->ids[i] == property->base.id) {
            *val_out = set->values[i];
            spin_unlock(&set->lock);
            return 0;
        }
    }
    spin_unlock(&set->lock);
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
    if (!obj || !property)
        return -EINVAL;

    if (!obj->properties) {
        struct drm_property_set *set;
        uint32_t                *ids;
        uint64_t                *vals;

        set = malloc(sizeof(*set));
        if (!set)
            return -ENOMEM;
        ids = malloc((size_t)DRM_OBJECT_PROP_INITIAL_CAPACITY * sizeof(*ids));
        if (!ids) {
            free(set);
            return -ENOMEM;
        }
        vals = malloc((size_t)DRM_OBJECT_PROP_INITIAL_CAPACITY * sizeof(*vals));
        if (!vals) {
            free(ids);
            free(set);
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

/* Initialise an empty property set (zero capacity, no backing storage). */
void drm_property_set_init(struct drm_property_set *set)
{
    if (!set)
        return;
    memset(set, 0, sizeof(*set));
}

/* Release backing storage of a property set and zero the struct. */
void drm_property_set_destroy(struct drm_property_set *set)
{
    if (!set)
        return;
    free(set->ids);
    free(set->values);
    memset(set, 0, sizeof(*set));
}
