/*
 *
 *      vmfgfx_kms.c
 *      VMware SVGA II display driver (vmfgfx) - KMS pipeline
 *
 *      2026/10/04 By Yinyuan34513
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/drm_print.h>
#include <drivers/gpu/vmware/vmfgfx.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

#if CONFIG_VMFGFX && CONFIG_DRM

/*
 * The console flush callback receives no context, so the device being
 * flushed is kept here.  There is exactly one controller per machine.
 */
static struct vmfgfx_device *vmfgfx_flush_dev;

/* Plane formats: both variants are scanned out as 32-bit XRGB. */
static const uint32_t vmfgfx_formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
};

/* Standard VESA timings the SVGA device is happy to scan out. */
static const struct {
        uint32_t width;
        uint32_t height;
} vmfgfx_mode_list[] = {
    {640,  480 },
    {720,  480 },
    {800,  600 },
    {848,  480 },
    {1024, 576 },
    {1024, 768 },
    {1152, 864 },
    {1280, 720 },
    {1280, 768 },
    {1280, 800 },
    {1280, 960 },
    {1280, 1024},
    {1360, 768 },
    {1366, 768 },
    {1440, 900 },
    {1600, 900 },
    {1600, 1200},
    {1680, 1050},
    {1920, 1080},
    {1920, 1200},
    {2048, 1152},
};

/* Copy one framebuffer rectangle into the scanout window of video RAM. */
static int vmfgfx_kms_scanout(struct vmfgfx_device *vdev, struct drm_framebuffer *fb)
{
    uint32_t copy_w, copy_h;
    int      ret;

    if (!vdev) return -EINVAL;
    if (!fb) {
        vdev->current_fb = NULL;
        return 0;
    }

    copy_w = fb->width < vdev->mode_width ? fb->width : vdev->mode_width;
    copy_h = fb->height < vdev->mode_height ? fb->height : vdev->mode_height;
    ret    = vmfgfx_scanout_rect(vdev, fb, 0, 0, copy_w, copy_h);
    if (ret) return ret;

    vdev->current_fb = fb;
    return 0;
}

/* Flush userspace damage from the current scanout buffer to video RAM. */
static int vmfgfx_kms_dirty_fb(struct drm_framebuffer *fb, struct drm_file *file_priv, unsigned int flags, unsigned int color, struct drm_clip_rect *clips, unsigned int num_clips)
{
    struct vmfgfx_device *vdev;
    unsigned int          first, step;
    int                   ret;

    (void)file_priv;
    (void)color;

    if (!fb || !fb->obj[0] || !fb->obj[0]->dev) return -EINVAL;
    vdev = (struct vmfgfx_device *)fb->obj[0]->dev->dev_private;
    if (!vdev) return -EINVAL;

    /* A later page flip uploads off-screen buffers in full. */
    if (vdev->current_fb != fb) return 0;
    if (!num_clips) return vmfgfx_scanout_rect(vdev, fb, 0, 0, fb->width, fb->height);

    first = (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) ? 1U : 0U;
    step  = (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) ? 2U : 1U;

    for (unsigned int i = first; i < num_clips; i += step) {
        ret = vmfgfx_scanout_rect(vdev, fb, clips[i].x1, clips[i].y1, clips[i].x2 - clips[i].x1, clips[i].y2 - clips[i].y1);
        if (ret) return ret;
    }
    return 0;
}

const struct drm_framebuffer_funcs vmfgfx_fb_funcs = {
    .dirty = vmfgfx_kms_dirty_fb,
};

/*
 * Flush callback invoked by the video subsystem after fbcon draws: copy the
 * damaged rectangle of the console buffer out to video RAM and tell the
 * host about it.  The path never logs, so a contended lock only ever means
 * that the next draw flushes the same rectangle again.
 */
static void vmfgfx_kms_flush_fb(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    struct vmfgfx_device *vdev = vmfgfx_flush_dev;

    if (!vdev || !vdev->current_fb) return;
    if (!raw_spin_trylock(&vdev->flush_lock)) return;

    (void)vmfgfx_scanout_rect(vdev, vdev->current_fb, x, y, width, height);

    raw_spin_unlock(&vdev->flush_lock);
}

/* On a modeset, push the new mode and the new framebuffer to the device. */
static void vmfgfx_kms_crtc_mode_set(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    struct vmfgfx_device *vdev = (struct vmfgfx_device *)crtc->dev->dev_private;

    if (!fb) return;

    /*
     * The atomic core only writes the new mode into crtc->mode after this
     * callback has returned, so there is no requested mode to read here -
     * only the framebuffer the core matched it against.  Clients build that
     * framebuffer from the mode they ask for, and this driver cannot scale
     * src onto dst, so its shape is also the shape the hardware has to scan
     * out.
     */
    (void)vmfgfx_set_mode(vdev, fb->width, fb->height);
    (void)vmfgfx_kms_scanout(vdev, fb);
}

/* Page flip helper for the legacy ioctl and atomic fb-only commits. */
static int vmfgfx_kms_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb, struct drm_pending_vblank_event *event, uint32_t flags)
{
    struct vmfgfx_device *vdev = (struct vmfgfx_device *)crtc->dev->dev_private;
    int                   ret;

    (void)event;
    (void)flags;
    if (!fb) return -EINVAL;

    ret = vmfgfx_kms_scanout(vdev, fb);
    if (ret) return ret;

    /*
     * The legacy page-flip ioctl expects the driver to commit the plane;
     * transfer the committed-state reference before its lookup pin drops.
     */
    if (crtc->primary && crtc->primary->state) {
        struct drm_framebuffer *old_fb = crtc->primary->state->fb;
        if (old_fb != fb) {
            drm_framebuffer_get(fb);
            crtc->primary->state->fb = fb;
            drm_framebuffer_put(old_fb);
        }
        crtc->primary->fb_id = fb ? fb->base.id : 0;
    }
    return 0;
}

/* Hand a new shape to the hardware cursor and show it. */
static int vmfgfx_kms_crtc_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *obj, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y)
{
    struct vmfgfx_device *vdev = (struct vmfgfx_device *)crtc->dev->dev_private;

    return vmfgfx_cursor_set(vdev, obj, width, height, hot_x, hot_y);
}

/* Move the hardware cursor to a new position. */
static int vmfgfx_kms_crtc_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y)
{
    struct vmfgfx_device *vdev = (struct vmfgfx_device *)crtc->dev->dev_private;

    return vmfgfx_cursor_move(vdev, x, y);
}

/* Enable the CRTC output and deliver any pending vblank event. */
static void vmfgfx_kms_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct vmfgfx_device *vdev  = (struct vmfgfx_device *)crtc->dev->dev_private;
    struct drm_plane     *plane = crtc->primary;

    (void)old_state;

    /*
     * On the initial commit the atomic core also ran mode_set, which already
     * scanned this fb out; skip the redundant full-frame copy.
     */
    if (plane && plane->state && plane->state->fb && vdev->current_fb != plane->state->fb) (void)vmfgfx_kms_scanout(vdev, plane->state->fb);
    if (crtc->state && crtc->state->event) {
        drm_crtc_send_vblank_event(crtc, crtc->state->event);
        crtc->state->event = NULL;
    }
}

/* Disable the CRTC scanout. */
static void vmfgfx_kms_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct vmfgfx_device *vdev = (struct vmfgfx_device *)crtc->dev->dev_private;

    (void)old_state;
    vdev->current_fb = NULL;
}

static const struct drm_crtc_helper_funcs vmfgfx_crtc_helpers = {
    .mode_set       = vmfgfx_kms_crtc_mode_set,
    .page_flip      = vmfgfx_kms_crtc_page_flip,
    .cursor_set     = vmfgfx_kms_crtc_cursor_set,
    .cursor_move    = vmfgfx_kms_crtc_cursor_move,
    .atomic_enable  = vmfgfx_kms_crtc_atomic_enable,
    .atomic_disable = vmfgfx_kms_crtc_atomic_disable,
};

/* A VM display is present whenever the controller probed. */
static enum drm_connector_status vmfgfx_kms_connector_detect(struct drm_connector *connector, bool force)
{
    (void)connector;
    (void)force;
    return connector_status_connected;
}

/* Add one mode with generic 60 Hz timings. */
static int vmfgfx_kms_add_mode(struct drm_connector *connector, uint32_t width, uint32_t height, bool preferred)
{
    struct drm_display_mode *mode;

    mode = drm_mode_create(connector->dev);
    if (!mode) return -ENOMEM;

    (void)snprintf(mode->name, DRM_DISPLAY_MODE_LEN - 1, "%ux%u", width, height);
    mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
    mode->hdisplay                       = (int)width;
    mode->hsync_start                    = (int)width + 80;
    mode->hsync_end                      = (int)width + 160;
    mode->htotal                         = (int)width + 320;
    mode->vdisplay                       = (int)height;
    mode->vsync_start                    = (int)height + 3;
    mode->vsync_end                      = (int)height + 6;
    mode->vtotal                         = (int)height + 32;
    mode->vrefresh                       = 60;
    mode->clock                          = (int)((uint64_t)mode->htotal * (uint64_t)mode->vtotal * (uint64_t)mode->vrefresh / 1000ULL);
    mode->flags                          = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type                           = (preferred ? DRM_MODE_TYPE_PREFERRED : 0) | DRM_MODE_TYPE_DRIVER;
    mode->status                         = MODE_OK;

    drm_mode_probed_add(connector, mode);
    return 1;
}

/*
 * The device reports no EDID, so the mode list is built by the driver.  The
 * mode the controller is already programmed with comes first and is the
 * preferred one, which keeps the console on the geometry it booted with.
 */
static int vmfgfx_kms_connector_get_modes(struct drm_connector *connector)
{
    struct vmfgfx_device *vdev  = (struct vmfgfx_device *)connector->dev->dev_private;
    int                   count = 0;
    int                   ret;

    if (vdev->mode_width && vdev->mode_height && vmfgfx_mode_supported(vdev, vdev->mode_width, vdev->mode_height)) {
        ret = vmfgfx_kms_add_mode(connector, vdev->mode_width, vdev->mode_height, true);
        if (ret < 0) return ret;
        count += ret;
    }

    for (size_t i = 0; i < sizeof(vmfgfx_mode_list) / sizeof(vmfgfx_mode_list[0]); i++) {
        uint32_t width  = vmfgfx_mode_list[i].width;
        uint32_t height = vmfgfx_mode_list[i].height;

        if (width == vdev->mode_width && height == vdev->mode_height) continue;
        if (!vmfgfx_mode_supported(vdev, width, height)) continue;

        ret = vmfgfx_kms_add_mode(connector, width, height, false);
        if (ret < 0) return ret;
        count += ret;
    }

    return count;
}

/* Reject anything the device cannot hold in video RAM at 32 bits per pixel. */
static enum drm_mode_status vmfgfx_kms_connector_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode)
{
    struct vmfgfx_device *vdev = (struct vmfgfx_device *)connector->dev->dev_private;

    if (!vmfgfx_mode_supported(vdev, (uint32_t)mode->hdisplay, (uint32_t)mode->vdisplay)) return MODE_BAD;
    return MODE_OK;
}

static const struct drm_connector_helper_funcs vmfgfx_conn_helpers = {
    .detect     = vmfgfx_kms_connector_detect,
    .get_modes  = vmfgfx_kms_connector_get_modes,
    .mode_valid = vmfgfx_kms_connector_mode_valid,
};

/* The encoder imposes no extra atomic constraints. */
static void vmfgfx_kms_encoder_atomic_check(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state, struct drm_connector_state *conn_state)
{
    (void)encoder;
    (void)crtc_state;
    (void)conn_state;
}

static const struct drm_encoder_helper_funcs vmfgfx_enc_helpers = {
    .atomic_mode_set = vmfgfx_kms_encoder_atomic_check,
};

/* Set up the software-scanout KMS pipeline: plane, CRTC, encoder, connector. */
int vmfgfx_kms_init(struct vmfgfx_device *vdev)
{
    struct drm_device    *dev;
    struct drm_crtc      *crtc;
    struct drm_plane     *primary;
    struct drm_encoder   *encoder;
    struct drm_connector *connector;
    int                   ret;

    if (!vdev || !vdev->drm) return -EINVAL;
    dev = vdev->drm;

    /* Primary plane */

    primary = malloc(sizeof(*primary));
    if (!primary) return -ENOMEM;
    memset(primary, 0, sizeof(*primary));
    vdev->primary = primary;

    ret = drm_plane_init(dev, primary, 1, NULL, vmfgfx_formats, sizeof(vmfgfx_formats) / sizeof(vmfgfx_formats[0]), NULL, DRM_PLANE_TYPE_PRIMARY, "vmfgfx-primary");
    if (ret) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        free(primary);
        vdev->primary = NULL;
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
    vdev->crtc = crtc;

    ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL, (void *)&vmfgfx_crtc_helpers, "vmfgfx-crtc-0");
    if (ret) {
        DRM_ERROR("Failed to init CRTC: %d\n", ret);
        free(crtc);
        vdev->crtc = NULL;
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
    vdev->encoder = encoder;

    ret = drm_encoder_init(dev, encoder, (void *)&vmfgfx_enc_helpers, DRM_MODE_ENCODER_DAC, "vmfgfx-encoder-0");
    if (ret) {
        DRM_ERROR("Failed to init encoder: %d\n", ret);
        free(encoder);
        vdev->encoder = NULL;
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
    vdev->connector = connector;

    ret = drm_connector_init(dev, connector, (void *)&vmfgfx_conn_helpers, DRM_MODE_CONNECTOR_VGA);
    if (ret) {
        DRM_ERROR("Failed to init connector: %d\n", ret);
        free(connector);
        vdev->connector = NULL;
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

    ret = vmfgfx_kms_connector_get_modes(connector);
    if (ret <= 0) {
        DRM_ERROR("Failed to add modes: %d\n", ret);
        return -EINVAL;
    }

    ret = drm_vblank_init(dev, 1);
    if (ret) {
        DRM_ERROR("Failed to init vblank: %d\n", ret);
        return ret;
    }

    drm_connector_register(connector);

    /*
     * The connector advertises every mode the controller can hold, so the
     * mode_config bounds are the device limits rather than a fixed mode.
     */
    dev->mode_config.min_width  = VMFGFX_MIN_WIDTH;
    dev->mode_config.min_height = VMFGFX_MIN_HEIGHT;
    dev->mode_config.max_width  = vdev->max_width;
    dev->mode_config.max_height = vdev->max_height;

    DRM_INFO("vmfgfx: KMS pipeline up, %ux%u..%ux%u\n", dev->mode_config.min_width, dev->mode_config.min_height, dev->mode_config.max_width, dev->mode_config.max_height);
    return 0;
}

/* Publish CRTC/plane state and switch the console onto the framebuffer. */
static int vmfgfx_kms_initial_commit(struct vmfgfx_device *vdev, struct drm_connector *conn, struct drm_display_mode *mode, struct drm_framebuffer *fb)
{
    struct drm_device *dev     = vdev->drm;
    struct drm_crtc   *crtc    = conn->state ? conn->state->crtc : vdev->crtc;
    struct drm_plane  *primary = crtc ? crtc->primary : vdev->primary;
    uint32_t           w       = (uint32_t)mode->hdisplay;
    uint32_t           h       = (uint32_t)mode->vdisplay;
    int                ret;

    /* mode_set never ran for this commit: program the device here. */
    ret = vmfgfx_set_mode(vdev, w, h);
    if (ret) return ret;

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
     * Publish the framebuffer before handing the console over: the handoff
     * copies the old frame across when the geometry is unchanged, and the
     * first flush must already have a scanout source to copy from.  Only
     * afterwards move that content into video RAM, so nothing on screen is
     * ever overwritten before it has been saved.
     */
    vdev->current_fb      = fb;
    vmfgfx_flush_dev      = vdev;
    dev->fb_console_flush = vmfgfx_kms_flush_fb;
    drm_kms_console_handoff(dev, fb);

    ret = vmfgfx_scanout_rect(vdev, fb, 0, 0, w, h);
    if (ret) DRM_WARN("initial scanout of %ux%u failed: %d\n", w, h, ret);

    DRM_INFO("Initial modeset: %ux%u fb=%u crtc=%u\n", w, h, fb->base.id, crtc ? crtc->base.id : 0);
    return 0;
}

/*
 * Initial modeset: give the console a framebuffer of its own at the mode
 * the controller is already showing.  Non-fatal on failure, so a display
 * that cannot be handed over still leaves a working DRM device behind.
 */
int vmfgfx_kms_initial_modeset(struct vmfgfx_device *vdev)
{
    struct drm_device       *dev;
    struct drm_connector    *conn;
    struct drm_display_mode *mode = NULL;
    struct drm_gem_object   *obj;
    struct drm_framebuffer  *fb;
    ilist_node_t            *node;
    uint32_t                 width, height, pitch;
    size_t                   size;
    int                      ret;

    if (!vdev || !vdev->drm || !vdev->connector) return -ENODEV;
    dev  = vdev->drm;
    conn = vdev->connector;
    if (conn->status != connector_status_connected) return -ENODEV;

    for (node = conn->modes.next; node && node != &conn->modes; node = node->next) {
        struct drm_display_mode *candidate = container_of(node, struct drm_display_mode, head);

        if (candidate->status != MODE_OK) continue;
        if (!mode) mode = candidate;
        if (candidate->type & DRM_MODE_TYPE_PREFERRED) {
            mode = candidate;
            break;
        }
    }
    if (!mode) return -ENODEV;

    width  = (uint32_t)mode->hdisplay;
    height = (uint32_t)mode->vdisplay;
    pitch  = width * VMFGFX_BYTES_PP;
    size   = (size_t)pitch * height;

    obj = vmfgfx_gem_create(dev, size);
    if (!obj) {
        DRM_ERROR("out of memory allocating the console framebuffer.\n");
        return -ENOMEM;
    }

    fb = malloc(sizeof(*fb));
    if (!fb) {
        drm_gem_object_put(obj);
        return -ENOMEM;
    }
    memset(fb, 0, sizeof(*fb));

    fb->format     = DRM_FORMAT_XRGB8888;
    fb->modifier   = DRM_FORMAT_MOD_LINEAR;
    fb->width      = width;
    fb->height     = height;
    fb->pitches[0] = pitch;
    fb->offsets[0] = 0;
    fb->obj[0]     = obj;

    ret = drm_framebuffer_init(dev, fb, &vmfgfx_fb_funcs);
    if (ret) {
        /* cleanup's drm_gem_object_put() frees obj when its refcount drops. */
        free(fb);
        drm_gem_object_put(obj);
        return ret;
    }

    ret = vmfgfx_kms_initial_commit(vdev, conn, mode, fb);
    if (ret) {
        drm_framebuffer_cleanup(fb);
        drm_framebuffer_put(fb);
        return ret;
    }
    return 0;
}

/* Drop the console flush hooks and free the driver-owned KMS objects. */
void vmfgfx_kms_cleanup(struct vmfgfx_device *vdev)
{
    if (!vdev) return;

    /* The framebuffers and GEM objects belong to drm_mode_config_cleanup(). */
    vmfgfx_flush_dev = NULL;
    vdev->current_fb = NULL;
    if (vdev->drm) vdev->drm->fb_console_flush = NULL;

    if (vdev->connector) {
        free(vdev->connector);
        vdev->connector = NULL;
    }
    if (vdev->encoder) {
        free(vdev->encoder);
        vdev->encoder = NULL;
    }
    if (vdev->crtc) {
        free(vdev->crtc);
        vdev->crtc = NULL;
    }
    if (vdev->primary) {
        free(vdev->primary);
        vdev->primary = NULL;
    }
}

#endif
