/*
 *
 *      drm_connector.c
 *      DRM connector management
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
#include <proc/uaccess.h>
#include <sync/spin_lock.h>

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Internal helpers from drm_mode_object.c and drm_property.c */
extern int                       drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);
extern struct drm_property_blob *drm_property_create_blob(struct drm_device *dev, const void *data, size_t length);
extern void                      drm_property_blob_put(struct drm_property_blob *blob);
extern void                      drm_convert_to_umode(struct drm_mode_modeinfo *out, const struct drm_display_mode *in);

/*
 * drm_connector_init - Initialise a connector object.
 * @dev: DRM device
 * @connector: connector to initialise
 * @funcs: connector helper funcs pointer (stored in helper_private)
 * @connector_type: DRM_MODE_CONNECTOR_* type
 *
 * Allocates a mode-object ID, initialises the mutex and mode lists,
 * inserts into the device connector list, and sets defaults.
 * Returns 0 on success or a negative errno on failure.
 */
int drm_connector_init(struct drm_device *dev, struct drm_connector *connector, void *funcs, int connector_type)
{
    int ret;

    if (!dev || !connector) { return -EINVAL; }

    ret = drm_mode_object_idr_alloc(dev, &connector->base, DRM_MODE_OBJECT_CONNECTOR);
    if (ret) { return ret; }

    drm_modeset_lock_init(&connector->mutex);

    ilist_init(&connector->modes);
    ilist_init(&connector->user_modes);

    ilist_insert_after(&dev->mode_config.connector_list, &connector->head);

    connector->dev                     = dev;
    connector->connector_type          = (uint32_t)connector_type;
    connector->connector_type_id       = 0;
    connector->status                  = connector_status_unknown;
    connector->force                   = DRM_FORCE_UNSPECIFIED;
    connector->helper_private          = funcs;
    connector->state                   = NULL;
    connector->edid_blob               = NULL;
    connector->path_blob               = NULL;
    connector->tile_blob               = NULL;
    connector->eld                     = NULL;
    connector->edid_blob_ptr           = NULL;
    connector->possible_encoders_count = 0;
    connector->possible_encoders_ids   = NULL;
    connector->interlace_allowed       = false;
    connector->doublescan_allowed      = false;
    connector->stereo_allowed          = false;
    connector->ycbcr_420_allowed       = 0;
    connector->display_info_width_mm   = 0;
    connector->display_info_height_mm  = 0;
    connector->null_edid_counter       = 0;
    connector->override_edid           = false;
    connector->override_edid_set       = false;
    memset(&connector->edid_lock, 0, sizeof(connector->edid_lock));
    memset(connector->name, 0, sizeof(connector->name));

    dev->mode_config.num_connector++;

    ret = drm_object_attach_property(&connector->base, dev->mode_config.prop_crtc_id, 0);
    if (ret) {
        drm_connector_cleanup(connector);
        return ret;
    }

    return 0;
}

/*
 * drm_connector_attach_encoder - Attach an encoder to a connector's possible encoders list.
 * @connector: connector
 * @encoder: encoder to attach
 *
 * Grows the possible_encoders_ids array by one and appends the encoder's
 * base ID. Returns 0 on success or -ENOMEM.
 */
int drm_connector_attach_encoder(struct drm_connector *connector, struct drm_encoder *encoder)
{
    uint32_t *new_ids;
    uint32_t  new_count;

    if (!connector || !encoder) { return -EINVAL; }

    new_count = connector->possible_encoders_count + 1;
    new_ids   = realloc(connector->possible_encoders_ids, (size_t)new_count * sizeof(uint32_t));
    if (!new_ids) { return -ENOMEM; }

    new_ids[connector->possible_encoders_count] = encoder->base.id;
    connector->possible_encoders_ids            = new_ids;
    connector->possible_encoders_count          = new_count;

    return 0;
}

/*
 * drm_connector_register - Register a connector with userspace.
 * @connector: connector to register
 *
 * MVP placeholder; returns 0.
 */
int drm_connector_register(struct drm_connector *connector)
{
    if (!connector) { return -EINVAL; }

    /* MVP: late-registration callbacks and sysfs not yet implemented. */
    return 0;
}

/*
 * drm_mode_getconnector - Handle DRM_IOCTL_MODE_GETCONNECTOR.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_get_connector (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up the connector by id, fills the struct with encoder count,
 * mode count, connection status, and physical dimensions.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_connector *conn_req = (struct drm_mode_get_connector *)data;
    struct drm_mode_object        *obj;
    struct drm_connector          *connector;
    int                            mode_count;
    int                            encoder_count;
    uint32_t                       user_modes, user_encoders, user_props;

    if (!dev || !conn_req) { return -EINVAL; }

    user_modes    = conn_req->count_modes;
    user_encoders = conn_req->count_encoders;
    user_props    = conn_req->count_props;
    obj           = drm_mode_object_find(dev, file_priv, conn_req->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!obj) { return -ENOENT; }
    connector = container_of(obj, struct drm_connector, base);

    /* Count modes in the modes list */
    mode_count = 0;
    {
        ilist_node_t *node = connector->modes.next;
        while (node && node != &connector->modes) {
            mode_count++;
            node = node->next;
        }
    }

    encoder_count = (int)connector->possible_encoders_count;

    if (user_modes && mode_count) {
        uint32_t                  count = user_modes < (uint32_t)mode_count ? user_modes : (uint32_t)mode_count;
        struct drm_mode_modeinfo *modes = malloc((size_t)count * sizeof(*modes));
        ilist_node_t             *node  = connector->modes.next;
        if (!modes) {
            drm_mode_object_put(obj);
            return -ENOMEM;
        }
        for (uint32_t i = 0; i < count; i++, node = node->next)
            drm_convert_to_umode(&modes[i], container_of(node, struct drm_display_mode, head));
        if (!conn_req->modes_ptr || copy_to_user((void *)(uintptr_t)conn_req->modes_ptr, modes, (size_t)count * sizeof(*modes))) {
            free(modes);
            drm_mode_object_put(obj);
            return -EFAULT;
        }
        free(modes);
    }
    if (user_encoders && encoder_count) {
        uint32_t count = user_encoders < (uint32_t)encoder_count ? user_encoders : (uint32_t)encoder_count;
        if (!conn_req->encoders_ptr
            || copy_to_user((void *)(uintptr_t)conn_req->encoders_ptr, connector->possible_encoders_ids,
                            (size_t)count * sizeof(*connector->possible_encoders_ids))) {
            drm_mode_object_put(obj);
            return -EFAULT;
        }
    }
    if (connector->base.properties && user_props) {
        struct drm_property_set *set = connector->base.properties;
        uint32_t                 count;
        uint32_t                *ids    = NULL;
        uint64_t                *values = NULL;
        spin_lock(&set->lock);
        count = user_props < set->count ? user_props : set->count;
        if (count) {
            ids    = malloc((size_t)count * sizeof(*ids));
            values = malloc((size_t)count * sizeof(*values));
            if (ids && values) {
                memcpy(ids, set->ids, (size_t)count * sizeof(*ids));
                memcpy(values, set->values, (size_t)count * sizeof(*values));
            }
        }
        spin_unlock(&set->lock);
        if (count && (!ids || !values)) {
            free(ids);
            free(values);
            drm_mode_object_put(obj);
            return -ENOMEM;
        }
        if (count
            && (!conn_req->props_ptr || !conn_req->prop_values_ptr
                || copy_to_user((void *)(uintptr_t)conn_req->props_ptr, ids, (size_t)count * sizeof(*ids))
                || copy_to_user((void *)(uintptr_t)conn_req->prop_values_ptr, values, (size_t)count * sizeof(*values)))) {
            free(ids);
            free(values);
            drm_mode_object_put(obj);
            return -EFAULT;
        }
        free(ids);
        free(values);
    }

    conn_req->encoder_id        = connector->state && connector->state->best_encoder ? connector->state->best_encoder->base.id : 0;
    conn_req->connector_type    = connector->connector_type;
    conn_req->connector_type_id = connector->connector_type_id;
    conn_req->connection        = (__u32)connector->status;
    conn_req->mm_width          = connector->display_info_width_mm;
    conn_req->mm_height         = connector->display_info_height_mm;
    conn_req->subpixel          = 0;
    conn_req->count_modes       = (__u32)mode_count;
    conn_req->count_props       = connector->base.properties ? connector->base.properties->count : 0;
    conn_req->count_encoders    = (__u32)encoder_count;

    drm_mode_object_put(obj);
    return 0;
}

/*
 * drm_connector_cleanup - Tear down a connector and release its resources.
 * @connector: connector to clean up
 *
 * Removes the connector from the device connector list, removes it from
 * the global IDR, frees the possible encoders array, releases the EDID
 * blob, and decrements num_connector.
 */
void drm_connector_cleanup(struct drm_connector *connector)
{
    struct drm_device *dev;

    if (!connector) { return; }

    dev = connector->dev;

    while (connector->modes.next && connector->modes.next != &connector->modes) {
        struct drm_display_mode *mode = container_of(connector->modes.next, struct drm_display_mode, head);
        ilist_remove(&mode->head);
        free(mode);
    }

    ilist_remove(&connector->head);

    if (dev) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, connector->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_connector > 0) { dev->mode_config.num_connector--; }
    }

    free(connector->possible_encoders_ids);
    connector->possible_encoders_ids   = NULL;
    connector->possible_encoders_count = 0;

    if (connector->edid_blob) {
        drm_property_blob_put(connector->edid_blob);
        connector->edid_blob = NULL;
    }

    if (connector->path_blob) {
        drm_property_blob_put(connector->path_blob);
        connector->path_blob = NULL;
    }

    if (connector->tile_blob) {
        drm_property_blob_put(connector->tile_blob);
        connector->tile_blob = NULL;
    }

    free(connector->eld);
    connector->eld = NULL;
    if (connector->base.properties) {
        drm_property_set_destroy(connector->base.properties);
        free(connector->base.properties);
        connector->base.properties = NULL;
    }
}

/*
 * drm_connector_update_edid_property - Update the EDID property blob for a connector.
 * @connector: connector
 * @edid: pointer to EDID data (may be NULL to clear)
 * @size: size of EDID data in bytes
 *
 * Destroys any existing EDID blob and creates a new one wrapping the
 * provided EDID data. Returns 0 on success or -ENOMEM.
 */
int drm_connector_update_edid_property(struct drm_connector *connector, const unsigned char *edid, size_t size)
{
    struct drm_device        *dev;
    struct drm_property_blob *new_blob = NULL;

    if (!connector || !connector->dev) { return -EINVAL; }

    dev = connector->dev;

    if (connector->edid_blob) {
        drm_property_blob_put(connector->edid_blob);
        connector->edid_blob = NULL;
    }

    if (edid && size > 0) {
        new_blob = drm_property_create_blob(dev, edid, size);
        if (!new_blob) { return -ENOMEM; }
    }

    connector->edid_blob = new_blob;
    return 0;
}
