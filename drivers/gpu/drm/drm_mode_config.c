/*
 *
 *      drm_mode_config.c
 *      DRM mode configuration initialisation and cleanup
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
#include <sync/spin_lock.h>

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Internal helpers from drm_property.c */
extern void drm_property_destroy(struct drm_device *dev, struct drm_property *property);
extern void drm_property_blob_put(struct drm_property_blob *blob);

/* Forward declarations of cleanup functions from sibling compilation units. */
extern void drm_crtc_cleanup(struct drm_crtc *crtc);
extern void drm_connector_cleanup(struct drm_connector *connector);
extern void drm_encoder_cleanup(struct drm_encoder *encoder);
extern void drm_plane_cleanup(struct drm_plane *plane);
extern void drm_framebuffer_cleanup(struct drm_framebuffer *fb);

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
    if (!dev) { return -EINVAL; }

    memset(&dev->mode_config.mutex, 0, sizeof(dev->mode_config.mutex));
    memset(&dev->mode_config.idr_mutex, 0, sizeof(dev->mode_config.idr_mutex));
    memset(&dev->mode_config.fb_lock, 0, sizeof(dev->mode_config.fb_lock));
    memset(&dev->mode_config.blob_lock, 0, sizeof(dev->mode_config.blob_lock));

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

    if (!dev || !res) { return -EINVAL; }

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
int drmm_mode_config_init(struct drm_device *dev)
{
    if (!dev) { return -EINVAL; }

    return drm_mode_config_init(dev);
}