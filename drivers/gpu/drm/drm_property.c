/*
 *
 *      drm_property.c
 *      DRM KMS property creation, lookup, and destruction
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

/* ------------------------------------------------------------------ */
/* Internal helpers exported by drm_mode_object.c                     */
/* ------------------------------------------------------------------ */

/* Allocate a mode-object ID and init the header; see drm_mode_object.c. */
extern int  drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

/* Decrement refcount under lock; return true iff it reached zero. */
extern bool drm_mode_object_put_dec_and_test(struct drm_mode_object *obj);

/* ------------------------------------------------------------------ */
/* Local helpers                                                      */
/* ------------------------------------------------------------------ */

/* Forward declaration: defined later in this file. */
void drm_property_destroy(struct drm_device *dev, struct drm_property *property);

/*
 * Allocate one enum entry, link it into @prop->enum_list, and record its
 * value at @index in @prop->values. Returns 0 on success or -ENOMEM.
 */
static int drm_property_add_enum(struct drm_property *prop, int index, uint64_t value, const char *name)
{
    struct drm_property_enum *e;

    e = malloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    memset(e, 0, sizeof(*e));
    e->value = value;
    strncpy(e->name, name, DRM_PROP_NAME_LEN - 1);
    e->name[DRM_PROP_NAME_LEN - 1] = '\0';
    ilist_insert_after(&prop->enum_list, &e->head);
    prop->values[index] = value;
    return 0;
}

/* Free every drm_property_enum entry linked to @prop->enum_list. */
static void drm_property_free_enum_list(struct drm_property *prop)
{
    ilist_node_t *node;

    node = prop->enum_list.next;
    while (node && node != &prop->enum_list) {
        ilist_node_t              *next = node->next;
        struct drm_property_enum *e     = container_of(node, struct drm_property_enum, head);

        ilist_remove(node);
        free(e);
        node = next;
    }
}

/* ------------------------------------------------------------------ */
/* Property construction                                              */
/* ------------------------------------------------------------------ */

/*
 * Create and register a new KMS property of @flags with @name and
 * @num_values value slots. The property is published in the device IDR
 * and the mode_config.property_list. Returns the property (refcount 1)
 * or NULL on failure; all intermediate allocations are rolled back.
 */
struct drm_property *drm_property_create(struct drm_device *dev, uint32_t flags, const char *name, int num_values)
{
    struct drm_property *prop;

    if (!dev || !name || num_values < 0)
        return NULL;

    prop = malloc(sizeof(*prop));
    if (!prop)
        return NULL;
    memset(prop, 0, sizeof(*prop));

    if (drm_mode_object_idr_alloc(dev, &prop->base, DRM_MODE_OBJECT_PROPERTY)) {
        free(prop);
        return NULL;
    }

    strncpy(prop->name, name, DRM_PROP_NAME_LEN - 1);
    prop->name[DRM_PROP_NAME_LEN - 1] = '\0';

    prop->flags      = flags;
    prop->num_values = (uint32_t)num_values;

    if (num_values > 0) {
        prop->values = malloc((size_t)num_values * sizeof(uint64_t));
        if (!prop->values) {
            spin_lock(&dev->mode_config.idr_mutex);
            drm_idr_remove(&dev->mode_config.object_idr, prop->base.id);
            spin_unlock(&dev->mode_config.idr_mutex);
            free(prop);
            return NULL;
        }
        memset(prop->values, 0, (size_t)num_values * sizeof(uint64_t));
    }

    ilist_init(&prop->enum_list);

    spin_lock(&dev->mode_config.mutex);
    ilist_insert_after(&dev->mode_config.property_list, &prop->dev_head);
    spin_unlock(&dev->mode_config.mutex);

    prop->dev = dev;
    return prop;
}

/* Create a signed/unsigned range property with bounds [min, max]. */
struct drm_property *drm_property_create_range(struct drm_device *dev, uint32_t flags, const char *name,
                                               uint64_t min, uint64_t max)
{
    struct drm_property *prop;

    if (!dev || !name)
        return NULL;

    prop = drm_property_create(dev, DRM_MODE_PROP_RANGE | flags, name, 2);
    if (!prop)
        return NULL;

    prop->values[0] = min;
    prop->values[1] = max;
    return prop;
}

/*
 * Create an enumerated property. One drm_property_enum entry is allocated
 * per supplied enum; values[i] mirrors enums[i].value. Returns NULL on
 * failure with full rollback.
 */
struct drm_property *drm_property_create_enum(struct drm_device *dev, uint32_t flags, const char *name,
                                              const struct drm_mode_property_enum *enums, int num_enums)
{
    struct drm_property *prop;
    int                  i;

    if (!dev || !name)
        return NULL;
    if (num_enums < 0)
        return NULL;
    if (num_enums > 0 && !enums)
        return NULL;

    prop = drm_property_create(dev, DRM_MODE_PROP_ENUM | flags, name, num_enums);
    if (!prop)
        return NULL;

    for (i = 0; i < num_enums; i++) {
        if (drm_property_add_enum(prop, i, enums[i].value, enums[i].name)) {
            drm_property_destroy(dev, prop);
            return NULL;
        }
    }
    return prop;
}

/*
 * Create a bitmask property. Only enums whose index bit is set in
 * @supported_bits (bits 0..31) are included; the rest are skipped.
 * values[j] mirrors the included enums[i].value. Returns NULL on failure
 * with full rollback.
 */
struct drm_property *drm_property_create_bitmask(struct drm_device *dev, uint32_t flags, const char *name,
                                                 const struct drm_mode_property_enum *enums, int num_enums,
                                                 uint32_t supported_bits)
{
    struct drm_property *prop;
    int                  i, j;

    if (!dev || !name)
        return NULL;
    if (num_enums < 0)
        return NULL;
    if (num_enums > 0 && !enums)
        return NULL;

    for (i = 0, j = 0; i < num_enums; i++) {
        if (i < 32 && (supported_bits & (1U << i)))
            j++;
    }

    prop = drm_property_create(dev, DRM_MODE_PROP_BITMASK | flags, name, j);
    if (!prop)
        return NULL;

    for (i = 0, j = 0; i < num_enums; i++) {
        if (i >= 32 || !(supported_bits & (1U << i)))
            continue;
        if (drm_property_add_enum(prop, j, enums[i].value, enums[i].name)) {
            drm_property_destroy(dev, prop);
            return NULL;
        }
        j++;
    }
    return prop;
}

/* ------------------------------------------------------------------ */
/* Blobs                                                              */
/* ------------------------------------------------------------------ */

/*
 * Create a property blob wrapping a copy of @data (@length bytes). The blob
 * is published in the device IDR and mode_config.property_blob_list with an
 * initial reference count of one. Zero-length blobs are permitted (data may
 * be NULL). Returns the blob or NULL on failure with full rollback.
 */
struct drm_property_blob *drm_property_create_blob(struct drm_device *dev, const void *data, size_t length)
{
    struct drm_property_blob *blob;
    void                     *buf = NULL;

    if (!dev)
        return NULL;
    if (length > 0 && !data)
        return NULL;

    blob = malloc(sizeof(*blob));
    if (!blob)
        return NULL;
    memset(blob, 0, sizeof(*blob));

    if (length > 0) {
        buf = malloc(length);
        if (!buf) {
            free(blob);
            return NULL;
        }
        memcpy(buf, data, length);
    }

    if (drm_mode_object_idr_alloc(dev, &blob->base, DRM_MODE_OBJECT_BLOB)) {
        free(buf);
        free(blob);
        return NULL;
    }

    blob->data   = buf;
    blob->length = length;

    spin_lock(&dev->mode_config.blob_lock);
    ilist_insert_after(&dev->mode_config.property_blob_list, &blob->head_global);
    spin_unlock(&dev->mode_config.blob_lock);

    return blob;
}

/* Acquire a reference on a blob. */
void drm_property_blob_get(struct drm_property_blob *blob)
{
    if (!blob)
        return;
    drm_mode_object_get(&blob->base);
}

/*
 * Release a reference on a blob. When the last reference is dropped the
 * blob is removed from the device IDR and the global blob list, its data
 * buffer is freed, and the blob struct itself is freed. The decrement and
 * the zero-test are performed atomically under ref_lock to guarantee a
 * unique owner for the finalisation.
 */
void drm_property_blob_put(struct drm_property_blob *blob)
{
    struct drm_device *dev;

    if (!blob)
        return;
    if (!drm_mode_object_put_dec_and_test(&blob->base))
        return;

    dev = blob->base.dev;

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, blob->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    spin_lock(&dev->mode_config.blob_lock);
    ilist_remove(&blob->head_global);
    spin_unlock(&dev->mode_config.blob_lock);

    free(blob->data);
    free(blob);
}

/*
 * Look up a blob by userspace ID. Returns the blob with an extra reference
 * (the caller must drm_property_blob_put it) or NULL if not found.
 */
struct drm_property_blob *drm_property_lookup_blob(struct drm_device *dev, uint32_t id)
{
    struct drm_mode_object *obj;

    if (!dev)
        return NULL;
    obj = drm_mode_object_find(dev, NULL, id, DRM_MODE_OBJECT_BLOB);
    if (!obj)
        return NULL;
    return container_of(obj, struct drm_property_blob, base);
}

/* ------------------------------------------------------------------ */
/* Destruction and lookup                                             */
/* ------------------------------------------------------------------ */

/*
 * Tear down a property: unlink it from the device property list, free all
 * enum entries and the value array, remove it from the object IDR, and
 * free the property struct. The caller must hold no other reference to
 * @property (i.e. this is finalisation, not a refcount drop).
 */
void drm_property_destroy(struct drm_device *dev, struct drm_property *property)
{
    if (!dev || !property)
        return;

    spin_lock(&dev->mode_config.mutex);
    ilist_remove(&property->dev_head);
    spin_unlock(&dev->mode_config.mutex);

    drm_property_free_enum_list(property);

    free(property->values);
    property->values = NULL;

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, property->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    free(property);
}

/* User-initiated property destruction (same as drm_property_destroy). */
void drm_property_destroy_user(struct drm_device *dev, struct drm_property *property)
{
    drm_property_destroy(dev, property);
}

/*
 * Look up a property by userspace ID, optionally consulting the per-file
 * handle IDR via @file_priv. Returns the property with an extra reference
 * (the caller must drm_mode_object_put(&prop->base) it) or NULL.
 */
struct drm_property *drm_property_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id)
{
    struct drm_mode_object *obj;

    if (!dev)
        return NULL;
    obj = drm_mode_object_find(dev, file_priv, id, DRM_MODE_OBJECT_PROPERTY);
    if (!obj)
        return NULL;
    return container_of(obj, struct drm_property, base);
}
