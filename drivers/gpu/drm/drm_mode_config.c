/*
 *
 *      drm_mode_config.c
 *      DRM mode configuration initialisation and cleanup
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
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
#include <proc/uaccess.h>
#include <sync/spin_lock.h>

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define DRM_S32_MAX ((int32_t)0x7fffffff)
#define DRM_S32_MIN (-DRM_S32_MAX - 1)

/* Internal helpers from drm_property.c */

static struct drm_property *drm_object_property(struct drm_device *dev, const char *name, uint32_t object_type)
{
    struct drm_property *prop = drm_property_create(dev, DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC, name, 1);
    if (prop) prop->values[0] = object_type;
    return prop;
}

static struct drm_property *drm_signed_property(struct drm_device *dev, const char *name, int32_t min, int32_t max)
{
    struct drm_property *prop = drm_property_create(dev, DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC, name, 2);
    if (prop) {
        prop->values[0] = (uint64_t)(int64_t)min;
        prop->values[1] = (uint64_t)(int64_t)max;
    }
    return prop;
}

/* Forward declarations of cleanup functions from sibling compilation units. */

/*
 * drm_mode_config_init - Initialise the mode configuration for a DRM device.
 * @dev: DRM device
 *
 * Initialises the IDR allocators, intrusive lists, locks, sets default
 * min/max dimensions, cursor dimensions, and feature flags.
 * Returns 0 on success.
 */
int drm_mode_config_init(struct drm_device *dev)
{
    if (!dev) {
        plogk("drm: Mode_config_init called with NULL device.\n");
        return -EINVAL;
    }

    memset(&dev->mode_config.mutex, 0, sizeof(dev->mode_config.mutex));
    memset(&dev->mode_config.idr_mutex, 0, sizeof(dev->mode_config.idr_mutex));
    memset(&dev->mode_config.fb_lock, 0, sizeof(dev->mode_config.fb_lock));
    memset(&dev->mode_config.blob_lock, 0, sizeof(dev->mode_config.blob_lock));
    memset(&dev->mode_config.commit_queue_lock, 0, sizeof(dev->mode_config.commit_queue_lock));
    wait_queue_init(&dev->mode_config.commit_queue_wait);
    dev->mode_config.commit_queue_next = 0;
    dev->mode_config.commit_queue_done = 0;

    drm_idr_init(&dev->mode_config.object_idr);
    drm_idr_init(&dev->mode_config.fb_idr);

    ilist_init(&dev->mode_config.fb_list);
    ilist_init(&dev->mode_config.crtc_list);
    ilist_init(&dev->mode_config.connector_list);
    ilist_init(&dev->mode_config.encoder_list);
    ilist_init(&dev->mode_config.plane_list);
    ilist_init(&dev->mode_config.property_list);
    ilist_init(&dev->mode_config.property_blob_list);
    ilist_init(&dev->mode_config.private_obj_list);

    dev->mode_config.num_connector               = 0;
    dev->mode_config.num_encoder                 = 0;
    dev->mode_config.num_crtc                    = 0;
    dev->mode_config.num_plane                   = 0;
    dev->mode_config.num_total_plane             = 0;
    dev->mode_config.num_fb                      = 0;
    dev->mode_config.num_connector_property_list = 0;

    dev->mode_config.min_width     = 0;
    dev->mode_config.min_height    = 0;
    dev->mode_config.max_width     = 8192;
    dev->mode_config.max_height    = 8192;
    dev->mode_config.cursor_width  = 64;
    dev->mode_config.cursor_height = 64;

    dev->mode_config.async_page_flip                             = false;
    dev->mode_config.fb_modifiers_not_supported                  = false;
    dev->mode_config.normalize_zpos                              = true;
    dev->mode_config.atomic_async_page_flip_not_supported_unused = false;
    dev->mode_config.poll_enabled                                = false;
    dev->mode_config.poll_running                                = false;
    dev->mode_config.delayed_event                               = false;
    dev->mode_config.poll_init                                   = false;

    dev->mode_config.poll_work_unused = NULL;
    dev->mode_config.helper_private   = NULL;

    dev->mode_config.prop_src_x                   = NULL;
    dev->mode_config.prop_src_y                   = NULL;
    dev->mode_config.prop_src_w                   = NULL;
    dev->mode_config.prop_src_h                   = NULL;
    dev->mode_config.prop_crtc_x                  = NULL;
    dev->mode_config.prop_crtc_y                  = NULL;
    dev->mode_config.prop_crtc_w                  = NULL;
    dev->mode_config.prop_crtc_h                  = NULL;
    dev->mode_config.prop_fb_id                   = NULL;
    dev->mode_config.prop_in_fence_fd             = NULL;
    dev->mode_config.prop_out_fence_ptr           = NULL;
    dev->mode_config.prop_crtc_id                 = NULL;
    dev->mode_config.prop_active                  = NULL;
    dev->mode_config.prop_mode_id                 = NULL;
    dev->mode_config.prop_plane_type              = NULL;
    dev->mode_config.prop_zpos                    = NULL;
    dev->mode_config.prop_zpos_default            = NULL;
    dev->mode_config.prop_rotation                = NULL;
    dev->mode_config.prop_pixel_blend_mode        = NULL;
    dev->mode_config.prop_src_blend_pixel_unused  = NULL;
    dev->mode_config.prop_alpha                   = NULL;
    dev->mode_config.prop_connector_id            = NULL;
    dev->mode_config.prop_dpms                    = NULL;
    dev->mode_config.prop_path                    = NULL;
    dev->mode_config.prop_tile                    = NULL;
    dev->mode_config.prop_link_status             = NULL;
    dev->mode_config.prop_edid                    = NULL;
    dev->mode_config.prop_content_protection      = NULL;
    dev->mode_config.prop_scaling_mode            = NULL;
    dev->mode_config.prop_aspect_ratio            = NULL;
    dev->mode_config.prop_vrr_capable             = NULL;
    dev->mode_config.prop_hdr_output_metadata     = NULL;
    dev->mode_config.prop_aspect_ratio_unused     = NULL;
    dev->mode_config.prop_gamma_lut               = NULL;
    dev->mode_config.prop_degamma_lut             = NULL;
    dev->mode_config.prop_ctm                     = NULL;
    dev->mode_config.prop_gamma_lut_size          = NULL;
    dev->mode_config.prop_degamma_lut_size        = NULL;
    dev->mode_config.prop_ctm_size                = NULL;
    dev->mode_config.prop_max_bpc                 = NULL;
    dev->mode_config.prop_color_mode_unused       = NULL;
    dev->mode_config.prop_colorspace              = NULL;
    dev->mode_config.prop_writeback_fb_id         = NULL;
    dev->mode_config.prop_writeback_pix_fmt       = NULL;
    dev->mode_config.prop_writeback_out_fence_ptr = NULL;

    dev->mode_config.prop_fb_id   = drm_object_property(dev, "FB_ID", DRM_MODE_OBJECT_FB);
    dev->mode_config.prop_crtc_id = drm_object_property(dev, "CRTC_ID", DRM_MODE_OBJECT_CRTC);
    dev->mode_config.prop_active  = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "ACTIVE", 0, 1);
    dev->mode_config.prop_mode_id = drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC, "MODE_ID", 0);
    dev->mode_config.prop_src_x   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_X", 0, UINT32_MAX);
    dev->mode_config.prop_src_y   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_Y", 0, UINT32_MAX);
    dev->mode_config.prop_src_w   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_W", 0, UINT32_MAX);
    dev->mode_config.prop_src_h   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_H", 0, UINT32_MAX);
    dev->mode_config.prop_crtc_x  = drm_signed_property(dev, "CRTC_X", DRM_S32_MIN, DRM_S32_MAX);
    dev->mode_config.prop_crtc_y  = drm_signed_property(dev, "CRTC_Y", DRM_S32_MIN, DRM_S32_MAX);
    dev->mode_config.prop_crtc_w  = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "CRTC_W", 0, UINT32_MAX);
    dev->mode_config.prop_crtc_h  = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "CRTC_H", 0, UINT32_MAX);
    dev->mode_config.prop_zpos    = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "zpos", 0, 255);
    dev->mode_config.prop_alpha   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "alpha", 0, UINT16_MAX);

    {
        static const struct drm_mode_property_enum plane_types[] = {
            {DRM_PLANE_TYPE_OVERLAY, "Overlay"},
            {DRM_PLANE_TYPE_PRIMARY, "Primary"},
            {DRM_PLANE_TYPE_CURSOR,  "Cursor" },
        };
        dev->mode_config.prop_plane_type = drm_property_create_enum(dev, DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_ATOMIC, "type", plane_types, 3);
    }

    if (!dev->mode_config.prop_fb_id || !dev->mode_config.prop_crtc_id || !dev->mode_config.prop_active || !dev->mode_config.prop_mode_id
        || !dev->mode_config.prop_src_x || !dev->mode_config.prop_src_y || !dev->mode_config.prop_src_w || !dev->mode_config.prop_src_h
        || !dev->mode_config.prop_crtc_x || !dev->mode_config.prop_crtc_y || !dev->mode_config.prop_crtc_w || !dev->mode_config.prop_crtc_h
        || !dev->mode_config.prop_zpos || !dev->mode_config.prop_alpha || !dev->mode_config.prop_plane_type) {
        plogk("drm: Mode_config_init: core property creation failed, returning -ENOMEM.\n");
        drm_mode_config_cleanup(dev);
        return -ENOMEM;
    }

    return 0;
}

/*
 * drm_mode_config_cleanup_helper - Clean up a single intrusive list of KMS objects.
 *
 * Iterates a list where each node is embedded in a struct whose first
 * member is a drm_mode_object. The cleanup callback is invoked for
 * each object. After iterating, the list head is re-initialised.
 */
static void __attribute__((unused)) drm_mode_config_cleanup_list(ilist_node_t *list, void (*cleanup)(void *obj))
{
    ilist_node_t *node;
    ilist_node_t *next;

    node = list->next;
    while (node && node != list) {
        next = node->next;
        /* The drm_mode_object is the first member, so node == obj pointer */
        if (cleanup) { cleanup(node); }
        node = next;
    }

    ilist_init(list);
}

/*
 * drm_mode_config_cleanup - Tear down the mode configuration for a DRM device.
 * @dev: DRM device
 *
 * Cleans up all KMS objects in reverse-dependency order: framebuffers,
 * planes, CRTCs, connectors, encoders, properties, and blobs. Destroys
 * the IDR allocators. All allocated memory is released.
 */
void drm_mode_config_cleanup(struct drm_device *dev)
{
    if (!dev) { return; }

    /* Clean up framebuffers first (they reference GEM objects) */
    {
        ilist_node_t *node;
        ilist_node_t *next;

        node = dev->mode_config.fb_list.next;
        while (node && node != &dev->mode_config.fb_list) {
            next = node->next;
            {
                struct drm_framebuffer *fb = container_of(node, struct drm_framebuffer, head);
                drm_framebuffer_cleanup(fb);
                free(fb);
            }
            node = next;
        }
        ilist_init(&dev->mode_config.fb_list);
    }

    /* Clean up planes */
    {
        ilist_node_t *node;
        ilist_node_t *next;

        node = dev->mode_config.plane_list.next;
        while (node && node != &dev->mode_config.plane_list) {
            next = node->next;
            {
                struct drm_plane *plane = container_of(node, struct drm_plane, head);
                drm_plane_cleanup(plane);
            }
            node = next;
        }
        ilist_init(&dev->mode_config.plane_list);
    }

    /* Clean up CRTCs */
    {
        ilist_node_t *node;
        ilist_node_t *next;

        node = dev->mode_config.crtc_list.next;
        while (node && node != &dev->mode_config.crtc_list) {
            next = node->next;
            {
                struct drm_crtc *crtc = container_of(node, struct drm_crtc, head);
                drm_crtc_cleanup(crtc);
            }
            node = next;
        }
        ilist_init(&dev->mode_config.crtc_list);
    }

    /* Clean up connectors */
    {
        ilist_node_t *node;
        ilist_node_t *next;

        node = dev->mode_config.connector_list.next;
        while (node && node != &dev->mode_config.connector_list) {
            next = node->next;
            {
                struct drm_connector *connector = container_of(node, struct drm_connector, head);
                drm_connector_cleanup(connector);
            }
            node = next;
        }
        ilist_init(&dev->mode_config.connector_list);
    }

    /* Clean up encoders */
    {
        ilist_node_t *node;
        ilist_node_t *next;

        node = dev->mode_config.encoder_list.next;
        while (node && node != &dev->mode_config.encoder_list) {
            next = node->next;
            {
                struct drm_encoder *encoder = container_of(node, struct drm_encoder, head);
                drm_encoder_cleanup(encoder);
            }
            node = next;
        }
        ilist_init(&dev->mode_config.encoder_list);
    }

    /* Clean up properties */
    {
        ilist_node_t *node;
        ilist_node_t *next;

        node = dev->mode_config.property_list.next;
        while (node && node != &dev->mode_config.property_list) {
            next = node->next;
            {
                struct drm_property *prop = container_of(node, struct drm_property, dev_head);
                drm_property_destroy(dev, prop);
            }
            node = next;
        }
        ilist_init(&dev->mode_config.property_list);
    }

    /* Clean up property blobs */
    {
        ilist_node_t *node;
        ilist_node_t *next;

        node = dev->mode_config.property_blob_list.next;
        while (node && node != &dev->mode_config.property_blob_list) {
            next = node->next;
            {
                struct drm_property_blob *blob = container_of(node, struct drm_property_blob, head_global);
                drm_property_blob_put(blob);
            }
            node = next;
        }
        ilist_init(&dev->mode_config.property_blob_list);
    }

    drm_idr_destroy(&dev->mode_config.object_idr);
    drm_idr_destroy(&dev->mode_config.fb_idr);

    dev->mode_config.num_connector   = 0;
    dev->mode_config.num_encoder     = 0;
    dev->mode_config.num_crtc        = 0;
    dev->mode_config.num_plane       = 0;
    dev->mode_config.num_total_plane = 0;
    dev->mode_config.num_fb          = 0;
}

/*
 * drm_mode_getresources - Handle DRM_IOCTL_MODE_GETRESOURCES.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_card_res (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Fills the drm_mode_card_res struct with counts of framebuffers, CRTCs,
 * connectors, and encoders, and the min/max dimensions.
 * Returns 0 on success.
 */
int drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)data;

    (void)file_priv;

    if (!dev || !res) {
        plogk("drm: GETRESOURCES with invalid args (dev=%p, res=%p)\n", dev, res);
        return -EINVAL;
    }

    uint32_t  user_fbs = res->count_fbs, user_crtcs = res->count_crtcs;
    uint32_t  user_connectors = res->count_connectors, user_encoders = res->count_encoders;
    uint32_t *fbs = NULL, *crtcs = NULL, *connectors = NULL, *encoders = NULL;
    uint32_t  n;

    if (dev->mode_config.num_fb) fbs = malloc((size_t)dev->mode_config.num_fb * sizeof(*fbs));
    if (dev->mode_config.num_crtc) crtcs = malloc((size_t)dev->mode_config.num_crtc * sizeof(*crtcs));
    if (dev->mode_config.num_connector) connectors = malloc((size_t)dev->mode_config.num_connector * sizeof(*connectors));
    if (dev->mode_config.num_encoder) encoders = malloc((size_t)dev->mode_config.num_encoder * sizeof(*encoders));
    if ((dev->mode_config.num_fb && !fbs) || (dev->mode_config.num_crtc && !crtcs) || (dev->mode_config.num_connector && !connectors)
        || (dev->mode_config.num_encoder && !encoders)) {
        plogk("drm: GETRESOURCES allocation failed (num_fb=%d num_crtc=%d num_connector=%d num_encoder=%d), returning -ENOMEM.\n",
              dev->mode_config.num_fb, dev->mode_config.num_crtc, dev->mode_config.num_connector, dev->mode_config.num_encoder);
        free(fbs);
        free(crtcs);
        free(connectors);
        free(encoders);
        return -ENOMEM;
    }
    n = 0;
    if (fbs)
        for (ilist_node_t *node = dev->mode_config.fb_list.next; node != &dev->mode_config.fb_list; node = node->next)
            fbs[n++] = container_of(node, struct drm_framebuffer, head)->base.id;
    n = 0;
    if (crtcs)
        for (ilist_node_t *node = dev->mode_config.crtc_list.next; node != &dev->mode_config.crtc_list; node = node->next)
            crtcs[n++] = container_of(node, struct drm_crtc, head)->base.id;
    n = 0;
    if (connectors)
        for (ilist_node_t *node = dev->mode_config.connector_list.next; node != &dev->mode_config.connector_list; node = node->next)
            connectors[n++] = container_of(node, struct drm_connector, head)->base.id;
    n = 0;
    if (encoders)
        for (ilist_node_t *node = dev->mode_config.encoder_list.next; node != &dev->mode_config.encoder_list; node = node->next)
            encoders[n++] = container_of(node, struct drm_encoder, head)->base.id;

    if ((user_fbs && dev->mode_config.num_fb
         && (!res->fb_id_ptr
             || copy_to_user((void *)(uintptr_t)res->fb_id_ptr, fbs,
                             (size_t)(user_fbs < (uint32_t)dev->mode_config.num_fb ? user_fbs : (uint32_t)dev->mode_config.num_fb)
                                 * sizeof(*fbs))))
        || (user_crtcs && dev->mode_config.num_crtc
            && (!res->crtc_id_ptr
                || copy_to_user((void *)(uintptr_t)res->crtc_id_ptr, crtcs,
                                (size_t)(user_crtcs < (uint32_t)dev->mode_config.num_crtc ? user_crtcs : (uint32_t)dev->mode_config.num_crtc)
                                    * sizeof(*crtcs))))
        || (user_connectors && dev->mode_config.num_connector
            && (!res->connector_id_ptr
                || copy_to_user((void *)(uintptr_t)res->connector_id_ptr, connectors,
                                (size_t)(user_connectors < (uint32_t)dev->mode_config.num_connector ? user_connectors :
                                                                                                      (uint32_t)dev->mode_config.num_connector)
                                    * sizeof(*connectors))))
        || (user_encoders && dev->mode_config.num_encoder
            && (!res->encoder_id_ptr
                || copy_to_user(
                    (void *)(uintptr_t)res->encoder_id_ptr, encoders,
                    (size_t)(user_encoders < (uint32_t)dev->mode_config.num_encoder ? user_encoders : (uint32_t)dev->mode_config.num_encoder)
                        * sizeof(*encoders))))) {
        plogk("drm: GETRESOURCES copy_to_user failed (user_fbs=%u user_crtcs=%u user_connectors=%u user_encoders=%u), returning -EFAULT.\n",
              user_fbs, user_crtcs, user_connectors, user_encoders);
        free(fbs);
        free(crtcs);
        free(connectors);
        free(encoders);
        return -EFAULT;
    }
    free(fbs);
    free(crtcs);
    free(connectors);
    free(encoders);

    res->min_width        = dev->mode_config.min_width;
    res->max_width        = dev->mode_config.max_width;
    res->min_height       = dev->mode_config.min_height;
    res->max_height       = dev->mode_config.max_height;
    res->count_fbs        = (__u32)dev->mode_config.num_fb;
    res->count_crtcs      = (__u32)dev->mode_config.num_crtc;
    res->count_connectors = (__u32)dev->mode_config.num_connector;
    res->count_encoders   = (__u32)dev->mode_config.num_encoder;

    return 0;
}

/*
 * drmm_mode_config_init - Managed resource wrapper for drm_mode_config_init.
 * @dev: DRM device
 *
 * Calls drm_mode_config_init. In a full implementation this would register
 * a cleanup action with the device resource manager; MVP delegates to the
 * manual cleanup path. Returns 0 on success.
 */
static int drmm_mode_config_init(struct drm_device *dev)
{
    if (!dev) {
        plogk("drm: Drmm_mode_config_init called with NULL device.\n");
        return -EINVAL;
    }

    return drm_mode_config_init(dev);
}
