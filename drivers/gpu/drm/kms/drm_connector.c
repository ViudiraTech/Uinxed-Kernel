/*
 *
 *      drm_connector.c
 *      DRM connector management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_idr.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_modeset_lock.h>
#include <drivers/gpu/drm/drm_print.h>
#include <fs/sysfs/drm_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>

/* Internal helpers from drm_mode_object.c and drm_property.c */

/*
 * drm_connector_type_name - Map a DRM_MODE_CONNECTOR_* value to its Linux name.
 * @type: DRM_MODE_CONNECTOR_* connector type
 *
 * Connector type names ("VGA", "DVI-I", "Virtual", ...).
 * Returns "Unknown" for any unrecognised type.
 */
const char *drm_connector_type_name(uint32_t type)
{
    switch (type) {
        case DRM_MODE_CONNECTOR_Unknown :
            return "Unknown";
        case DRM_MODE_CONNECTOR_VGA :
            return "VGA";
        case DRM_MODE_CONNECTOR_DVII :
            return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID :
            return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA :
            return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite :
            return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO :
            return "SVIDEO";
        case DRM_MODE_CONNECTOR_LVDS :
            return "LVDS";
        case DRM_MODE_CONNECTOR_Component :
            return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN :
            return "DIN";
        case DRM_MODE_CONNECTOR_DisplayPort :
            return "DP";
        case DRM_MODE_CONNECTOR_HDMIA :
            return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB :
            return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV :
            return "TV";
        case DRM_MODE_CONNECTOR_eDP :
            return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL :
            return "Virtual";
        case DRM_MODE_CONNECTOR_DSI :
            return "DSI";
        case DRM_MODE_CONNECTOR_DPI :
            return "DPI";
        case DRM_MODE_CONNECTOR_WRITEBACK :
            return "Writeback";
        case DRM_MODE_CONNECTOR_SPI :
            return "SPI";
        case DRM_MODE_CONNECTOR_USB :
            return "USB";
        default :
            return "Unknown";
    }
}

/* Initialise a connector object. Returns 0 on success or a negative errno on failure. */
int drm_connector_init(struct drm_device *dev, struct drm_connector *connector, void *funcs, int connector_type)
{
    int ret;

    if (!dev || !connector) {
        DRM_ERROR("Init with NULL dev or connector.\n");
        return -EINVAL;
    }

    /*
     * connector_type indexes dev->mode_config.connector_type_count[], so it
     * must stay inside the DRM_MODE_CONNECTOR_* range.
     */
    if (connector_type < 0 || connector_type > DRM_MODE_CONNECTOR_USB) {
        DRM_ERROR("Init with out-of-range connector_type %d.\n", connector_type);
        return -EINVAL;
    }

    ret = drm_mode_object_idr_alloc(dev, &connector->base, DRM_MODE_OBJECT_CONNECTOR);
    if (ret) {
        DRM_ERROR("Failed to allocate object id (ret=%d)\n", ret);
        return ret;
    }
    drm_modeset_lock_init(&connector->mutex);

    ilist_init(&connector->modes);
    ilist_init(&connector->user_modes);
    ilist_insert_after(&dev->mode_config.connector_list, &connector->head);

    connector->dev               = dev;
    connector->connector_type    = (uint32_t)connector_type;
    connector->connector_type_id = ++dev->mode_config.connector_type_count[connector_type];
    {
        /* Name, e.g. "Virtual-1", used in logs and DRM_IOCTL_MODE_GETCONNECTOR. */
        (void)snprintf(connector->name, sizeof(connector->name), "%s-%u", drm_connector_type_name(connector->connector_type), connector->connector_type_id);
    }
    connector->status                  = connector_status_unknown;
    connector->force                   = DRM_FORCE_UNSPECIFIED;
    connector->dpms                    = DRM_MODE_DPMS_ON;
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

    dev->mode_config.num_connector++;
    ret = drm_object_attach_property(&connector->base, dev->mode_config.prop_crtc_id, 0);
    if (ret) {
        DRM_ERROR("Failed to attach crtc_id property (ret=%d)\n", ret);
        drm_connector_cleanup(connector);
        return ret;
    }

    ret = drm_connector_attach_dpms_property(connector);
    if (ret) {
        DRM_ERROR("Failed to attach dpms property (ret=%d)\n", ret);
        drm_connector_cleanup(connector);
        return ret;
    }

    /* Standard connector properties: link-status (GOOD) and non-desktop (false). */
    ret = drm_object_attach_property(&connector->base, dev->mode_config.prop_link_status, DRM_MODE_LINK_STATUS_GOOD);
    if (!ret) ret = drm_object_attach_property(&connector->base, dev->mode_config.prop_non_desktop, 0);
    if (ret) {
        DRM_ERROR("Failed to attach link property (ret=%d)\n", ret);
        drm_connector_cleanup(connector);
        return ret;
    }

    return 0;
}

/* Attach an encoder to a connector's possible encoders list. */
int drm_connector_attach_encoder(struct drm_connector *connector, struct drm_encoder *encoder)
{
    uint32_t *new_ids;
    uint32_t  new_count;

    if (!connector || !encoder) {
        DRM_ERROR("Attach_encoder with NULL connector or encoder.\n");
        return -EINVAL;
    }

    new_count = connector->possible_encoders_count + 1;
    new_ids   = realloc(connector->possible_encoders_ids, (size_t)new_count * sizeof(uint32_t));
    if (!new_ids) {
        DRM_ERROR("Failed to grow possible encoders (count=%u)\n", new_count);
        return -ENOMEM;
    }

    new_ids[connector->possible_encoders_count] = encoder->base.id;
    connector->possible_encoders_ids            = new_ids;
    connector->possible_encoders_count          = new_count;

    return 0;
}

/*
 * drm_connector_register - Register a connector with userspace.
 * @connector: connector to register
 *
 * Validates the connector and exposes it under /sys/class/drm/.
 */
int drm_connector_register(struct drm_connector *connector)
{
    if (!connector) {
        DRM_ERROR("Register with NULL connector.\n");
        return -EINVAL;
    }
    drm_sysfs_connector_add(connector);
    return 0;
}

/* Handle DRM_IOCTL_MODE_GETCONNECTOR. Returns 0 on success or -EINVAL/-ENOENT. */
int drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_connector *conn_req = (struct drm_mode_get_connector *)data;
    struct drm_mode_object        *obj;
    struct drm_connector          *connector;
    int                            mode_count;
    int                            encoder_count;
    uint32_t                       user_modes, user_encoders, user_props;

    if (!dev || !conn_req) {
        DRM_ERROR("Getconnector with invalid args.\n");
        return -EINVAL;
    }

    user_modes    = conn_req->count_modes;
    user_encoders = conn_req->count_encoders;
    user_props    = conn_req->count_props;
    obj           = drm_mode_object_find(dev, file_priv, conn_req->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!obj) {
        DRM_ERROR("Connector %u not found.\n", conn_req->connector_id);
        return -ENOENT;
    }
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
        uint32_t count = user_modes < (uint32_t)mode_count ? user_modes : (uint32_t)mode_count;
        if ((size_t)count > SIZE_MAX / sizeof(struct drm_mode_modeinfo)) {
            DRM_ERROR("Mode count %u overflow.\n", count);
            drm_mode_object_put(obj);
            return -EOVERFLOW;
        }
        struct drm_mode_modeinfo *modes = malloc((size_t)count * sizeof(*modes));
        ilist_node_t             *node  = connector->modes.next;
        if (!modes) {
            DRM_ERROR("Failed to allocate mode list (%u modes)\n", count);
            drm_mode_object_put(obj);
            return -ENOMEM;
        }
        for (uint32_t i = 0; i < count; i++, node = node->next) drm_convert_to_umode(&modes[i], container_of(node, struct drm_display_mode, head));
        if (!conn_req->modes_ptr || copy_to_user((void *)(uintptr_t)conn_req->modes_ptr, modes, (size_t)count * sizeof(*modes))) {
            DRM_ERROR("Failed to copy modes to user.\n");
            free(modes);
            drm_mode_object_put(obj);
            return -EFAULT;
        }
        free(modes);
    }
    if (user_encoders && encoder_count) {
        uint32_t count = user_encoders < (uint32_t)encoder_count ? user_encoders : (uint32_t)encoder_count;
        if (!conn_req->encoders_ptr || copy_to_user((void *)(uintptr_t)conn_req->encoders_ptr, connector->possible_encoders_ids, (size_t)count * sizeof(*connector->possible_encoders_ids))) {
            DRM_ERROR("Failed to copy encoders to user.\n");
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
            DRM_ERROR("Failed to allocate property arrays (count=%u)\n", count);
            free(ids);
            free(values);
            drm_mode_object_put(obj);
            return -ENOMEM;
        }
        if (count
            && (!conn_req->props_ptr || !conn_req->prop_values_ptr || copy_to_user((void *)(uintptr_t)conn_req->props_ptr, ids, (size_t)count * sizeof(*ids))
                || copy_to_user((void *)(uintptr_t)conn_req->prop_values_ptr, values, (size_t)count * sizeof(*values)))) {
            DRM_ERROR("Failed to copy properties to user.\n");
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

    if (!connector) return;

    dev = connector->dev;

    /* Remove the connector's /sys/class/drm/ device first. */
    drm_sysfs_connector_remove(connector);

    while (connector->modes.next && connector->modes.next != &connector->modes) {
        struct drm_display_mode *mode = container_of(connector->modes.next, struct drm_display_mode, head);
        ilist_remove(&mode->head);
        free(mode); // NOLINT(clang-analyzer-unix.Malloc)
    }

    ilist_remove(&connector->head);

    if (dev) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, connector->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_connector > 0) dev->mode_config.num_connector--;
        /*
         * Give the per-type instance counter back so a re-init of the same
         * type keeps the names sequential (Virtual-1, not Virtual-2).
         */
        if (connector->connector_type <= DRM_MODE_CONNECTOR_USB && dev->mode_config.connector_type_count[connector->connector_type] > 0)
            dev->mode_config.connector_type_count[connector->connector_type]--;
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

    free(connector->state);
    connector->state = NULL;

    if (connector->base.properties) {
        drm_property_set_destroy(connector->base.properties);
        free(connector->base.properties);
        connector->base.properties = NULL;
    }
}

/* Legacy connector property setter (libdrm drmModeConnectorSetProperty); DPMS goes through the DPMS core, everything else through OBJ_SETPROPERTY. */
int drm_connector_property_set_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_connector_set_property *set_prop = (struct drm_mode_connector_set_property *)data;
    struct drm_mode_obj_set_property        obj_set_prop;

    if (!dev || !set_prop) {
        DRM_ERROR("SETPROPERTY with invalid args (dev=%p, set_prop=%p)\n", dev, set_prop);
        return -EINVAL;
    }

    obj_set_prop.value    = set_prop->value;
    obj_set_prop.prop_id  = set_prop->prop_id;
    obj_set_prop.obj_id   = set_prop->connector_id;
    obj_set_prop.obj_type = DRM_MODE_OBJECT_CONNECTOR;

    /* It does all the locking and checking we need. */
    return drm_mode_obj_setproperty_ioctl(dev, &obj_set_prop, file_priv);
}

/* Update the EDID property blob for a connector. */
int drm_connector_update_edid_property(struct drm_connector *connector, const unsigned char *edid, size_t size)
{
    struct drm_device        *dev;
    struct drm_property_blob *new_blob = NULL;

    if (!connector || !connector->dev) {
        DRM_ERROR("Update_edid_property with invalid connector.\n");
        return -EINVAL;
    }

    dev = connector->dev;

    if (connector->edid_blob) {
        drm_property_blob_put(connector->edid_blob);
        connector->edid_blob = NULL;
    }

    if (edid && size > 0) {
        new_blob = drm_property_create_blob(dev, edid, size);
        if (!new_blob) {
            DRM_ERROR("Failed to create edid blob (size=%zu)\n", size);
            return -ENOMEM;
        }
    }

    connector->edid_blob = new_blob;
    return 0;
}

/*
 * Add the Kconfig-driven fallback mode so a connector is never mode-less
 * when EDID/display-info probing yields nothing. Returns 0 on success.
 */
int drm_connector_add_fallback_mode(struct drm_connector *connector)
{
    struct drm_display_mode *mode;
    uint32_t                 w = DRM_DEFAULT_WIDTH, h = DRM_DEFAULT_HEIGHT;

    if (!connector || !connector->dev || !w || !h) return -EINVAL;

    mode = drm_mode_create(connector->dev);
    if (!mode) return -ENOMEM;

    (void)snprintf(mode->name, DRM_DISPLAY_MODE_LEN - 1, "%dx%d", w, h);
    mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
    mode->hdisplay                       = w;
    mode->hsync_start                    = w + 80;
    mode->hsync_end                      = w + 160;
    mode->htotal                         = w + 320;
    mode->vdisplay                       = h;
    mode->vsync_start                    = h + 3;
    mode->vsync_end                      = h + 6;
    mode->vtotal                         = h + 32;
    mode->vrefresh                       = 60;
    mode->clock                          = (int)((uint64_t)mode->htotal * (uint64_t)mode->vtotal * (uint64_t)mode->vrefresh / 1000ULL);
    mode->flags                          = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type                           = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    mode->status                         = MODE_OK;

    drm_mode_probed_add(connector, mode);
    return 0;
}
