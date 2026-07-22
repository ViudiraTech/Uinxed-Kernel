/*
 *
 *      drm_encoder.c
 *      DRM encoder management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_device.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_idr.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <alloc.h>
#include <errno.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Internal helper from drm_mode_object.c */
extern int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

/*
 * drm_encoder_init - Initialise an encoder object.
 * @dev: DRM device
 * @encoder: encoder to initialise
 * @funcs: encoder helper funcs pointer (stored in helper_private)
 * @encoder_type: DRM_MODE_ENCODER_* type
 * @name: name of the encoder (unused in MVP, kept for API compatibility)
 *
 * Allocates a mode-object ID, inserts into the device encoder list,
 * and sets the encoder type. Returns 0 on success or a negative errno.
 */
int drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder,
                     void *funcs, int encoder_type, const char *name)
{
    int ret;

    (void)name;

    if (!dev || !encoder) {
        return -EINVAL;
    }

    ret = drm_mode_object_idr_alloc(dev, &encoder->base, DRM_MODE_OBJECT_ENCODER);
    if (ret) {
        return ret;
    }

    ilist_insert_after(&dev->mode_config.encoder_list, &encoder->head);

    encoder->dev            = dev;
    encoder->encoder_type   = (uint32_t)encoder_type;
    encoder->possible_crtcs = 0;
    encoder->possible_clones = 0;
    encoder->crtc           = NULL;
    encoder->helper_private  = funcs;

    dev->mode_config.num_encoder++;

    return 0;
}

/*
 * drm_mode_getencoder - Handle DRM_IOCTL_MODE_GETENCODER.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_get_encoder (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up the encoder by id, fills the struct with encoder type,
 * attached CRTC id, possible_crtcs, and possible_clones.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_getencoder(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_encoder *enc_req = (struct drm_mode_get_encoder *)data;
    struct drm_mode_object *obj;
    struct drm_encoder *encoder;

    if (!dev || !enc_req) {
        return -EINVAL;
    }

    obj = drm_mode_object_find(dev, file_priv, enc_req->encoder_id, DRM_MODE_OBJECT_ENCODER);
    if (!obj) {
        return -ENOENT;
    }
    encoder = container_of(obj, struct drm_encoder, base);

    enc_req->encoder_type   = encoder->encoder_type;
    enc_req->crtc_id         = encoder->crtc ? encoder->crtc->base.id : 0;
    enc_req->possible_crtcs  = encoder->possible_crtcs;
    enc_req->possible_clones = encoder->possible_clones;

    drm_mode_object_put(obj);
    return 0;
}

/*
 * drm_encoder_cleanup - Tear down an encoder and release its resources.
 * @encoder: encoder to clean up
 *
 * Removes the encoder from the device encoder list, removes it from
 * the global IDR, and decrements num_encoder.
 */
void drm_encoder_cleanup(struct drm_encoder *encoder)
{
    struct drm_device *dev;

    if (!encoder) {
        return;
    }

    dev = encoder->dev;

    ilist_remove(&encoder->head);

    if (dev) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, encoder->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_encoder > 0) {
            dev->mode_config.num_encoder--;
        }
    }
}