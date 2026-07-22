/*
 *
 *      drm_framebuffer.c
 *      DRM framebuffer management
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
 * drm_framebuffer_init - Initialise a framebuffer object.
 * @dev: DRM device
 * @fb: framebuffer to initialise
 * @funcs: framebuffer funcs pointer (unused in MVP, kept for API compat)
 *
 * Allocates a framebuffer-specific ID from the fb_idr, allocates a
 * mode-object ID for the base, inserts into the device fb_list, and
 * increments num_fb. Returns 0 on success or a negative errno.
 */
int drm_framebuffer_init(struct drm_device *dev, struct drm_framebuffer *fb,
                         void *funcs)
{
    uint32_t fb_id = 0;
    int ret;

    if (!dev || !fb) {
        return -EINVAL;
    }

    (void)funcs;

    spin_lock(&dev->mode_config.fb_lock);
    ret = drm_idr_alloc(&dev->mode_config.fb_idr, fb, 1, 0, &fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (ret) {
        return ret;
    }

    ret = drm_mode_object_idr_alloc(dev, &fb->base, DRM_MODE_OBJECT_FB);
    if (ret) {
        spin_lock(&dev->mode_config.fb_lock);
        drm_idr_remove(&dev->mode_config.fb_idr, fb_id);
        spin_unlock(&dev->mode_config.fb_lock);
        return ret;
    }

    fb->id = (int)fb_id;

    ilist_insert_after(&dev->mode_config.fb_list, &fb->head);

    dev->mode_config.num_fb++;

    return 0;
}

/*
 * drm_mode_addfb - Handle DRM_IOCTL_MODE_ADDFB (legacy).
 * @dev: DRM device
 * @data: pointer to struct drm_mode_fb_cmd (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Allocates a framebuffer struct, derives the fourcc format from bpp
 * and depth, sets width/height/pitch, initialises and registers the fb.
 * Returns 0 on success or -EINVAL/-ENOMEM.
 */
int drm_mode_addfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd *r = (struct drm_mode_fb_cmd *)data;
    struct drm_framebuffer *fb;
    uint32_t format;
    int ret;

    if (!dev || !r) {
        return -EINVAL;
    }

    (void)file_priv;

    /* Derive fourcc from bpp and depth (legacy compatibility) */
    if (r->bpp == 32 && r->depth == 24) {
        format = DRM_FORMAT_XRGB8888;
    } else if (r->bpp == 32 && r->depth == 32) {
        format = DRM_FORMAT_ARGB8888;
    } else if (r->bpp == 24 && r->depth == 24) {
        format = DRM_FORMAT_RGB888;
    } else if (r->bpp == 16 && r->depth == 16) {
        format = DRM_FORMAT_RGB565;
    } else if (r->bpp == 16 && r->depth == 15) {
        format = DRM_FORMAT_XRGB1555;
    } else if (r->bpp == 8 && r->depth == 8) {
        format = DRM_FORMAT_C8;
    } else {
        return -EINVAL;
    }

    fb = malloc(sizeof(*fb));
    if (!fb) {
        return -ENOMEM;
    }
    memset(fb, 0, sizeof(*fb));

    fb->format   = format;
    fb->modifier = DRM_FORMAT_MOD_LINEAR;
    fb->width    = r->width;
    fb->height   = r->height;
    fb->pitches[0] = r->pitch;
    fb->offsets[0] = 0;
    fb->pitches[1] = 0;
    fb->pitches[2] = 0;
    fb->pitches[3] = 0;
    fb->offsets[1] = 0;
    fb->offsets[2] = 0;
    fb->offsets[3] = 0;
    fb->hot_x = 0;
    fb->hot_y = 0;
    fb->file  = file_priv;

    ret = drm_framebuffer_init(dev, fb, NULL);
    if (ret) {
        free(fb);
        return ret;
    }

    r->fb_id = (__u32)fb->base.id;

    return 0;
}

/*
 * drm_mode_addfb2 - Handle DRM_IOCTL_MODE_ADDFB2.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_fb_cmd2 (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Allocates a framebuffer struct, copies the format, width, height,
 * pitches, offsets, handles, and modifiers from the UAPI struct,
 * initialises and registers the fb. Returns 0 on success or -EINVAL/-ENOMEM.
 */
int drm_mode_addfb2(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd2 *r = (struct drm_mode_fb_cmd2 *)data;
    struct drm_framebuffer *fb;
    int ret;
    int i;

    if (!dev || !r) {
        return -EINVAL;
    }

    (void)file_priv;

    if (r->pixel_format == DRM_FORMAT_INVALID) {
        return -EINVAL;
    }

    fb = malloc(sizeof(*fb));
    if (!fb) {
        return -ENOMEM;
    }
    memset(fb, 0, sizeof(*fb));

    fb->format   = r->pixel_format;
    fb->modifier = r->modifier[0];
    fb->width    = r->width;
    fb->height   = r->height;
    fb->file     = file_priv;

    for (i = 0; i < 4; i++) {
        fb->pitches[i] = r->pitches[i];
        fb->offsets[i] = r->offsets[i];
    }

    ret = drm_framebuffer_init(dev, fb, NULL);
    if (ret) {
        free(fb);
        return ret;
    }

    r->fb_id = (__u32)fb->base.id;

    return 0;
}

/*
 * drm_mode_rmfb - Handle DRM_IOCTL_MODE_RMFB.
 * @dev: DRM device
 * @data: pointer to fb_id (uint32_t)
 * @file_priv: DRM file handle
 *
 * Removes a framebuffer from the fb_idr and device fb_list, and frees it.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_rmfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    uint32_t fb_id = *(uint32_t *)data;
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (!dev || !data) {
        return -EINVAL;
    }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, fb_id);
    if (!fb) {
        spin_unlock(&dev->mode_config.fb_lock);
        return -ENOENT;
    }

    drm_idr_remove(&dev->mode_config.fb_idr, fb_id);
    spin_unlock(&dev->mode_config.fb_lock);

    ilist_remove(&fb->head);

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, fb->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    if (dev->mode_config.num_fb > 0) {
        dev->mode_config.num_fb--;
    }

    free(fb);

    return 0;
}

/*
 * drm_mode_getfb - Handle DRM_IOCTL_MODE_GETFB (legacy).
 * @dev: DRM device
 * @data: pointer to struct drm_mode_fb_cmd (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up the framebuffer by fb_id, fills width, height, pitch, bpp,
 * and depth fields. Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_getfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd *r = (struct drm_mode_fb_cmd *)data;
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (!dev || !r) {
        return -EINVAL;
    }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, r->fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (!fb) {
        return -ENOENT;
    }

    r->width  = fb->width;
    r->height = fb->height;
    r->pitch  = fb->pitches[0];

    /* Derive bpp/depth from fourcc (legacy compatibility) */
    switch (fb->format) {
    case DRM_FORMAT_XRGB8888:
        r->bpp   = 32;
        r->depth = 24;
        break;
    case DRM_FORMAT_ARGB8888:
        r->bpp   = 32;
        r->depth = 32;
        break;
    case DRM_FORMAT_RGB888:
        r->bpp   = 24;
        r->depth = 24;
        break;
    case DRM_FORMAT_RGB565:
        r->bpp   = 16;
        r->depth = 16;
        break;
    case DRM_FORMAT_XRGB1555:
        r->bpp   = 16;
        r->depth = 15;
        break;
    case DRM_FORMAT_C8:
        r->bpp   = 8;
        r->depth = 8;
        break;
    default:
        r->bpp   = 32;
        r->depth = 24;
        break;
    }

    r->handle = 0; /* MVP: GEM handle lookup not yet wired */

    return 0;
}

/*
 * drm_mode_dirtyfb - Handle DRM_IOCTL_MODE_DIRTYFB.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_fb_dirty_cmd (userspace buffer)
 * @file_priv: DRM file handle
 *
 * MVP stub: validates the fb_id exists, then returns 0.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_dirtyfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_dirty_cmd *r = (struct drm_mode_fb_dirty_cmd *)data;
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (!dev || !r) {
        return -EINVAL;
    }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, r->fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (!fb) {
        return -ENOENT;
    }

    /* MVP: dirty rectangle tracking not yet implemented */
    return 0;
}

/*
 * drm_mode_getfb2_ioctl - Handle DRM_IOCTL_MODE_GETFB2.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_get_fb2 (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Looks up a framebuffer by fb_id and fills in its properties.
 * Returns 0 on success or -EINVAL/-ENOENT.
 */
int drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_fb2 *r = (struct drm_mode_get_fb2 *)data;
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (!dev || !r) {
        return -EINVAL;
    }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, r->fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (!fb) {
        return -ENOENT;
    }

    r->width        = fb->width;
    r->height       = fb->height;
    r->pixel_format = fb->format;
    r->flags        = 0;
    r->modifier[0]  = fb->modifier;
    r->pitches[0]   = fb->pitches[0];
    r->offsets[0]   = fb->offsets[0];

    return 0;
}

/*
 * drm_framebuffer_cleanup - Tear down a framebuffer and release resources.
 * @fb: framebuffer to clean up
 *
 * Removes the framebuffer from the device fb_list, removes it from both
 * the fb_idr and the global object IDR, and decrements num_fb.
 * Does NOT free the struct; the caller owns that.
 */
void drm_framebuffer_cleanup(struct drm_framebuffer *fb)
{
    struct drm_device *dev;

    if (!fb) {
        return;
    }

    dev = fb->base.dev;

    ilist_remove(&fb->head);

    if (dev) {
        spin_lock(&dev->mode_config.fb_lock);
        drm_idr_remove(&dev->mode_config.fb_idr, (uint32_t)fb->id);
        spin_unlock(&dev->mode_config.fb_lock);

        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, fb->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_fb > 0) {
            dev->mode_config.num_fb--;
        }
    }
}

/*
 * drm_framebuffer_lookup - Look up a framebuffer by ID.
 * @dev: DRM device
 * @file_priv: DRM file handle (unused)
 * @id: framebuffer ID
 *
 * Returns the framebuffer pointer or NULL if not found.
 * The caller does NOT receive an extra reference.
 */
struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev,
                                                struct drm_file *file_priv,
                                                uint32_t id)
{
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (!dev) {
        return NULL;
    }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, id);
    spin_unlock(&dev->mode_config.fb_lock);

    return fb;
}