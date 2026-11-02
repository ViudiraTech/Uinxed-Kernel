/*
 *
 *      virtgpu_kms.c
 *      VirtIO-GPU KMS display pipeline
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <drivers/gpu/drm/drm_edid.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/virtio/virtgpu_drv.h>
#include <drivers/gpu/drm/virtio/virtgpu_kms.h>
#include <drivers/gpu/fbdev/video.h>

#if CONFIG_VIRTIO_GPU && CONFIG_DRM && CONFIG_VIRTIO_PCI

/* Static context for the DRM-flush callback (set during initial modeset) */
static struct virtio_gpu_device *vgdev_flush_ctx;
static struct virtio_gpu_object *vgdev_flush_obj;
static raw_spinlock_t            vgdev_flush_lock;

static enum drm_connector_status virtgpu_connector_detect(struct drm_connector *connector, bool force);
static int                       virtgpu_connector_get_modes(struct drm_connector *connector);
static int                       virtgpu_connector_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode);
static uint64_t                  virtgpu_mode_area(const struct drm_display_mode *mode);
static void                      virtgpu_encoder_atomic_check(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state, struct drm_connector_state *conn_state);
static int                       virtgpu_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_framebuffer *fb);
static void                      virtgpu_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state);
static int                       virtgpu_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state);
static int                       virtgpu_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb, struct drm_pending_vblank_event *event, uint32_t flags);
static int                       virtgpu_crtc_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *gem, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y);
static int                       virtgpu_crtc_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y);
static int                       virtgpu_plane_atomic_check(struct drm_plane *plane, struct drm_plane_state *state);

static const uint32_t virtgpu_formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
};

static const struct drm_plane_helper_funcs virtgpu_plane_helpers = {
    .atomic_check = virtgpu_plane_atomic_check,
};

static const struct drm_crtc_helper_funcs virtgpu_crtc_helpers = {
    .mode_set       = virtgpu_crtc_atomic_flush,
    .page_flip      = virtgpu_crtc_page_flip,
    .cursor_set     = virtgpu_crtc_cursor_set,
    .cursor_move    = virtgpu_crtc_cursor_move,
    .atomic_enable  = virtgpu_crtc_atomic_enable,
    .atomic_disable = virtgpu_crtc_atomic_disable,
};

static const struct drm_encoder_helper_funcs virtgpu_enc_helpers = {
    .atomic_mode_set = virtgpu_encoder_atomic_check,
};

static const struct drm_connector_helper_funcs virtgpu_conn_helpers = {
    .detect     = virtgpu_connector_detect,
    .get_modes  = virtgpu_connector_get_modes,
    .mode_valid = virtgpu_connector_mode_valid,
};

/* Reject modes and layouts the 2D scanout cannot scale or address. */
static int virtgpu_plane_atomic_check(struct drm_plane *plane, struct drm_plane_state *state)
{
    struct drm_crtc_state    *crtc_state = state->crtc_state;
    struct drm_framebuffer   *fb         = state->fb;
    struct virtio_gpu_object *obj;

    if (!fb) return 0;

    /* The host scanout takes the framebuffer size as the output size; no scaler. */
    if (crtc_state && crtc_state->active && ((uint32_t)crtc_state->mode.hdisplay != fb->width || (uint32_t)crtc_state->mode.vdisplay != fb->height)) return -EINVAL;

    /* The host scanout cannot crop or pan; the committed view must cover the whole buffer. */
    if (!drm_plane_state_full_frame(fb, state)) return -EINVAL;

    /* A blob carries its own layout; a 2D resource is scanned out through its packed stride. */
    obj = (fb->obj[0] && fb->obj[0]->dev == plane->dev) ? to_virtio_gpu_object(fb->obj[0]) : NULL;
    if (!obj) return -EINVAL;
    if (!obj->created_blob && (fb->width > obj->width || fb->height > obj->height || fb->pitches[0] != obj->stride || fb->offsets[0] || !virtgpu_2d_formats_compatible(obj->format, fb->format)))
        return -EINVAL;

    return 0;
}

/* Panic-time flush guard: skip while an ordinary control submission is active. */
static bool virtgpu_display_flush_guard(void)
{
    struct virtio_gpu_device *vgdev = vgdev_flush_ctx;
    if (!vgdev) return true;
    return !mutex_is_locked(&vgdev->ctrlq_cmd_lock);
}

/* Report connection status: a VM GPU is present whenever scanouts exist. */
static enum drm_connector_status virtgpu_connector_detect(struct drm_connector *connector, bool force)
{
    struct drm_device        *dev   = connector->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    (void)force;

    /* VirtIO-GPU is always connected in a VM */
    return vgdev->num_scanouts > 0 ? connector_status_connected : connector_status_disconnected;
}

/* Publish the host EDID and its modes; returns modes added, 0 when none, negative on a read fault. */
static int virtgpu_connector_get_edid_modes(struct drm_connector *connector)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)connector->dev->dev_private;
    struct edid              *edid;
    ilist_node_t             *node;
    int                       edid_size = 0;
    int                       count     = 0;
    int                       ret;

    vgdev->edid_mode_count = 0;
    if (!vgdev->has_edid) return 0;

    /* The host EDID response is capped at 1024 bytes by the virtio protocol. */
    edid = malloc(1024);
    if (!edid) return -ENOMEM;

    ret = virtgpu_cmd_get_edid(vgdev, 0, edid, &edid_size);
    if (ret) {
        free(edid);
        return ret;
    }
    if (edid_size <= 0) {
        free(edid);
        return 0;
    }

    /* A partial EDID is a malformed reply, not an absent display. */
    if (edid_size < EDID_LENGTH) {
        free(edid);
        return -EIO;
    }

    /* Reject a truncated EDID whose header overstates its size. */
    if ((size_t)((unsigned)edid->extensions + 1) * EDID_LENGTH > (size_t)edid_size) {
        free(edid);
        return -EIO;
    }

    if (!drm_edid_is_valid(edid)) {
        free(edid);
        return -EINVAL;
    }

    /* Publish the raw EDID as the connector's EDID property blob. */
    drm_connector_update_edid_property(connector, (const unsigned char *)edid, (size_t)edid_size);
    if (connector->edid_blob && connector->edid_blob->data) {
        /* The blob owns a persistent copy of the EDID bytes. */
        connector->edid_blob_ptr = (uint8_t *)connector->edid_blob->data;
    }

    /* Fill in the connector display info derived from the EDID. */
    drm_edid_to_display_info(connector, edid);
    count = drm_add_edid_modes(connector, edid);

    /* Keep a flagged timing first so the cap cannot drop the monitor's preferred mode. */
    for (node = connector->modes.next; node && node != &connector->modes && vgdev->edid_mode_count < VIRTGPU_MAX_EDID_MODES; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        if (mode->type & DRM_MODE_TYPE_PREFERRED) vgdev->edid_modes[vgdev->edid_mode_count++] = mode;
    }
    for (node = connector->modes.next; node && node != &connector->modes && vgdev->edid_mode_count < VIRTGPU_MAX_EDID_MODES; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        if (!(mode->type & DRM_MODE_TYPE_PREFERRED)) vgdev->edid_modes[vgdev->edid_mode_count++] = mode;
    }
    if (count > 0) {
        char name[14];

        drm_edid_get_monitor_name(edid, name, sizeof(name));
        DRM_INFO("Connector: EDID parsed: %d mode(s), monitor=\"%s\", %ux%u mm.\n", count, name[0] ? name : "unknown", connector->display_info_width_mm, connector->display_info_height_mm);
    }

    free(edid);
    return count;
}

/* Describe @width x @height with a synthetic timing. */
static void virtgpu_mode_fill(struct drm_display_mode *mode, uint32_t width, uint32_t height, uint32_t vrefresh)
{
    memset(mode, 0, sizeof(*mode));

    (void)snprintf(mode->name, DRM_DISPLAY_MODE_LEN - 1, "%ux%u", (unsigned)width, (unsigned)height);
    mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
    mode->hdisplay                       = (int)width;
    mode->hsync_start                    = (int)width + 80;
    mode->hsync_end                      = (int)width + 160;
    mode->htotal                         = (int)width + 320;
    mode->vdisplay                       = (int)height;
    mode->vsync_start                    = (int)height + 3;
    mode->vsync_end                      = (int)height + 6;
    mode->vtotal                         = (int)height + 32;
    mode->vrefresh                       = (int)(vrefresh ? vrefresh : 60);

    /* Pixel clock includes blanking totals, so userspace derives the refresh. */
    mode->clock  = (int)((uint64_t)mode->htotal * (uint64_t)mode->vtotal * (uint64_t)mode->vrefresh / 1000ULL);
    mode->flags  = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type   = DRM_MODE_TYPE_DRIVER;
    mode->status = MODE_OK;
}

/* Make @preferred the only DRM_MODE_TYPE_PREFERRED mode of the connector. */
static void virtgpu_connector_set_preferred(struct drm_connector *connector, struct drm_display_mode *preferred)
{
    ilist_node_t *node;

    for (node = connector->modes.next; node && node != &connector->modes; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        if (mode == preferred) {
            mode->type |= DRM_MODE_TYPE_PREFERRED;
        } else {
            mode->type &= ~DRM_MODE_TYPE_PREFERRED;
        }
    }
}

/* Fill the connector mode list from host display info or EDID. */
static int virtgpu_connector_get_modes(struct drm_connector *connector)
{
    struct drm_device        *dev            = connector->dev;
    struct virtio_gpu_device *vgdev          = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_display_mode  *edid_preferred = NULL;
    ilist_node_t             *node;
    bool                      host_preferred = false;
    int                       edid_modes;
    int                       i;

    /* virtio-gpu has no hotplug: probe the EDID/scanout modes once. */
    if (vgdev->modes_probed) return 0;

    /* Query display info from host if not already done */
    if (vgdev->num_scanouts == 0) virtgpu_cmd_get_display_info(vgdev);

    /* The mode pointers die with the list rebuilt below. */
    vgdev->edid_mode_count = 0;

    for (node = connector->modes.next; node != &connector->modes;) {
        struct drm_display_mode *m = container_of(node, struct drm_display_mode, head);

        node = node->next;
        drm_mode_destroy(dev, m);
    }

    /* Publish EDID modes (preferred first) plus the host scanout modes. */
    edid_modes = virtgpu_connector_get_edid_modes(connector);
    if (edid_modes < 0) DRM_ERROR("connector %s: failed to read EDID (%d), continuing without it.\n", connector->name, edid_modes);

    for (i = 0; i < vgdev->edid_mode_count; i++) {
        if (vgdev->edid_modes[i]->type & DRM_MODE_TYPE_PREFERRED) {
            edid_preferred = vgdev->edid_modes[i];
            break;
        }
    }

    /* Without a flagged preference the console leads with the largest EDID mode. */
    if (!edid_preferred) {
        for (i = 0; i < vgdev->edid_mode_count; i++)
            if (!edid_preferred || virtgpu_mode_area(vgdev->edid_modes[i]) > virtgpu_mode_area(edid_preferred)) edid_preferred = vgdev->edid_modes[i];
    }

    for (i = 0; i < vgdev->num_scanouts && i < VIRTGPU_MAX_SCANOUTS; i++) {
        struct virtio_gpu_display_mode *dm = &vgdev->scanouts[i];
        struct drm_display_mode        *mode;
        if (!dm->enabled) continue;
        if (dm->width <= 0 || dm->height <= 0 || dm->width > 8192 || dm->height > 8192) {
            plogk("virtgpu: scanout %d: host reports invalid geometry %dx%d, skipped.\n", i, dm->width, dm->height);
            continue;
        }

        mode = drm_mode_create(dev);
        if (!mode) {
            DRM_ERROR("out of memory allocating display mode.\n");
            return -ENOMEM;
        }
        virtgpu_mode_fill(mode, (uint32_t)dm->width, (uint32_t)dm->height, (uint32_t)dm->vrefresh);

        /* The host scanout leads only when the EDID offers no mode. */
        if (!edid_preferred && !host_preferred) {
            mode->type |= DRM_MODE_TYPE_PREFERRED;
            host_preferred = true;
        }
        drm_mode_probed_add(connector, mode);
    }

    /* Fallback: DRM default mode if the host reported no display info. */
    if (vgdev->num_scanouts == 0) {
        if (drm_connector_add_fallback_mode(connector)) {
            DRM_ERROR("out of memory adding fallback mode.\n");
            return -ENOMEM;
        }

        /* The synthesized scanout only keeps the connector present; the boot chain skips it. */
        vgdev->num_scanouts         = 1;
        vgdev->scanouts[0].enabled  = false;
        vgdev->scanouts[0].width    = CONFIG_DRM_DEFAULT_WIDTH;
        vgdev->scanouts[0].height   = CONFIG_DRM_DEFAULT_HEIGHT;
        vgdev->scanouts[0].vrefresh = 60;
    }
    if (edid_preferred) virtgpu_connector_set_preferred(connector, edid_preferred);

    vgdev->modes_probed = true;
    return 0;
}

/* Accept any mode within the host's 8192x8192 scanout limit. */
static int virtgpu_connector_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode)
{
    (void)connector;

    /* Accept all modes up to 8192x8192 */
    if (mode->hdisplay <= 0 || mode->vdisplay <= 0 || mode->hdisplay > 8192 || mode->vdisplay > 8192) return MODE_BAD;
    return MODE_OK;
}

/* The encoder imposes no extra atomic constraints. */
static void virtgpu_encoder_atomic_check(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state, struct drm_connector_state *conn_state)
{
    (void)encoder;
    (void)crtc_state;
    (void)conn_state;
}

/* On modeset, push the new framebuffer to the scanout. */
static int virtgpu_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    struct drm_device        *dev   = crtc->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    if (!fb) return 0;
    return virtgpu_page_flip(vgdev, fb, NULL);
}

/* Enable the CRTC output and deliver any pending vblank event. */
static void virtgpu_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct drm_device        *dev   = crtc->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_plane         *plane = crtc->primary;

    (void)old_state;

    if (plane && plane->state && plane->state->fb && plane->state->fb != vgdev->current_fb) virtgpu_page_flip(vgdev, plane->state->fb, NULL);
    if (crtc->state && crtc->state->event) {
        drm_crtc_send_vblank_event(crtc, crtc->state->event);
        crtc->state->event = NULL;
    }
}

/* Disable the CRTC scanout and drop the current framebuffer. */
static int virtgpu_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct drm_device        *dev   = crtc->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    (void)old_state;

    /* Keep the recorded scanout on failure so a later commit can retry the disable. */
    return virtgpu_page_flip(vgdev, NULL, vgdev->current_fb);
}

/* Page flip helper for legacy ioctl */
static int virtgpu_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb, struct drm_pending_vblank_event *event, uint32_t flags)
{
    struct drm_device        *dev    = crtc->dev;
    struct virtio_gpu_device *vgdev  = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_framebuffer   *old_fb = crtc->primary->state->fb;
    int                       ret;

    (void)event;
    (void)flags;

    /* A flip must not silently change the mode the CRTC reports. */
    if (fb && crtc->mode.hdisplay && ((uint32_t)crtc->mode.hdisplay != fb->width || (uint32_t)crtc->mode.vdisplay != fb->height)) return -EINVAL;

    ret = virtgpu_page_flip(vgdev, fb, old_fb);
    if (ret) return ret;

    /* The committed plane state, not the ioctl lookup, owns scanout. */
    if (old_fb != fb) {
        drm_framebuffer_get(fb);
        crtc->primary->state->fb = fb;
        drm_framebuffer_put(old_fb);
    }
    crtc->primary->fb_id = fb ? fb->base.id : 0;

    return 0;
}

/*
 * Flush callback invoked by the video subsystem after fbcon draws.
 * Transfers the guest-side framebuffer to the host GPU resource and
 * flushes the scanout so the new pixels become visible.
 */
static void virtgpu_kms_flush_fb(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    struct virtio_gpu_rect damage;
    uint64_t               offset;

    if (!vgdev_flush_ctx || !vgdev_flush_obj) return;
    if (x >= vgdev_flush_obj->width || y >= vgdev_flush_obj->height || !width || !height) return;
    if (width > vgdev_flush_obj->width - x) width = vgdev_flush_obj->width - x;
    if (height > vgdev_flush_obj->height - y) height = vgdev_flush_obj->height - y;

    /*
     * printk is backed by this same scanout.  A GPU error emitted while a
     * flush is unwinding therefore re-enters this callback.  Damage is
     * best-effort (the refresh worker retries changed rows), so drop only the
     * recursive/concurrent flush instead of deadlocking controlq.
     */
    if (!raw_spin_trylock(&vgdev_flush_lock)) return;

    damage = (struct virtio_gpu_rect) {x, y, width, height};
    offset = (uint64_t)y * vgdev_flush_obj->stride + (uint64_t)x * sizeof(uint32_t);
    {
        int flush_ret = virtgpu_cmd_update_2d(vgdev_flush_ctx, vgdev_flush_obj, &damage, offset);

        if (flush_ret) {
            static bool flush_fail_logged;

            if (!flush_fail_logged) {
                flush_fail_logged = true;
                DRM_ERROR("Console flush failed: %d (rect %ux%u+%u+%u)\n", flush_ret, width, height, x, y);
            }
        }
    }
    raw_spin_unlock(&vgdev_flush_lock);
}

/* Set or clear the hardware cursor for the CRTC. */
static int virtgpu_crtc_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *gem, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)crtc->dev->dev_private;
    struct virtio_gpu_object *obj   = gem ? to_virtio_gpu_object(gem) : NULL;
    int                       ret;

    if (obj) {
        /*
         * The VirtIO cursor command requires a 64x64 resource but does not
         * impose a DRM fourcc.  Xorg allocates cursor storage through the
         * dumb-buffer API, which creates XRGB8888 objects.  Accept both
         * advertised 32-bit KMS formats here.
         */
        if (width != 64 || height != 64 || gem->size < 64 * 64 * 4 || obj->width != 64 || obj->height != 64 || (obj->format != DRM_FORMAT_XRGB8888 && obj->format != DRM_FORMAT_ARGB8888)
            || obj->created_3d || obj->created_blob)
            return -EINVAL;
        ret = virtgpu_cmd_transfer_to_host_2d(vgdev, obj, 0);
        if (ret) return ret;
    }
    ret = virtgpu_cmd_update_cursor(vgdev, (uint32_t)crtc->index, obj, crtc->x, crtc->y, hot_x, hot_y);
    return ret;
}

/* Move the hardware cursor to a new position. */
static int virtgpu_crtc_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)crtc->dev->dev_private;
    return virtgpu_cmd_move_cursor(vgdev, (uint32_t)crtc->index, x, y);
}

/* Try one mode for the initial console; release everything on failure. */
static int virtgpu_kms_initial_try_mode(struct virtio_gpu_device *vgdev, struct drm_display_mode *mode, struct virtio_gpu_object **out_obj, struct drm_framebuffer **out_fb)
{
    struct drm_device        *dev = vgdev->drm_dev;
    struct virtio_gpu_object *obj;
    struct drm_framebuffer   *fb;
    uint32_t                  w, h, pitch, size;
    int                       ret;

    *out_obj = NULL;
    *out_fb  = NULL;

    w = (uint32_t)mode->hdisplay;
    h = (uint32_t)mode->vdisplay;

    /* Reject zero or oversized modes before allocating the backing. */
    if (!w || !h || w > 8192 || h > 8192) return -EINVAL;

    pitch = ALIGN_UP(w * 4, VIRTGPU_STRIDE_ALIGN);
    size  = pitch * h;

    obj = virtgpu_gem_alloc_object(dev, size);
    if (!obj) return -ENOMEM;

    obj->format        = DRM_FORMAT_XRGB8888;
    obj->width         = w;
    obj->height        = h;
    obj->stride        = pitch;
    obj->depth         = 24;
    obj->created_3d    = false;
    obj->hw_res_handle = virtgpu_resource_id_alloc(vgdev);

    ret = virtgpu_cmd_create_resource_2d(vgdev, obj);
    if (ret) {
        obj->hw_res_handle = 0;
        goto err_free_obj;
    }

    ret = virtgpu_cmd_attach_backing(vgdev, obj);
    if (ret) goto err_free_obj;
    obj->backing_attached = true;

    fb = malloc(sizeof(*fb));
    if (!fb) {
        ret = -ENOMEM;
        goto err_free_obj;
    }
    memset(fb, 0, sizeof(*fb));

    fb->format     = DRM_FORMAT_XRGB8888;
    fb->modifier   = DRM_FORMAT_MOD_LINEAR;
    fb->width      = w;
    fb->height     = h;
    fb->pitches[0] = pitch;
    fb->offsets[0] = 0;
    fb->obj[0]     = &obj->base;

    ret = drm_framebuffer_init(dev, fb, &virtgpu_fb_funcs);
    if (ret) {
        free(fb);
        goto err_free_obj;
    }

    /* Host rejection of this mode fails here; the caller tries the next. */
    ret = virtgpu_page_flip(vgdev, fb, NULL);
    if (ret) goto err_free_fb;

    *out_obj = obj;
    *out_fb  = fb;
    return 0;
err_free_fb:
    /* Unregister, then drop the registered reference; the final put releases the GEM object. */
    drm_framebuffer_cleanup(fb);
    drm_framebuffer_put(fb);
    return ret;
err_free_obj:
    /* Put releases the mmap offset slot before the driver frees obj. */
    drm_gem_object_put(&obj->base);
    return ret;
}

/* Publish CRTC/plane state and switch the console onto the framebuffer. */
static int virtgpu_kms_initial_commit(struct virtio_gpu_device *vgdev, struct drm_connector *conn, struct drm_display_mode *mode, struct virtio_gpu_object *obj, struct drm_framebuffer *fb)
{
    struct drm_device *dev     = vgdev->drm_dev;
    struct drm_crtc   *crtc    = conn->state ? conn->state->crtc : NULL;
    struct drm_plane  *primary = crtc ? crtc->primary : NULL;
    uint32_t           w       = (uint32_t)mode->hdisplay;
    uint32_t           h       = (uint32_t)mode->vdisplay;

    if (crtc) {
        crtc->enabled = true;
        memcpy(&crtc->mode, mode, sizeof(*mode));
        if (crtc->state) {
            crtc->state->active = true;
            crtc->state->enable = true;
            memcpy(&crtc->state->mode, mode, sizeof(*mode));
        }
        drm_crtc_vblank_on(crtc);
    }

    if (primary) {
        if (primary->state) {
            if (primary->state->fb != fb) {
                drm_framebuffer_get(fb);
                drm_framebuffer_put(primary->state->fb);
                primary->state->fb = fb;
            }
            primary->state->crtc    = crtc;
            primary->state->src.x1  = 0;
            primary->state->src.y1  = 0;
            primary->state->src.x2  = (int32_t)(w << 16);
            primary->state->src.y2  = (int32_t)(h << 16);
            primary->state->dst.x1  = 0;
            primary->state->dst.y1  = 0;
            primary->state->dst.x2  = (int32_t)w;
            primary->state->dst.y2  = (int32_t)h;
            primary->state->visible = true;
        }
        primary->fb_id   = fb->base.id;
        primary->crtc_id = crtc ? crtc->base.id : 0;
    }

    /*
     * Attach the scanout flush hooks to the DRM device, then let the DRM
     * core hand the kernel console over to this framebuffer. The driver
     * only reports its scanout to DRM; all console/TTY work is core-owned.
     */
    vgdev_flush_ctx             = vgdev;
    vgdev_flush_obj             = obj;
    dev->fb_console_flush       = virtgpu_kms_flush_fb;
    dev->fb_console_flush_guard = virtgpu_display_flush_guard;
    drm_kms_console_handoff(dev, fb);

    DRM_INFO("Initial modeset: %ux%u fb=%u crtc=%u\n", w, h, fb->base.id, crtc ? crtc->base.id : 0);
    return 0;
}

/* Pixel area of a mode, used to order the EDID fallbacks. */
static uint64_t virtgpu_mode_area(const struct drm_display_mode *mode)
{
    return (uint64_t)(uint32_t)mode->hdisplay * (uint32_t)mode->vdisplay;
}

/* Sort EDID modes from the largest pixel area down. */
static void virtgpu_edid_sort(struct drm_display_mode **modes, int count)
{
    int i;
    int j;

    for (i = 1; i < count; i++) {
        struct drm_display_mode *key = modes[i];
        for (j = i - 1; j >= 0 && virtgpu_mode_area(modes[j]) < virtgpu_mode_area(key); j--) modes[j + 1] = modes[j];
        modes[j + 1] = key;
    }
}

/* The EDID preferred mode, else the largest one; @modes must be sorted. */
static struct drm_display_mode *virtgpu_edid_preferred(struct virtio_gpu_device *vgdev)
{
    int i;

    for (i = 0; i < vgdev->edid_mode_count; i++)
        if (vgdev->edid_modes[i]->type & DRM_MODE_TYPE_PREFERRED) return vgdev->edid_modes[i];
    return vgdev->edid_mode_count ? vgdev->edid_modes[0] : NULL;
}

/* True when @width x @height was already attempted. */
static bool virtgpu_geometry_tried(const uint32_t (*tried)[2], unsigned count, uint32_t width, uint32_t height)
{
    unsigned i;

    for (i = 0; i < count; i++)
        if (tried[i][0] == width && tried[i][1] == height) return true;
    return false;
}

/* Try one mode for the console, remembering its geometry. */
static int virtgpu_kms_try_mode(struct virtio_gpu_device *vgdev, struct drm_connector *connector, struct drm_display_mode *mode, uint32_t (*tried)[2], unsigned *tried_count, int *last_ret)
{
    struct virtio_gpu_object *obj;
    struct drm_framebuffer   *fb;
    uint32_t                  width  = (uint32_t)mode->hdisplay;
    uint32_t                  height = (uint32_t)mode->vdisplay;
    int                       ret;

    if (!width || !height) return -EINVAL;
    if (virtgpu_geometry_tried(tried, *tried_count, width, height)) return 1;
    if (*tried_count < VIRTGPU_MAX_EDID_MODES + VIRTGPU_MAX_SCANOUTS + 2) {
        tried[*tried_count][0] = width;
        tried[*tried_count][1] = height;
        (*tried_count)++;
    }

    ret = virtgpu_kms_initial_try_mode(vgdev, mode, &obj, &fb);
    if (ret) {
        *last_ret = ret;
        return ret;
    }

    ret = virtgpu_kms_initial_commit(vgdev, connector, mode, obj, fb);
    if (!ret) return 0;

    drm_framebuffer_cleanup(fb);
    drm_framebuffer_put(fb);
    *last_ret = ret;
    return ret;
}

/* Try a synthetic mode of the given geometry. */
static int virtgpu_kms_try_geometry(struct virtio_gpu_device *vgdev, struct drm_connector *connector, uint32_t width, uint32_t height, uint32_t (*tried)[2], unsigned *tried_count, int *last_ret)
{
    struct drm_display_mode mode;
    if (!width || !height || width > 8192 || height > 8192) return -EINVAL;

    virtgpu_mode_fill(&mode, width, height, 60);
    return virtgpu_kms_try_mode(vgdev, connector, &mode, tried, tried_count, last_ret);
}

/* Boot modeset in fallback order: EDID modes, boot geometry, configured default. */
static int virtgpu_kms_initial_modeset(struct virtio_gpu_device *vgdev)
{
    struct drm_device         *dev    = vgdev->drm_dev;
    struct drm_mode_config    *config = &dev->mode_config;
    struct drm_connector      *conn   = NULL;
    struct drm_display_mode   *preferred;
    struct limine_framebuffer *boot;
    uint32_t                   tried[VIRTGPU_MAX_EDID_MODES + VIRTGPU_MAX_SCANOUTS + 2][2];
    unsigned                   tried_count = 0;
    ilist_node_t              *node;
    int                        last_ret = -ENODEV;
    int                        i;

    /* Find the first connected connector */
    for (node = config->connector_list.next; node != &config->connector_list; node = node->next) {
        struct drm_connector *candidate = container_of(node, struct drm_connector, head);
        if (candidate->status == connector_status_connected) {
            conn = candidate;
            break;
        }
    }
    if (!conn) {
        DRM_INFO("No connected connector - skipping initial modeset.\n");
        return -ENODEV;
    }

    /* The EDID preferred mode, then every other EDID mode, largest first. */
    virtgpu_edid_sort(vgdev->edid_modes, vgdev->edid_mode_count);
    preferred = virtgpu_edid_preferred(vgdev);

    if (preferred && !virtgpu_kms_try_mode(vgdev, conn, preferred, tried, &tried_count, &last_ret)) return 0;
    for (i = 0; i < vgdev->edid_mode_count; i++) {
        if (vgdev->edid_modes[i] == preferred) continue;
        if (!virtgpu_kms_try_mode(vgdev, conn, vgdev->edid_modes[i], tried, &tried_count, &last_ret)) return 0;
    }

    /* Host scanout modes are advertised by the display too; they precede the boot geometry. */
    for (i = 0; i < vgdev->num_scanouts && i < VIRTGPU_MAX_SCANOUTS; i++) {
        struct virtio_gpu_display_mode *dm = &vgdev->scanouts[i];
        struct drm_display_mode         mode;

        if (!dm->enabled || dm->width <= 0 || dm->height <= 0 || dm->width > 8192 || dm->height > 8192) continue;
        virtgpu_mode_fill(&mode, (uint32_t)dm->width, (uint32_t)dm->height, (uint32_t)dm->vrefresh);
        if (!virtgpu_kms_try_mode(vgdev, conn, &mode, tried, &tried_count, &last_ret)) return 0;
    }

    /* The geometry the bootloader left on the host window. */
    boot = get_framebuffer();
    if (boot && boot->width && boot->height && boot->width <= 8192 && boot->height <= 8192) {
        if (!virtgpu_kms_try_geometry(vgdev, conn, (uint32_t)boot->width, (uint32_t)boot->height, tried, &tried_count, &last_ret)) return 0;
    }

    /* The configured console mode. */
    if (!virtgpu_kms_try_geometry(vgdev, conn, CONFIG_DRM_DEFAULT_WIDTH, CONFIG_DRM_DEFAULT_HEIGHT, tried, &tried_count, &last_ret)) return 0;

    DRM_WARN("initial modeset: no usable mode (%d)\n", last_ret);
    return last_ret;
}

/* Set up the full KMS pipeline: plane, CRTC, encoder and connector. */
int virtgpu_kms_init(struct virtio_gpu_device *vgdev)
{
    struct drm_device    *dev = vgdev->drm_dev;
    struct drm_crtc      *crtc;
    struct drm_plane     *primary;
    struct drm_encoder   *encoder;
    struct drm_connector *connector;
    int                   ret;

    /* Query display info from host */
    ret = virtgpu_cmd_get_display_info(vgdev);
    if (ret) {
        /* An unknown scanout count lets get_modes() synthesize the fallback mode. */
        vgdev->num_scanouts = 0;
        plogk("virtgpu: Failed to get display info: %d (using defaults)\n", ret);
    }

    /* Primary plane */

    primary = malloc(sizeof(*primary));
    if (!primary) {
        DRM_ERROR("out of memory allocating primary plane.\n");
        return -ENOMEM;
    }
    memset(primary, 0, sizeof(*primary));
    vgdev->kms_primary = primary;

    ret = drm_plane_init(dev, primary, 1, (void *)&virtgpu_plane_helpers, virtgpu_formats, sizeof(virtgpu_formats) / sizeof(virtgpu_formats[0]), NULL, DRM_PLANE_TYPE_PRIMARY, "virtgpu-primary");
    if (ret) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        free(primary);
        vgdev->kms_primary = NULL;
        return ret;
    }

    primary->state = malloc(sizeof(*primary->state));
    if (!primary->state) {
        DRM_ERROR("out of memory allocating plane state.\n");
        return -ENOMEM;
    }
    memset(primary->state, 0, sizeof(*primary->state));
    primary->state->plane   = primary;
    primary->state->alpha   = 0xFFFF;
    primary->state->visible = true;

    /* CRTC (with real helper callbacks) */

    crtc = malloc(sizeof(*crtc));
    if (!crtc) {
        DRM_ERROR("out of memory allocating CRTC.\n");
        return -ENOMEM;
    }
    memset(crtc, 0, sizeof(*crtc));
    vgdev->kms_crtc = crtc;

    /* CRTC helpers the core calls on modeset/page-flip/enable/disable; kept in crtc->helper_private. */
    ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL, (void *)&virtgpu_crtc_helpers, "virtgpu-crtc-0");
    if (ret) {
        DRM_ERROR("Failed to init CRTC: %d\n", ret);
        free(crtc);
        vgdev->kms_crtc = NULL;
        return ret;
    }

    dev->mode_config.async_page_flip = true;

    crtc->state = malloc(sizeof(*crtc->state));
    if (!crtc->state) {
        DRM_ERROR("out of memory allocating CRTC state.\n");
        return -ENOMEM;
    }
    memset(crtc->state, 0, sizeof(*crtc->state));
    crtc->state->crtc   = crtc;
    crtc->state->active = false;
    crtc->state->enable = false;

    /* Encoder (with helper callbacks) */

    encoder = malloc(sizeof(*encoder));
    if (!encoder) {
        DRM_ERROR("out of memory allocating encoder.\n");
        return -ENOMEM;
    }
    memset(encoder, 0, sizeof(*encoder));
    vgdev->kms_encoder = encoder;

    ret = drm_encoder_init(dev, encoder, (void *)&virtgpu_enc_helpers, DRM_MODE_ENCODER_VIRTUAL, "virtgpu-encoder-0");
    if (ret) {
        DRM_ERROR("Failed to init encoder: %d\n", ret);
        free(encoder);
        vgdev->kms_encoder = NULL;
        return ret;
    }
    encoder->possible_crtcs = 1;
    encoder->crtc           = crtc;

    /* Connector (with helper callbacks) */

    connector = malloc(sizeof(*connector));
    if (!connector) {
        DRM_ERROR("out of memory allocating connector.\n");
        return -ENOMEM;
    }
    memset(connector, 0, sizeof(*connector));
    vgdev->kms_connector = connector;

    ret = drm_connector_init(dev, connector, (void *)&virtgpu_conn_helpers, DRM_MODE_CONNECTOR_VIRTUAL);
    if (ret) {
        DRM_ERROR("Failed to init connector: %d\n", ret);
        free(connector);
        vgdev->kms_connector = NULL;
        return ret;
    }
    connector->status = connector_status_connected;

    connector->state = malloc(sizeof(*connector->state));
    if (!connector->state) {
        DRM_ERROR("out of memory allocating connector state.\n");
        return -ENOMEM;
    }
    memset(connector->state, 0, sizeof(*connector->state));
    connector->state->connector    = connector;
    connector->state->crtc         = crtc;
    connector->state->best_encoder = encoder;

    ret = drm_connector_attach_encoder(connector, encoder);
    if (ret) {
        DRM_ERROR("Failed to attach encoder: %d\n", ret);
        return ret;
    }

    /* Add display modes */
    ret = virtgpu_connector_get_modes(connector);
    if (ret) {
        DRM_ERROR("Failed to add modes: %d\n", ret);
        return ret;
    }

    ret = drm_vblank_init(dev, 1);
    if (ret) {
        DRM_ERROR("Failed to init vblank: %d\n", ret);
        return ret;
    }

    drm_connector_register(connector);

    {
        /* Count the probed modes actually on the connector. */
        ilist_node_t *node       = connector->modes.next;
        int           mode_count = 0;

        while (node && node != &connector->modes) {
            mode_count++;
            node = node->next;
        }
        DRM_INFO("virtgpu: KMS pipeline up, %d mode(s)\n", mode_count);
    }

    /* The initial modeset runs before drm_dev_register() publishes the node. */
    ret = virtgpu_kms_initial_modeset(vgdev);
    if (ret) return ret;

    return 0;
}

/* Disable the scanout when the KMS pipeline is torn down. */
void virtgpu_kms_fini(struct virtio_gpu_device *vgdev)
{
    if (!vgdev) return;

    /* Disable only the scanout we set; a failed probe leaves the boot one alone. */
    if (vgdev->current_fb) (void)virtgpu_cmd_set_scanout(vgdev, 0, NULL);
}

#endif
