/*
 *
 *      virtgpu_kms.c
 *      VirtIO-GPU KMS display pipeline
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Implements the full KMS pipeline: CRTC, plane, encoder, connector,
 *  atomic commit, page flip, and framebuffer-to-scanout connection.
 *  Display modes are queried from the virtio host via GET_DISPLAY_INFO
 *  (and optionally via EDID). The DRM core calls our helper callbacks
 *  via helper_private pointers stored during init.
 *
 */

#include <drivers/gpu/drm/drm_edid.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_print.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/tty/tty.h>
#include <drivers/virt/gpu/virtgpu_cmd.h>
#include <drivers/virt/gpu/virtgpu_drv.h>
#include <drivers/virt/gpu/virtgpu_gem.h>
#include <drivers/virt/gpu/virtgpu_kms.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <mem/alloc.h>

/* Static context for the DRM-flush callback (set during initial modeset) */
static struct virtio_gpu_device *vgdev_flush_ctx;
static struct virtio_gpu_object *vgdev_flush_obj;

/* container_of helper (not yet in a shared kernel header) */
#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Extern declarations for DRM core helpers used here */

/* Forward declaration of the page-flip helper defined in gpu.c */

/* ------------------------------------------------------------------ */
/* Connector helper functions                                         */
/* ------------------------------------------------------------------ */

static enum drm_connector_status virtgpu_connector_detect(struct drm_connector *connector, bool force)
{
    struct drm_device        *dev   = connector->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    (void)force;

    /* VirtIO-GPU is always connected in a VM */
    return vgdev->num_scanouts > 0 ? connector_status_connected : connector_status_disconnected;
}

/*
 * Fetch the EDID for scanout 0 from the host and, if valid, publish it as
 * the connector's EDID property blob and add the EDID-derived modes.
 *
 * Returns the number of modes added, or 0 if no EDID / invalid EDID.
 */
static int virtgpu_connector_get_edid_modes(struct drm_connector *connector)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)connector->dev->dev_private;
    struct edid              *edid;
    int                       edid_size = 0;
    int                       count     = 0;
    int                       ret;

    if (!vgdev->has_edid) { return 0; }

    /* The host EDID response is capped at 1024 bytes by the virtio protocol. */
    edid = malloc(1024);
    if (!edid) { return 0; }

    ret = virtgpu_cmd_get_edid(vgdev, 0, edid, &edid_size);
    if (ret || edid_size <= 0) {
        free(edid);
        return 0;
    }

    if (!drm_edid_is_valid(edid)) {
        plogk("virtgpu: Connector: EDID invalid (%d bytes)\n", edid_size);
        free(edid);
        return 0;
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
    if (count > 0) {
        char name[14];

        drm_edid_get_monitor_name(edid, name, sizeof(name));
        plogk("virtgpu: Connector: EDID parsed: %d mode(s), monitor=\"%s\", %ux%u mm.\n", count, name[0] ? name : "unknown",
              connector->display_info_width_mm, connector->display_info_height_mm);
    }

    free(edid);
    return count;
}

static int virtgpu_connector_get_modes(struct drm_connector *connector)
{
    struct drm_device        *dev   = connector->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    int                       i;

    /* Query display info from host if not already done */
    if (vgdev->num_scanouts == 0) { virtgpu_cmd_get_display_info(vgdev); }

    /* Prefer the real EDID modes when the host provides a valid EDID. */
    if (virtgpu_connector_get_edid_modes(connector) > 0) { return 0; }

    for (i = 0; i < vgdev->num_scanouts && i < VIRTGPU_MAX_SCANOUTS; i++) {
        struct virtio_gpu_display_mode *dm = &vgdev->scanouts[i];
        struct drm_display_mode        *mode;

        if (!dm->enabled) { continue; }

        mode = drm_mode_create(dev);
        if (!mode) { return -ENOMEM; }

        (void)snprintf(mode->name, DRM_DISPLAY_MODE_LEN - 1, "%dx%d", dm->width, dm->height);
        mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
        mode->clock                          = dm->width * dm->height * (dm->vrefresh ? dm->vrefresh : 60) / 1000;
        mode->hdisplay                       = dm->width;
        mode->hsync_start                    = dm->width + 80;
        mode->hsync_end                      = dm->width + 160;
        mode->htotal                         = dm->width + 320;
        mode->vdisplay                       = dm->height;
        mode->vsync_start                    = dm->height + 3;
        mode->vsync_end                      = dm->height + 6;
        mode->vtotal                         = dm->height + 32;
        mode->vrefresh                       = dm->vrefresh ? dm->vrefresh : 60;
        mode->flags                          = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
        mode->type                           = DRM_MODE_TYPE_DRIVER;
        mode->status                         = MODE_OK;

        if (i == 0) { mode->type |= DRM_MODE_TYPE_PREFERRED; }

        drm_mode_probed_add(connector, mode);
    }

    /* Fallback: add a default mode if host returned no display info */
    if (vgdev->num_scanouts == 0) {
        struct drm_display_mode *mode = drm_mode_create(dev);

        if (!mode) { return -ENOMEM; }

        (void)snprintf(mode->name, DRM_DISPLAY_MODE_LEN - 1, "%dx%d", VIRTGPU_DEFAULT_WIDTH, VIRTGPU_DEFAULT_HEIGHT);
        mode->clock       = VIRTGPU_DEFAULT_WIDTH * VIRTGPU_DEFAULT_HEIGHT * 60 / 1000;
        mode->hdisplay    = VIRTGPU_DEFAULT_WIDTH;
        mode->hsync_start = VIRTGPU_DEFAULT_WIDTH + 80;
        mode->hsync_end   = VIRTGPU_DEFAULT_WIDTH + 160;
        mode->htotal      = VIRTGPU_DEFAULT_WIDTH + 320;
        mode->vdisplay    = VIRTGPU_DEFAULT_HEIGHT;
        mode->vsync_start = VIRTGPU_DEFAULT_HEIGHT + 3;
        mode->vsync_end   = VIRTGPU_DEFAULT_HEIGHT + 6;
        mode->vtotal      = VIRTGPU_DEFAULT_HEIGHT + 32;
        mode->vrefresh    = 60;
        mode->flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
        mode->type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
        mode->status      = MODE_OK;

        drm_mode_probed_add(connector, mode);
        vgdev->num_scanouts         = 1;
        vgdev->scanouts[0].enabled  = true;
        vgdev->scanouts[0].width    = VIRTGPU_DEFAULT_WIDTH;
        vgdev->scanouts[0].height   = VIRTGPU_DEFAULT_HEIGHT;
        vgdev->scanouts[0].vrefresh = 60;
    }

    return 0;
}

static int virtgpu_connector_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode)
{
    (void)connector;

    /* Accept all modes up to 8192x8192 */
    if (mode->hdisplay <= 0 || mode->vdisplay <= 0 || mode->hdisplay > 8192 || mode->vdisplay > 8192) return MODE_BAD;
    return MODE_OK;
}

/* ------------------------------------------------------------------ */
/* Encoder helper functions                                           */
/* ------------------------------------------------------------------ */

static void virtgpu_encoder_atomic_check(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state, struct drm_connector_state *conn_state)
{
    (void)encoder;
    (void)crtc_state;
    (void)conn_state;
}

/* ------------------------------------------------------------------ */
/* CRTC helper functions                                              */
/* ------------------------------------------------------------------ */

static void virtgpu_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    struct drm_device        *dev   = crtc->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    if (fb) { virtgpu_page_flip(vgdev, fb, NULL); }
}

static void virtgpu_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct drm_device        *dev   = crtc->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_plane         *plane = crtc->primary;

    (void)old_state;

    DRM_DEBUG_KMS("CRTC-%d enabled\n", crtc->base.id);

    if (plane && plane->state && plane->state->fb && plane->state->fb != vgdev->current_fb) { virtgpu_page_flip(vgdev, plane->state->fb, NULL); }

    if (crtc->state && crtc->state->event) {
        drm_crtc_send_vblank_event(crtc, crtc->state->event);
        crtc->state->event = NULL;
    }
}

static void virtgpu_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct drm_device        *dev   = crtc->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    (void)old_state;

    DRM_DEBUG_KMS("CRTC-%d disabled\n", crtc->base.id);

    virtgpu_cmd_set_scanout(vgdev, 0, NULL);
    vgdev->current_fb          = NULL;
    vgdev->current_scanout_obj = NULL;
}

/* ------------------------------------------------------------------ */
/* Page flip helper for legacy ioctl                                   */
/* ------------------------------------------------------------------ */

static int virtgpu_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb, struct drm_pending_vblank_event *event, uint32_t flags)
{
    struct drm_device        *dev   = crtc->dev;
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    int                       ret;

    (void)event;
    (void)flags;

    ret = virtgpu_page_flip(vgdev, fb, crtc->primary->state->fb);
    if (ret) { return ret; }

    crtc->primary->state->fb = fb;
    crtc->primary->fb_id     = fb ? fb->base.id : 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Mode config helpers                                                 */
/* ------------------------------------------------------------------ */

static const uint32_t virtgpu_formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
};

/* ------------------------------------------------------------------ */
/* Initial modeset –enable a framebuffer for immediate display        */
/* ------------------------------------------------------------------ */

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

    damage = (struct virtio_gpu_rect) {x, y, width, height};
    offset = (uint64_t)y * vgdev_flush_obj->stride + (uint64_t)x * sizeof(uint32_t);
    (void)virtgpu_cmd_update_2d(vgdev_flush_ctx, vgdev_flush_obj, &damage, offset);
}

static int virtgpu_crtc_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *gem, uint32_t width, uint32_t height, int32_t hot_x,
                                   int32_t hot_y)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)crtc->dev->dev_private;
    struct virtio_gpu_object *obj   = gem ? to_virtio_gpu_object(gem) : NULL;
    int                       ret;

    if (obj) {
        /* VirtIO 1.2 requires a 64x64 ARGB resource on the cursor fast path. */
        if (width != 64 || height != 64 || gem->size < 64 * 64 * 4 || obj->width != 64 || obj->height != 64 || obj->format != DRM_FORMAT_ARGB8888
            || obj->created_3d || obj->created_blob)
            return -EINVAL;
        ret = virtgpu_cmd_transfer_to_host_2d(vgdev, obj, 0);
        if (ret) return ret;
    }
    return virtgpu_cmd_update_cursor(vgdev, (uint32_t)crtc->index, obj, crtc->x, crtc->y, hot_x, hot_y);
}

static int virtgpu_crtc_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)crtc->dev->dev_private;
    return virtgpu_cmd_move_cursor(vgdev, (uint32_t)crtc->index, x, y);
}

/*
 * Perform an initial modeset on the first connected connector so the
 * display is active as soon as the driver loads.  Without this the CRTC
 * defaults to inactive and the screen stays blank until userspace opens
 * the DRM device and commits a mode.
 *
 * Returns 0 on success; negative errno on failure (non-fatal –the KMS
 * pipeline stays registered and usable from userspace).
 */
static int virtgpu_kms_initial_modeset(struct virtio_gpu_device *vgdev)
{
    struct drm_device        *dev      = vgdev->drm_dev;
    struct drm_mode_config   *config   = &dev->mode_config;
    struct drm_connector     *conn     = NULL;
    struct drm_display_mode  *pref     = NULL;
    struct drm_display_mode  *fallback = NULL;
    struct drm_crtc          *crtc;
    struct drm_plane         *primary;
    struct virtio_gpu_object *obj;
    struct drm_framebuffer   *fb;
    ilist_node_t             *node;
    uint32_t                  w, h, pitch, size;
    int                       ret;

    /* 1. Find the first connected connector */
    for (node = config->connector_list.next; node != &config->connector_list; node = node->next) {
        conn = container_of(node, struct drm_connector, head);
        if (conn->status == connector_status_connected) { break; }
    }
    if (!conn) {
        DRM_INFO("No connected connector –skipping initial modeset\n");
        return -ENODEV;
    }

    /* 2. Pick a mode –PREFERRED wins, otherwise first probed */
    for (node = conn->modes.next; node != &conn->modes; node = node->next) {
        struct drm_display_mode *m = container_of(node, struct drm_display_mode, head);
        if (!fallback) { fallback = m; }
        if (m->type & DRM_MODE_TYPE_PREFERRED) {
            pref = m;
            break;
        }
    }
    if (!pref) { pref = fallback; }
    if (!pref) {
        DRM_INFO("No modes on connector –skipping initial modeset\n");
        return -ENODEV;
    }

    w = (uint32_t)pref->hdisplay;
    h = (uint32_t)pref->vdisplay;
    if (!w || !h) { return -EINVAL; }

    /* 3. Create a GEM object with host-side 2D resource + backing */
    pitch = ALIGN_UP(w * 4, VIRTGPU_STRIDE_ALIGN);
    size  = pitch * h;

    obj = virtgpu_gem_alloc_object(dev, size);
    if (!obj) { return -ENOMEM; }

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
    if (ret) { goto err_free_obj; }
    obj->backing_attached = true;

    /* 4. Create and register the DRM framebuffer */
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

    /* 5. Push the framebuffer to the scanout */
    ret = virtgpu_page_flip(vgdev, fb, NULL);
    if (ret) {
        DRM_ERROR("Initial modeset page_flip failed: %d\n", ret);
        /* Don't bail –FB is registered and usable; just not shown */
    }

    /* 6. Update in-kernel CRTC and primary-plane state */
    crtc    = conn->state ? conn->state->crtc : NULL;
    primary = crtc ? crtc->primary : NULL;

    if (crtc) {
        crtc->enabled = true;
        memcpy(&crtc->mode, pref, sizeof(*pref));
        if (crtc->state) {
            crtc->state->active = true;
            crtc->state->enable = true;
            memcpy(&crtc->state->mode, pref, sizeof(*pref));
        }
    }

    if (primary) {
        if (primary->state) {
            primary->state->fb      = fb;
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
     * Switch the kernel console to the DRM framebuffer so all printk
     * and TTY output appears on the virtio-gpu display.
     */
    vgdev_flush_ctx = vgdev;
    vgdev_flush_obj = obj;
    video_switch_framebuffer(obj->base.backing, w, h, pitch, virtgpu_kms_flush_fb);

    /* Update TTY to DRM mode now that the DRM framebuffer is live */
    tty_set_device_type(TTY_DEVICE_DRM);

    DRM_INFO("Initial modeset: %ux%u fb=%u crtc=%u\n", w, h, fb->base.id, crtc ? crtc->base.id : 0);
    return 0;

err_free_obj:
    virtgpu_gem_free_object(&obj->base);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Public init / fini                                                  */
/* ------------------------------------------------------------------ */

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
    if (ret) { plogk("virtgpu: Failed to get display info: %d (using defaults)\n", ret); }

    /* ---------- Primary plane ---------- */

    primary = malloc(sizeof(*primary));
    if (!primary) { return -ENOMEM; }
    memset(primary, 0, sizeof(*primary));
    vgdev->kms_primary = primary;

    ret = drm_plane_init(dev, primary, 1, NULL, virtgpu_formats, sizeof(virtgpu_formats) / sizeof(virtgpu_formats[0]), NULL,
                         DRM_PLANE_TYPE_PRIMARY, "virtgpu-primary");
    if (ret) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        free(primary);
        vgdev->kms_primary = NULL;
        return ret;
    }

    primary->state = malloc(sizeof(*primary->state));
    if (!primary->state) { return -ENOMEM; }
    memset(primary->state, 0, sizeof(*primary->state));
    primary->state->plane   = primary;
    primary->state->alpha   = 0xFFFF;
    primary->state->visible = true;

    /* ---------- CRTC (with real helper callbacks) ---------- */

    crtc = malloc(sizeof(*crtc));
    if (!crtc) { return -ENOMEM; }
    memset(crtc, 0, sizeof(*crtc));
    vgdev->kms_crtc = crtc;

    /*
     * Define CRTC helper functions that the DRM core will call
     * on modeset, page_flip, enable, and disable operations.
     * These are stored in crtc->helper_private and cast back
     * to drm_crtc_helper_funcs by the core when needed.
     */
    {
        static const struct drm_crtc_helper_funcs crtc_helpers = {
            .mode_set       = virtgpu_crtc_atomic_flush,
            .page_flip      = virtgpu_crtc_page_flip,
            .cursor_set     = virtgpu_crtc_cursor_set,
            .cursor_move    = virtgpu_crtc_cursor_move,
            .atomic_enable  = virtgpu_crtc_atomic_enable,
            .atomic_disable = virtgpu_crtc_atomic_disable,
        };

        ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL, (void *)&crtc_helpers, "virtgpu-crtc-0");
        if (ret) {
            DRM_ERROR("Failed to init CRTC: %d\n", ret);
            free(crtc);
            vgdev->kms_crtc = NULL;
            return ret;
        }
    }

    dev->mode_config.async_page_flip = true;

    crtc->state = malloc(sizeof(*crtc->state));
    if (!crtc->state) { return -ENOMEM; }
    memset(crtc->state, 0, sizeof(*crtc->state));
    crtc->state->crtc   = crtc;
    crtc->state->active = false;
    crtc->state->enable = false;

    /* ---------- Encoder (with helper callbacks) ---------- */

    encoder = malloc(sizeof(*encoder));
    if (!encoder) { return -ENOMEM; }
    memset(encoder, 0, sizeof(*encoder));
    vgdev->kms_encoder = encoder;

    {
        static const struct drm_encoder_helper_funcs enc_helpers = {
            .atomic_mode_set = virtgpu_encoder_atomic_check,
        };

        ret = drm_encoder_init(dev, encoder, (void *)&enc_helpers, DRM_MODE_ENCODER_VIRTUAL, "virtgpu-encoder-0");
        if (ret) {
            DRM_ERROR("Failed to init encoder: %d\n", ret);
            free(encoder);
            vgdev->kms_encoder = NULL;
            return ret;
        }
    }
    encoder->possible_crtcs = 1;
    encoder->crtc           = crtc;

    /* ---------- Connector (with helper callbacks) ---------- */

    connector = malloc(sizeof(*connector));
    if (!connector) { return -ENOMEM; }
    memset(connector, 0, sizeof(*connector));
    vgdev->kms_connector = connector;

    {
        static const struct drm_connector_helper_funcs conn_helpers = {
            .detect     = virtgpu_connector_detect,
            .get_modes  = virtgpu_connector_get_modes,
            .mode_valid = virtgpu_connector_mode_valid,
        };

        ret = drm_connector_init(dev, connector, (void *)&conn_helpers, DRM_MODE_CONNECTOR_VIRTUAL);
        if (ret) {
            DRM_ERROR("Failed to init connector: %d\n", ret);
            free(connector);
            vgdev->kms_connector = NULL;
            return ret;
        }
    }
    connector->status                 = connector_status_connected;
    connector->display_info_width_mm  = 500;
    connector->display_info_height_mm = 280;

    connector->state = malloc(sizeof(*connector->state));
    if (!connector->state) { return -ENOMEM; }
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
        DRM_INFO("KMS pipeline: CRTC-%d + primary plane-%d + encoder-%d + connector-%d (%d modes)\n", crtc->base.id, primary->base.id,
                 encoder->base.id, connector->base.id, mode_count);
    }

    /*
     * Perform an initial modeset so the display is live immediately.
     * Failure is non-fatal –the KMS pipeline remains registered.
     */
    ret = virtgpu_kms_initial_modeset(vgdev);
    if (ret) { DRM_INFO("Initial modeset deferred (display will activate on first userspace commit)\n"); }

    return 0;
}

void virtgpu_kms_fini(struct virtio_gpu_device *vgdev)
{
    virtgpu_cmd_set_scanout(vgdev, 0, NULL);
}
