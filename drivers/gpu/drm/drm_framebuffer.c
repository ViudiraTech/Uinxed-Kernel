/*
 *
 *      drm_framebuffer.c
 *      DRM framebuffer management
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
int drm_framebuffer_init(struct drm_device *dev, struct drm_framebuffer *fb, const struct drm_framebuffer_funcs *funcs)
{
    uint32_t fb_id = 0;
    int      ret;

    if (!dev || !fb) { return -EINVAL; }

    fb->funcs = funcs;

    spin_lock(&dev->mode_config.fb_lock);
    ret = drm_idr_alloc(&dev->mode_config.fb_idr, fb, 1, 0, &fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (ret) { return ret; }

    ret = drm_mode_object_idr_alloc(dev, &fb->base, DRM_MODE_OBJECT_FB);
    if (ret) {
        spin_lock(&dev->mode_config.fb_lock);
        drm_idr_remove(&dev->mode_config.fb_idr, fb_id);
        spin_unlock(&dev->mode_config.fb_lock);
        return ret;
    }

    fb->id = (int)fb_id;

    ilist_insert_after(&dev->mode_config.fb_list, &fb->head);
    if (fb->file) ilist_insert_after(&fb->file->fbs_head, &fb->filp_head);

    dev->mode_config.num_fb++;

    return 0;
}

/*
 * drm_mode_addfb - Handle DRM_IOCTL_MODE_ADDFB (legacy).
 * @dev: DRM device
 * @data: pointer to struct drm_mode_fb_cmd (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Derives the fourcc format from bpp/depth, looks up the GEM object
 * backing the buffer, validates pitch/size, and registers the fb.
 * Returns 0 on success or -EINVAL/-ENOMEM.
 */
int drm_mode_addfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd *r = (struct drm_mode_fb_cmd *)data;
    struct drm_framebuffer *fb;
    struct drm_gem_object  *obj;
    uint32_t                format;
    uint32_t                bpp_bytes;
    uint32_t                min_pitch;
    int                     ret;

    if (!dev || !r) { return -EINVAL; }

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

    /* Validate dimensions against mode_config limits */
    if (r->width == 0 || r->height == 0) { return -EINVAL; }
    if (r->width > dev->mode_config.max_width || r->height > dev->mode_config.max_height) { return -EINVAL; }
    if (r->handle == 0) { return -EINVAL; }

    /* Validate pitch: must be >= width * bytes_per_pixel */
    bpp_bytes = r->bpp / 8;
    min_pitch = r->width * bpp_bytes;
    if (r->pitch < min_pitch) { return -EINVAL; }

    /* Look up the GEM object by handle */
    obj = drm_gem_object_lookup(file_priv, r->handle);
    if (!obj) { return -ENOENT; }
    /* Verify the backing object is large enough */
    if (obj->size < (size_t)r->pitch * r->height) {
        drm_gem_object_put(obj);
        return -EINVAL;
    }

    fb = malloc(sizeof(*fb));
    if (!fb) {
        if (obj) drm_gem_object_put(obj);
        return -ENOMEM;
    }
    memset(fb, 0, sizeof(*fb));

    fb->format     = format;
    fb->modifier   = DRM_FORMAT_MOD_LINEAR;
    fb->width      = r->width;
    fb->height     = r->height;
    fb->pitches[0] = r->pitch;
    fb->offsets[0] = 0;
    fb->pitches[1] = 0;
    fb->pitches[2] = 0;
    fb->pitches[3] = 0;
    fb->offsets[1] = 0;
    fb->offsets[2] = 0;
    fb->offsets[3] = 0;
    fb->hot_x      = 0;
    fb->hot_y      = 0;
    fb->obj[0]     = obj;
    fb->file       = file_priv;

    ret = drm_framebuffer_init(dev, fb, dev->driver ? dev->driver->fb_funcs : NULL);
    if (ret) {
        if (obj) drm_gem_object_put(obj);
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
 * Looks up GEM objects for each plane handle, validates pitch/size
 * per plane, and registers the fb. Returns 0 on success or -EINVAL/-ENOMEM.
 */
int drm_mode_addfb2(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd2 *r = (struct drm_mode_fb_cmd2 *)data;
    struct drm_framebuffer  *fb;
    struct drm_gem_object   *obj;
    int                      ret;
    int                      i;
    int                      num_planes;
    uint32_t                 min_pitch;

    if (!dev || !r) { return -EINVAL; }

    if (r->pixel_format == DRM_FORMAT_INVALID) { return -EINVAL; }
    if (r->flags & ~(DRM_MODE_FB_INTERLACED | DRM_MODE_FB_MODIFIERS)) return -EINVAL;

    /* This driver intentionally exposes the same scanout formats as Linux
     * virtgpu's 2D plane.  Rejecting unsupported layouts is safer than
     * creating a framebuffer that the host will later misinterpret. */
    if (r->pixel_format != DRM_FORMAT_XRGB8888 && r->pixel_format != DRM_FORMAT_ARGB8888) return -EINVAL;

    /* Validate dimensions against mode_config limits */
    if (r->width == 0 || r->height == 0) { return -EINVAL; }
    if (r->width > dev->mode_config.max_width || r->height > dev->mode_config.max_height) { return -EINVAL; }

    num_planes = 1;
    min_pitch = r->width * 4;
    if (r->pitches[0] < min_pitch) return -EINVAL;
    if (r->flags & DRM_MODE_FB_MODIFIERS) {
        if (r->modifier[0] != DRM_FORMAT_MOD_LINEAR) return -EINVAL;
        for (i = 1; i < 4; i++) {
            if (r->handles[i] || r->pitches[i] || r->offsets[i] || r->modifier[i]) return -EINVAL;
        }
    } else {
        /* Linux treats modifier[] as ignored unless the flag is set. */
        r->modifier[0] = DRM_FORMAT_MOD_LINEAR;
    }

    fb = malloc(sizeof(*fb));
    if (!fb) { return -ENOMEM; }
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

    /* Look up GEM objects for each plane and validate */
    for (i = 0; i < num_planes; i++) {
        uint32_t handle = r->handles[i];

        if (handle == 0) {
            ret = -EINVAL;
            goto err_cleanup;
        }

        obj = drm_gem_object_lookup(file_priv, handle);
        if (!obj) {
            ret = -ENOENT;
            goto err_cleanup;
        }

        /* Include the plane offset and protect the arithmetic from wrap. */
        if (r->offsets[i] > obj->size || (r->height > 0
            && ((uint64_t)r->pitches[i] * (r->height - 1) + min_pitch > obj->size - r->offsets[i]))) {
            drm_gem_object_put(obj);
            ret = -EINVAL;
            goto err_cleanup;
        }

        fb->obj[i] = obj;
    }

    ret = drm_framebuffer_init(dev, fb, dev->driver ? dev->driver->fb_funcs : NULL);
    if (ret) { goto err_cleanup; }

    r->fb_id = (__u32)fb->base.id;

    return 0;

err_cleanup:
    for (i = 0; i < 4; i++) {
        if (fb->obj[i]) {
            drm_gem_object_put(fb->obj[i]);
            fb->obj[i] = NULL;
        }
    }
    free(fb);
    return ret;
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
    uint32_t                fb_id = *(uint32_t *)data;
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (!dev || !data) { return -EINVAL; }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, fb_id);
    if (!fb) {
        spin_unlock(&dev->mode_config.fb_lock);
        return -ENOENT;
    }

    spin_unlock(&dev->mode_config.fb_lock);

    for (ilist_node_t *node = dev->mode_config.plane_list.next; node != &dev->mode_config.plane_list; node = node->next) {
        struct drm_plane *plane = container_of(node, struct drm_plane, head);
        if (!plane->state || plane->state->fb != fb) continue;
        if (plane->state->crtc && plane == plane->state->crtc->primary) {
            struct drm_crtc *crtc = plane->state->crtc;
            struct drm_crtc_helper_funcs *helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (!helpers || !helpers->page_flip) return -EBUSY;
            int ret = helpers->page_flip(crtc, NULL, NULL, 0);
            if (ret) return ret;
        }
        plane->state->fb = NULL;
        plane->state->crtc = NULL;
        plane->fb_id = 0;
        plane->crtc_id = 0;
    }
    drm_framebuffer_cleanup(fb);
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

    if (!dev || !r) { return -EINVAL; }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, r->fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (!fb) { return -ENOENT; }

    r->width  = fb->width;
    r->height = fb->height;
    r->pitch  = fb->pitches[0];

    /* Derive bpp/depth from fourcc (legacy compatibility) */
    switch (fb->format) {
        case DRM_FORMAT_XRGB8888 :
            r->bpp   = 32;
            r->depth = 24;
            break;
        case DRM_FORMAT_ARGB8888 :
            r->bpp   = 32;
            r->depth = 32;
            break;
        case DRM_FORMAT_RGB888 :
            r->bpp   = 24;
            r->depth = 24;
            break;
        case DRM_FORMAT_RGB565 :
            r->bpp   = 16;
            r->depth = 16;
            break;
        case DRM_FORMAT_XRGB1555 :
            r->bpp   = 16;
            r->depth = 15;
            break;
        case DRM_FORMAT_C8 :
            r->bpp   = 8;
            r->depth = 8;
            break;
        default :
            r->bpp   = 32;
            r->depth = 24;
            break;
    }

    r->handle = 0;
    if (fb->obj[0]) {
        if (drm_gem_handle_create(file_priv, fb->obj[0], &r->handle)) {
            return -ENOMEM;
        }
    }

    return 0;
}

/*
 * drm_mode_dirtyfb - Handle DRM_IOCTL_MODE_DIRTYFB.
 * @dev: DRM device
 * @data: pointer to struct drm_mode_fb_dirty_cmd (userspace buffer)
 * @file_priv: DRM file handle
 *
 * Mirrors Linux DRM UAPI validation and passes annotations through to
 * the framebuffer's dirty callback unchanged.
 */
int drm_mode_dirtyfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_dirty_cmd *r = (struct drm_mode_fb_dirty_cmd *)data;
    struct drm_framebuffer       *fb;
    struct drm_clip_rect         *clips = NULL;
    unsigned int                 flags;
    int                          ret = 0;

    if (!dev || !r) { return -EINVAL; }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, r->fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (!fb) { return -ENOENT; }

    if ((!r->num_clips) != (!r->clips_ptr)) return -EINVAL;

    flags = r->flags & DRM_MODE_FB_DIRTY_FLAGS;
    if ((flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) && (r->num_clips & 1U)) return -EINVAL;

    if (r->num_clips) {
        if (r->num_clips > DRM_MODE_FB_DIRTY_MAX_CLIPS) return -EINVAL;
        clips = malloc((size_t)r->num_clips * sizeof(*clips));
        if (!clips) return -ENOMEM;
        if (copy_from_user(clips, (const void *)(uintptr_t)r->clips_ptr, (size_t)r->num_clips * sizeof(*clips))) {
            free(clips);
            return -EFAULT;
        }

        if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) {
            for (uint32_t i = 0; i < r->num_clips; i += 2) {
                unsigned int src_w = clips[i].x2 - clips[i].x1;
                unsigned int src_h = clips[i].y2 - clips[i].y1;
                unsigned int dst_w = clips[i + 1].x2 - clips[i + 1].x1;
                unsigned int dst_h = clips[i + 1].y2 - clips[i + 1].y1;

                if (clips[i].x2 < clips[i].x1 || clips[i].y2 < clips[i].y1 || clips[i + 1].x2 < clips[i + 1].x1
                    || clips[i + 1].y2 < clips[i + 1].y1 || src_w != dst_w || src_h != dst_h) {
                    free(clips);
                    return -EINVAL;
                }
            }
        }
    }

    if (fb->funcs && fb->funcs->dirty) {
        ret = fb->funcs->dirty(fb, file_priv, flags, r->color, clips, r->num_clips);
    } else {
        ret = -ENOSYS;
    }
    free(clips);
    return ret;
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
    struct drm_framebuffer  *fb;

    if (!dev || !r) { return -EINVAL; }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, r->fb_id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (!fb) { return -ENOENT; }

    r->width        = fb->width;
    r->height       = fb->height;
    r->pixel_format = fb->format;
    r->flags        = DRM_MODE_FB_MODIFIERS;
    for (int i = 0; i < 4; i++) {
        r->handles[i] = 0;
        r->modifier[i] = i == 0 ? fb->modifier : 0;
        r->pitches[i] = fb->pitches[i];
        r->offsets[i] = fb->offsets[i];
        if (fb->obj[i]) {
            if (drm_gem_handle_create(file_priv, fb->obj[i], &r->handles[i])) {
                for (int j = 0; j < i; j++) if (r->handles[j]) drm_gem_handle_delete(file_priv, r->handles[j]);
                return -ENOMEM;
            }
        }
    }

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
    int                i;

    if (!fb) { return; }

    dev = fb->base.dev;

    /* Release references to GEM backing objects */
    for (i = 0; i < 4; i++) {
        if (fb->obj[i]) {
            drm_gem_object_put(fb->obj[i]);
            fb->obj[i] = NULL;
        }
    }

    ilist_remove(&fb->head);
    if (fb->file) {
        ilist_remove(&fb->filp_head);
        fb->file = NULL;
    }

    if (dev) {
        spin_lock(&dev->mode_config.fb_lock);
        drm_idr_remove(&dev->mode_config.fb_idr, (uint32_t)fb->id);
        spin_unlock(&dev->mode_config.fb_lock);

        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, fb->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_fb > 0) { dev->mode_config.num_fb--; }
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
struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev, struct drm_file *file_priv, uint32_t id)
{
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (!dev) { return NULL; }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, id);
    spin_unlock(&dev->mode_config.fb_lock);

    return fb;
}
