/*
 *
 *      drm_property.c
 *      DRM KMS property creation, lookup, and destruction
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
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

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define DRM_PROPERTY_BLOB_MAX_SIZE (16u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* Internal helpers exported by drm_mode_object.c                     */
/* ------------------------------------------------------------------ */

/* Allocate a mode-object ID and init the header; see drm_mode_object.c. */

/* Decrement refcount under lock; return true iff it reached zero. */

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
    if (!e) {
        plogk("drm_property: Enum entry allocation failed, returning -ENOMEM.\n");
        return -ENOMEM;
    }
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
        ilist_node_t             *next = node->next;
        struct drm_property_enum *e    = container_of(node, struct drm_property_enum, head);

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

    if (!dev || !name || num_values < 0) {
        plogk("drm_property: Create with invalid args (dev=%p, name=%p, num_values=%d)\n", dev, name, num_values);
        return NULL;
    }

    prop = malloc(sizeof(*prop));
    if (!prop) {
        plogk("drm_property: Create %s: property allocation failed.\n", name);
        return NULL;
    }
    memset(prop, 0, sizeof(*prop));

    if (drm_mode_object_idr_alloc(dev, &prop->base, DRM_MODE_OBJECT_PROPERTY)) {
        free(prop);
        plogk("drm_property: Create %s: mode object IDR allocation failed.\n", name);
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
            plogk("drm_property: Create %s: values array allocation failed (num_values=%d)\n", name, num_values);
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
struct drm_property *drm_property_create_range(struct drm_device *dev, uint32_t flags, const char *name, uint64_t min, uint64_t max)
{
    struct drm_property *prop;

    if (!dev || !name) {
        plogk("drm_property: Create_range with invalid args (dev=%p, name=%p)\n", dev, name);
        return NULL;
    }

    prop = drm_property_create(dev, DRM_MODE_PROP_RANGE | flags, name, 2);
    if (!prop) return NULL;

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

    if (!dev || !name) {
        plogk("drm_property: Create_enum with invalid args (dev=%p, name=%p)\n", dev, name);
        return NULL;
    }
    if (num_enums < 0) {
        plogk("drm_property: Create_enum %s: negative num_enums (%d)\n", name, num_enums);
        return NULL;
    }
    if (num_enums > 0 && !enums) {
        plogk("drm_property: Create_enum %s: enums array missing (num_enums=%d)\n", name, num_enums);
        return NULL;
    }

    prop = drm_property_create(dev, DRM_MODE_PROP_ENUM | flags, name, num_enums);
    if (!prop) return NULL;

    for (i = 0; i < num_enums; i++) {
        if (drm_property_add_enum(prop, i, enums[i].value, enums[i].name)) {
            drm_property_destroy(dev, prop);
            plogk("drm_property: Create_enum %s: enum entry %d add failed, rolling back.\n", name, i);
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
                                                 const struct drm_mode_property_enum *enums, int num_enums, uint32_t supported_bits)
{
    struct drm_property *prop;
    int                  i, j;

    if (!dev || !name) {
        plogk("drm_property: Create_bitmask with invalid args (dev=%p, name=%p)\n", dev, name);
        return NULL;
    }
    if (num_enums < 0) {
        plogk("drm_property: Create_bitmask %s: negative num_enums (%d)\n", name, num_enums);
        return NULL;
    }
    if (num_enums > 0 && !enums) {
        plogk("drm_property: Create_bitmask %s: enums array missing (num_enums=%d)\n", name, num_enums);
        return NULL;
    }

    for (i = 0, j = 0; i < num_enums; i++)
        if (i < 32 && (supported_bits & (1U << i))) j++;

    prop = drm_property_create(dev, DRM_MODE_PROP_BITMASK | flags, name, j);
    if (!prop) return NULL;

    for (i = 0, j = 0; i < num_enums; i++) {
        if (i >= 32 || !(supported_bits & (1U << i))) continue;
        if (drm_property_add_enum(prop, j, enums[i].value, enums[i].name)) {
            drm_property_destroy(dev, prop);
            plogk("drm_property: Create_bitmask %s: enum entry %d add failed, rolling back.\n", name, i);
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

    if (!dev) {
        plogk("drm_property: Create_blob with NULL device.\n");
        return NULL;
    }
    if (length > 0 && !data) {
        plogk("drm_property: Create_blob with NULL data (length=%zu)\n", length);
        return NULL;
    }

    blob = malloc(sizeof(*blob));
    if (!blob) {
        plogk("drm_property: Create_blob: blob allocation failed (length=%zu)\n", length);
        return NULL;
    }
    memset(blob, 0, sizeof(*blob));

    if (length > 0) {
        buf = malloc(length);
        if (!buf) {
            free(blob);
            plogk("drm_property: Create_blob: data buffer allocation failed (length=%zu)\n", length);
            return NULL;
        }
        memcpy(buf, data, length);
    }

    if (drm_mode_object_idr_alloc(dev, &blob->base, DRM_MODE_OBJECT_BLOB)) {
        free(buf);
        free(blob);
        plogk("drm_property: Create_blob: mode object IDR allocation failed (length=%zu)\n", length);
        return NULL;
    }

    blob->data   = buf;
    blob->length = length;
    ilist_init(&blob->head_global);
    ilist_init(&blob->head_file);

    spin_lock(&dev->mode_config.blob_lock);
    ilist_insert_after(&dev->mode_config.property_blob_list, &blob->head_global);
    spin_unlock(&dev->mode_config.blob_lock);

    return blob;
}

/* Acquire a reference on a blob. */
void drm_property_blob_get(struct drm_property_blob *blob)
{
    if (!blob) return;
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

    if (!blob) return;
    if (!drm_mode_object_put_dec_and_test(&blob->base)) return;

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

    if (!dev) {
        plogk("drm_property: Lookup_blob with NULL device.\n");
        return NULL;
    }
    obj = drm_mode_object_find(dev, NULL, id, DRM_MODE_OBJECT_BLOB);
    if (!obj) return NULL;
    return container_of(obj, struct drm_property_blob, base);
}

/*
 * Return a property blob to userspace. A zero length is the normal size
 * query; otherwise the supplied buffer must hold the whole immutable blob.
 */
int drm_mode_getblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_blob *req = data;
    struct drm_property_blob *blob;
    uint32_t                  capacity;
    int                       ret = 0;

    (void)file_priv;
    if (!dev || !req) {
        plogk("drm_property: GETBLOB with invalid args (dev=%p, req=%p)\n", dev, req);
        return -EINVAL;
    }

    blob = drm_property_lookup_blob(dev, req->blob_id);
    if (!blob) {
        plogk("drm_property: GETBLOB: blob %u not found, returning -ENOENT.\n", req->blob_id);
        return -ENOENT;
    }
    if (blob->length > UINT32_MAX) {
        size_t blob_length = blob->length;
        drm_property_blob_put(blob);
        plogk("drm_property: GETBLOB: blob %u too large (%zu bytes), returning -E2BIG.\n", req->blob_id, blob_length);
        return -E2BIG;
    }

    capacity    = req->length;
    req->length = (uint32_t)blob->length;
    if (capacity) {
        if (capacity < blob->length) {
            plogk("drm_property: GETBLOB: buffer too small for blob %u (capacity=%u, length=%zu), returning -EINVAL.\n", req->blob_id, capacity,
                  blob->length);
            ret = -EINVAL;
        } else if (!req->data || copy_to_user((void *)(uintptr_t)req->data, blob->data, blob->length)) {
            plogk("drm_property: GETBLOB: copy_to_user failed for blob %u, returning -EFAULT.\n", req->blob_id);
            ret = -EFAULT;
        }
    }

    drm_property_blob_put(blob);
    return ret;
}

/*
 * Create an immutable userspace-owned blob. Its initial object reference is
 * owned by this drm_file until DESTROYPROPBLOB or file close.
 */
int drm_mode_createblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_create_blob *req = data;
    struct drm_property_blob    *blob;
    void                        *payload;

    if (!dev || !req || !file_priv) {
        plogk("drm_property: CREATEBLOB with invalid args (dev=%p, req=%p, file_priv=%p), returning -EINVAL.\n", dev, req, file_priv);
        return -EINVAL;
    }
    if (!req->length || !req->data || req->length > DRM_PROPERTY_BLOB_MAX_SIZE) {
        plogk("drm_property: CREATEBLOB: invalid length/data (length=%u, data=%p), returning -EINVAL.\n", req->length, req->data);
        return -EINVAL;
    }

    payload = malloc(req->length);
    if (!payload) {
        plogk("drm_property: CREATEBLOB: payload allocation failed (length=%u), returning -ENOMEM.\n", req->length);
        return -ENOMEM;
    }
    if (copy_from_user(payload, (const void *)(uintptr_t)req->data, req->length)) {
        free(payload);
        plogk("drm_property: CREATEBLOB: copy_from_user failed, returning -EFAULT.\n");
        return -EFAULT;
    }

    blob = drm_property_create_blob(dev, payload, req->length);
    free(payload);
    if (!blob) return -ENOMEM;

    spin_lock(&file_priv->table_lock);
    ilist_insert_after(&file_priv->blobs_head, &blob->head_file);
    spin_unlock(&file_priv->table_lock);
    req->blob_id = blob->base.id;
    return 0;
}

/* A blob may only be destroyed by the file which created it. */
int drm_mode_destroyblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_destroy_blob *req  = data;
    struct drm_property_blob     *blob = NULL;
    ilist_node_t                 *node;

    (void)dev;
    if (!req || !file_priv) {
        plogk("drm_property: DESTROYBLOB with invalid args (req=%p, file_priv=%p)\n", req, file_priv);
        return -EINVAL;
    }

    spin_lock(&file_priv->table_lock);
    for (node = file_priv->blobs_head.next; node && node != &file_priv->blobs_head; node = node->next) {
        struct drm_property_blob *candidate = container_of(node, struct drm_property_blob, head_file);
        if (candidate->base.id == req->blob_id) {
            blob = candidate;
            ilist_remove(&blob->head_file);
            break;
        }
    }
    spin_unlock(&file_priv->table_lock);

    if (!blob) {
        plogk("drm_property: DESTROYBLOB: blob %u not owned by this file, returning -ENOENT.\n", req->blob_id);
        return -ENOENT;
    }
    drm_property_blob_put(blob);
    return 0;
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
    if (!dev || !property) return;

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

/* Forward declaration */
struct drm_property *drm_property_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id);

/*
 * drm_mode_getproperty_ioctl - Handle DRM_IOCTL_MODE_GETPROPERTY.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_get_property (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up a property by ID and fills in its name, flags, value count,
 * and enum/blob count.
 */
int drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_property *prop_req = (struct drm_mode_get_property *)data;
    struct drm_property          *prop;

    (void)file_priv;

    if (!dev || !prop_req) {
        plogk("drm_property: GETPROPERTY with invalid args (dev=%p, prop_req=%p)\n", dev, prop_req);
        return -EINVAL;
    }

    prop = drm_property_find(dev, NULL, prop_req->prop_id);
    if (!prop) {
        plogk("drm_property: GETPROPERTY: property %u not found, returning -ENOENT.\n", prop_req->prop_id);
        return -ENOENT;
    }

    uint32_t user_values = prop_req->count_values;
    uint32_t user_enums  = prop_req->count_enum_blobs;
    uint32_t enum_count  = 0;

    strncpy(prop_req->name, prop->name, DRM_PROP_NAME_LEN - 1);
    prop_req->name[DRM_PROP_NAME_LEN - 1] = '\0';
    prop_req->flags                       = prop->flags;
    prop_req->count_values                = prop->num_values;

    /* Count enum entries */
    {
        ilist_node_t *node;
        node = prop->enum_list.next;
        while (node && node != &prop->enum_list) {
            enum_count++;
            node = node->next;
        }
        prop_req->count_enum_blobs = enum_count;
    }

    if (user_values && prop->num_values) {
        uint32_t count = user_values < prop->num_values ? user_values : prop->num_values;
        if (!prop_req->values_ptr
            || copy_to_user((void *)(uintptr_t)prop_req->values_ptr, prop->values, (size_t)count * sizeof(*prop->values))) {
            drm_mode_object_put(&prop->base);
            plogk("drm_property: GETPROPERTY: values copy_to_user failed for property %u, returning -EFAULT.\n", prop_req->prop_id);
            return -EFAULT;
        }
    }
    if (user_enums && enum_count) {
        uint32_t                       count   = user_enums < enum_count ? user_enums : enum_count;
        struct drm_mode_property_enum *entries = malloc((size_t)count * sizeof(*entries));
        ilist_node_t                  *node    = prop->enum_list.next;
        if (!entries) {
            drm_mode_object_put(&prop->base);
            plogk("drm_property: GETPROPERTY: enum entries allocation failed (count=%u), returning -ENOMEM.\n", count);
            return -ENOMEM;
        }
        for (uint32_t i = 0; i < count; i++, node = node->next) {
            struct drm_property_enum *entry = container_of(node, struct drm_property_enum, head);
            entries[i].value                = entry->value;
            memcpy(entries[i].name, entry->name, sizeof(entries[i].name));
        }
        if (!prop_req->enum_blob_ptr || copy_to_user((void *)(uintptr_t)prop_req->enum_blob_ptr, entries, (size_t)count * sizeof(*entries))) {
            free(entries);
            drm_mode_object_put(&prop->base);
            plogk("drm_property: GETPROPERTY: enum copy_to_user failed for property %u, returning -EFAULT.\n", prop_req->prop_id);
            return -EFAULT;
        }
        free(entries);
    }

    drm_mode_object_put(&prop->base);
    return 0;
}

/* User-initiated property destruction (same as drm_property_destroy). */
static void drm_property_destroy_user(struct drm_device *dev, struct drm_property *property)
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

    if (!dev) {
        plogk("drm_property: Find with NULL device.\n");
        return NULL;
    }
    obj = drm_mode_object_find(dev, file_priv, id, DRM_MODE_OBJECT_PROPERTY);
    if (!obj) return NULL;
    return container_of(obj, struct drm_property, base);
}
