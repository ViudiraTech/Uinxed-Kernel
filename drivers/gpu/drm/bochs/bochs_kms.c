/*
 *
 *      bochs_kms.c
 *      bochs dispi vga KMS display pipeline
 *
 *      2026/11/1 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <drivers/gpu/drm/bochs/bochs_drv.h>
#include <drivers/gpu/drm/drm_edid.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/fbdev/video.h>
#include <mem/page.h>

#if CONFIG_BOCHS && CONFIG_DRM

/* clang-format off */

/* Standard 60 Hz ladder; the configured console mode leads. */
static const struct bochs_reference_mode {
        uint16_t width;
        uint16_t height;
} bochs_reference_modes[BOCHS_LADDER_MODE_COUNT] = {
    {(uint16_t)CONFIG_DRM_DEFAULT_WIDTH, (uint16_t)CONFIG_DRM_DEFAULT_HEIGHT},
    { 640,  480},
    { 800,  600},
    {1024,  600},
    {1152,  864},
    {1280,  720},
    {1280,  800},
    {1280,  960},
    {1280, 1024},
    {1360,  768},
    {1440,  900},
    {1600,  900},
    {1600, 1200},
    {1680, 1050},
    {1920, 1080},
    {1920, 1200},
    {2560, 1440},
    {2560, 1600},
};

/* clang-format on */

/* True when the adapter can scan out @width x @height at 32 bpp. */
static bool bochs_mode_scannable(const struct bochs_device *bdev, uint32_t width, uint32_t height)
{
    if (!width || !height || width > BOCHS_MAX_DIMENSION || height > BOCHS_MAX_DIMENSION) return false;

    /* VBE cannot scan out a width that is not a multiple of 8. */
    if (width % 8) return false;

    /* Skip anything the adapter cannot hold in VRAM. */
    return (uint64_t)width * sizeof(uint32_t) * height <= bdev->fb_size;
}

/* Describe @width x @height with a synthetic 60 Hz timing. */
static void bochs_mode_fill(struct drm_display_mode *mode, uint32_t width, uint32_t height)
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
    mode->vrefresh                       = 60;
    mode->clock                          = (int)((uint64_t)mode->htotal * (uint64_t)mode->vtotal * 60ULL / 1000ULL);
    mode->flags                          = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type                           = DRM_MODE_TYPE_DRIVER;
    mode->status                         = MODE_OK;
}

/* Pixel area of a mode, used to rank EDID fallbacks. */
static uint64_t bochs_mode_area(const struct drm_display_mode *mode)
{
    return (uint64_t)(uint32_t)mode->hdisplay * (uint32_t)mode->vdisplay;
}

/* Look up the standard 60 Hz timing for a size, else describe one. */
static struct drm_display_mode *bochs_mode_create(struct drm_device *dev, uint32_t width, uint32_t height)
{
    struct drm_display_mode *mode;

    mode = drm_mode_find_dmt(dev, (int)width, (int)height, 60, false);
    if (mode) return mode;

    /* No DMT entry: use the same synthetic 60 Hz timing as the other KMS drivers. */
    mode = drm_mode_create(dev);
    if (!mode) return NULL;

    bochs_mode_fill(mode, width, height);
    return mode;
}

/* Resolution the bootloader left on the adapter; keep the console layout. */
static void bochs_boot_geometry(uint32_t *width, uint32_t *height, uint32_t *pitch)
{
    struct limine_framebuffer *framebuffer;

    *width  = 0;
    *height = 0;
    *pitch  = 0;

    framebuffer = get_framebuffer();
    if (!framebuffer || !framebuffer->address || framebuffer->bpp != BOCHS_BPP) return;
    if (!framebuffer->width || !framebuffer->height) return;
    if (framebuffer->pitch < framebuffer->width * sizeof(uint32_t)) return;

    *width  = (uint32_t)framebuffer->width;
    *height = (uint32_t)framebuffer->height;
    *pitch  = (uint32_t)framebuffer->pitch;
}

/* Drop the published modes so probing again cannot duplicate the list. */
static void bochs_connector_reset_modes(struct drm_connector *connector)
{
    while (connector->modes.next && connector->modes.next != &connector->modes) {
        struct drm_display_mode *mode = container_of(connector->modes.next, struct drm_display_mode, head);
        drm_mode_destroy(connector->dev, mode);
    }
}

/* Publish one mode; returns 1 when it was added. */
static int bochs_connector_add_mode(struct bochs_device *bdev, struct drm_connector *connector, uint32_t width, uint32_t height)
{
    struct drm_display_mode *mode;
    ilist_node_t            *node;

    if (!bochs_mode_scannable(bdev, width, height)) return 0;

    /* The EDID may already list this geometry; publish it once. */
    for (node = connector->modes.next; node && node != &connector->modes; node = node->next) {
        struct drm_display_mode *existing = container_of(node, struct drm_display_mode, head);
        if ((uint32_t)existing->hdisplay == width && (uint32_t)existing->vdisplay == height) return 0;
    }

    mode = bochs_mode_create(connector->dev, width, height);
    if (!mode) return 0;

    drm_mode_probed_add(connector, mode);
    return 1;
}

/* Publish the standard 60 Hz ladder. */
static int bochs_connector_add_reference_modes(struct bochs_device *bdev, struct drm_connector *connector)
{
    const int count = (int)(sizeof(bochs_reference_modes) / sizeof(bochs_reference_modes[0]));
    int       added = 0;
    int       i;

    for (i = 0; i < count; i++) added += bochs_connector_add_mode(bdev, connector, bochs_reference_modes[i].width, bochs_reference_modes[i].height);
    return added;
}

/* First mode of the connector that drives @width x @height, or NULL. */
static struct drm_display_mode *bochs_connector_find_geometry(struct drm_connector *connector, uint32_t width, uint32_t height)
{
    ilist_node_t *node;

    if (!width || !height) return NULL;
    for (node = connector->modes.next; node && node != &connector->modes; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        if ((uint32_t)mode->hdisplay == width && (uint32_t)mode->vdisplay == height) return mode;
    }

    return NULL;
}

/* Make @preferred the only DRM_MODE_TYPE_PREFERRED mode of the connector. */
static void bochs_connector_set_preferred(struct drm_connector *connector, struct drm_display_mode *preferred)
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

/* The mode the monitor advertised as preferred, or NULL. */
static struct drm_display_mode *bochs_connector_find_preferred(struct drm_connector *connector)
{
    ilist_node_t *node;

    for (node = connector->modes.next; node && node != &connector->modes; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        if (mode->type & DRM_MODE_TYPE_PREFERRED) return mode;
    }

    return NULL;
}

/* Drop EDID modes the adapter cannot scan out; returns the number removed. */
static int bochs_connector_drop_unscannable_modes(struct bochs_device *bdev, struct drm_connector *connector)
{
    ilist_node_t *node    = connector->modes.next;
    int           dropped = 0;

    while (node && node != &connector->modes) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        node                          = node->next;
        if (!bochs_mode_scannable(bdev, (uint32_t)mode->hdisplay, (uint32_t)mode->vdisplay)) {
            drm_mode_destroy(connector->dev, mode);
            dropped++;
        }
    }

    return dropped;
}

/* Publish the monitor EDID and its modes; returns the number of modes added. */
static int bochs_connector_probe_edid(struct bochs_device *bdev, struct drm_connector *connector)
{
    struct edid  *edid;
    ilist_node_t *node;
    int           size;
    int           count;

    edid = malloc(BOCHS_EDID_MAX);
    if (!edid) return -ENOMEM;

    size = bochs_edid_read(bdev, edid, BOCHS_EDID_MAX);

    /* A missing EDID is normal; pass malformed data up so the connector owner reports it. */
    if (size <= 0 || !drm_edid_is_valid(edid)) {
        int ret = size == -ENODEV ? 0 : (size < 0 ? size : -EINVAL);

        free(edid);

        /* Drop a stale property before falling back to the built-in modes. */
        (void)drm_connector_update_edid_property(connector, NULL, 0);
        drm_edid_to_display_info(connector, NULL);
        return ret;
    }

    /* The blob owns a persistent copy of the EDID bytes. */
    if (drm_connector_update_edid_property(connector, (const unsigned char *)edid, (size_t)size)) {
        free(edid);
        return -ENOMEM;
    }
    if (connector->edid_blob && connector->edid_blob->data) connector->edid_blob_ptr = (uint8_t *)connector->edid_blob->data;

    drm_edid_to_display_info(connector, edid);
    count = drm_add_edid_modes(connector, edid);
    count -= bochs_connector_drop_unscannable_modes(bdev, connector);

    /* Keep a flagged timing first so the cap cannot drop the monitor's preferred mode. */
    for (node = connector->modes.next; node && node != &connector->modes && bdev->edid_mode_count < BOCHS_MAX_EDID_MODES; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        if (mode->type & DRM_MODE_TYPE_PREFERRED) bdev->edid_modes[bdev->edid_mode_count++] = mode;
    }
    for (node = connector->modes.next; node && node != &connector->modes && bdev->edid_mode_count < BOCHS_MAX_EDID_MODES; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        if (!(mode->type & DRM_MODE_TYPE_PREFERRED)) bdev->edid_modes[bdev->edid_mode_count++] = mode;
    }
    if (count > 0) {
        char name[14];
        drm_edid_get_monitor_name(edid, name, (int)sizeof(name));
        DRM_INFO("connector %s: EDID parsed, %d mode(s), monitor \"%s\"\n", connector->name, count, name[0] ? name : "unknown");
    }

    free(edid);
    return count;
}

/* The adapter always drives one output. */
static enum drm_connector_status bochs_connector_detect(struct drm_connector *connector, bool force)
{
    (void)connector;
    (void)force;
    return connector_status_connected;
}

/* Rebuild the mode list: EDID modes lead, else boot geometry, plus the 60 Hz ladder. */
static int bochs_connector_get_modes(struct drm_connector *connector)
{
    struct bochs_device     *bdev           = to_bochs_device(connector->dev);
    struct drm_display_mode *preferred      = NULL;
    const char              *preferred_from = NULL;
    uint32_t                 boot_width;
    uint32_t                 boot_height;
    uint32_t                 boot_pitch;
    int                      edid_modes;
    int                      count;
    int                      i;

    if (!bdev) return -EINVAL;

    /* The mode pointers die with the list rebuilt below. */
    bdev->edid_mode_count = 0;
    bochs_connector_reset_modes(connector);

    /* The scanout geometry the bootloader set up, when the adapter can hold it. */
    bochs_boot_geometry(&boot_width, &boot_height, &boot_pitch);
    if ((uint64_t)boot_pitch * boot_height > bdev->fb_size) boot_width = boot_height = 0;

    edid_modes = bochs_connector_probe_edid(bdev, connector);

    /* A missing EDID is normal; malformed data and allocation failures report here. */
    if (edid_modes < 0) {
        DRM_ERROR("connector %s: failed to probe EDID (%d), continuing without it.\n", connector->name, edid_modes);
        edid_modes = 0;
    }

    count = edid_modes;
    count += bochs_connector_add_mode(bdev, connector, boot_width, boot_height);
    count += bochs_connector_add_reference_modes(bdev, connector);

    /* Publishing a mode the adapter cannot scan would only fail later. */
    if (count <= 0) DRM_WARN("connector %s: no EDID and no mode fits VRAM.\n", connector->name);

    /* Pick one preferred mode; the boot log names its source. */
    if (edid_modes > 0) {
        preferred = bochs_connector_find_preferred(connector);
        if (preferred) preferred_from = "adapter EDID preferred timing";

        /* No flagged timing: the largest EDID mode leads, as in the boot chain. */
        if (!preferred) {
            for (i = 0; i < bdev->edid_mode_count; i++)
                if (!preferred || bochs_mode_area(bdev->edid_modes[i]) > bochs_mode_area(preferred)) preferred = bdev->edid_modes[i];
            if (preferred) preferred_from = "largest adapter EDID mode";
        }
    }
    if (!preferred) {
        preferred = bochs_connector_find_geometry(connector, boot_width, boot_height);
        if (preferred) preferred_from = "boot framebuffer geometry";
    }
    if (!preferred) {
        preferred = bochs_connector_find_geometry(connector, CONFIG_DRM_DEFAULT_WIDTH, CONFIG_DRM_DEFAULT_HEIGHT);
        if (preferred) preferred_from = "configured default";
    }
    if (!preferred && connector->modes.next != &connector->modes) {
        preferred      = container_of(connector->modes.next, struct drm_display_mode, head);
        preferred_from = "first mode in the list";
    }
    if (preferred) {
        bochs_connector_set_preferred(connector, preferred);
        DRM_INFO("connector %s: %d mode(s), preferred %dx%d from the %s\n", connector->name, count, preferred->hdisplay, preferred->vdisplay, preferred_from ? preferred_from : "list");
    }

    return 0;
}

/* Reject modes the adapter cannot hold. */
static int bochs_connector_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode)
{
    struct bochs_device *bdev = to_bochs_device(connector->dev);

    if (!bdev || !mode) return MODE_BAD;
    if (mode->hdisplay <= 0 || mode->vdisplay <= 0) return MODE_BAD;
    if (mode->hdisplay > BOCHS_MAX_DIMENSION || mode->vdisplay > BOCHS_MAX_DIMENSION) return MODE_BAD;

    /* VBE cannot scan out a width that is not a multiple of 8. */
    if (mode->hdisplay % 8) return MODE_BAD;

    /* A framebuffer of this size has to fit into VRAM. */
    if ((uint64_t)mode->hdisplay * sizeof(uint32_t) * (uint64_t)mode->vdisplay > bdev->fb_size) return MODE_MEM;

    return MODE_OK;
}

static const struct drm_connector_helper_funcs bochs_conn_helpers = {
    .detect     = bochs_connector_detect,
    .get_modes  = bochs_connector_get_modes,
    .mode_valid = bochs_connector_mode_valid,
};

/* The adapter has no separate encoder state to program. */
static void bochs_encoder_atomic_mode_set(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state, struct drm_connector_state *conn_state)
{
    (void)encoder;
    (void)crtc_state;
    (void)conn_state;
}

static const struct drm_encoder_helper_funcs bochs_enc_helpers = {
    .atomic_mode_set = bochs_encoder_atomic_mode_set,
};

/* Blank the output only when @crtc's committed primary still owns the live scanout. */
static void bochs_blank_committed_scanout(struct bochs_device *bdev, struct drm_crtc *crtc)
{
    struct drm_framebuffer *committed = (crtc->primary && crtc->primary->state) ? crtc->primary->state->fb : NULL;

    /* The console or another client may own the live scanout; leave it alone then. */
    if (!bdev->current_fb || bdev->current_fb != committed) return;
    (void)bochs_scanout_fb(bdev, NULL);
    bochs_hw_blank(bdev, true);
}

/* Program the mode; without a scaler the framebuffer size is the scanout. */
static int bochs_crtc_mode_set(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    struct bochs_device *bdev = to_bochs_device(crtc->dev);
    int                  ret;

    if (!bdev) return -EINVAL;

    /* A modeset that also disables the primary plane leaves nothing to scan out. */
    if (!fb) {
        bochs_blank_committed_scanout(bdev, crtc);
        return 0;
    }
    if (!bochs_scanout_supported(bdev, fb)) {
        DRM_ERROR("mode set: %ux%u framebuffer is not scanout capable.\n", fb->width, fb->height);
        return -EINVAL;
    }

    bochs_hw_setmode(bdev, fb);
    bochs_hw_setformat(bdev, fb->format);

    ret = bochs_scanout_fb(bdev, fb);
    if (ret) return ret;

    /* A previous plane disable blanked the output; show this frame. */
    bochs_hw_blank(bdev, false);

    /* The upload painted over any pointer that was on screen. */
    bochs_cursor_repaint(bdev, true);

    return 0;
}

/* Push a new framebuffer to the scanout (legacy ioctl and atomic fb-only commits). */
static int bochs_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb, struct drm_pending_vblank_event *event, uint32_t flags)
{
    struct bochs_device *bdev = to_bochs_device(crtc->dev);
    int                  ret;

    (void)event;
    (void)flags;

    if (!bdev) return -EINVAL;
    if (fb) {
        /* A flip must not silently change the mode the CRTC reports. */
        if (crtc->mode.hdisplay && ((uint32_t)crtc->mode.hdisplay != fb->width || (uint32_t)crtc->mode.vdisplay != fb->height)) return -EINVAL;
        if (!bochs_scanout_supported(bdev, fb)) return -EINVAL;

        /* A flip may change the geometry; keep the adapter in step with it. */
        if (fb->width != bdev->xres || fb->height != bdev->yres || fb->pitches[0] != bdev->stride) bochs_hw_setmode(bdev, fb);
        bochs_hw_setformat(bdev, fb->format);

        ret = bochs_scanout_fb(bdev, fb);
        if (ret) return ret;

        /* A previous plane disable blanked the output; show this frame. */
        bochs_hw_blank(bdev, false);

        /* The upload painted over any pointer that was on screen. */
        bochs_cursor_repaint(bdev, true);
    } else {
        bochs_blank_committed_scanout(bdev, crtc);
    }

    /* The legacy ioctl commits the plane; move the reference before the pin drops. */
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

/* Enable the output; reprogram only when the committed fb is not live. */
static void bochs_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct bochs_device    *bdev = to_bochs_device(crtc->dev);
    struct drm_framebuffer *fb   = (crtc->primary && crtc->primary->state) ? crtc->primary->state->fb : NULL;
    (void)old_state;

    if (!bdev) return;

    /* An active CRTC without a framebuffer has only stale VRAM to show. */
    if (!fb) {
        bochs_hw_blank(bdev, true);
        return;
    }
    if (bdev->current_fb != fb || fb->width != bdev->xres || fb->height != bdev->yres || fb->pitches[0] != bdev->stride) {
        if (bochs_crtc_mode_set(crtc, fb)) return;
    }

    bochs_hw_blank(bdev, false);
}

/* Blank the output when the CRTC goes inactive. */
static int bochs_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    struct bochs_device *bdev = to_bochs_device(crtc->dev);
    (void)old_state;

    if (!bdev) return -EINVAL;

    bochs_hw_blank(bdev, true);
    bdev->current_fb = NULL;

    return 0;
}

static const struct drm_crtc_helper_funcs bochs_crtc_helpers = {
    .mode_set       = bochs_crtc_mode_set,
    .page_flip      = bochs_crtc_page_flip,
    .cursor_set     = bochs_cursor_set,
    .cursor_move    = bochs_cursor_move,
    .atomic_enable  = bochs_crtc_atomic_enable,
    .atomic_disable = bochs_crtc_atomic_disable,
};

/* Reject framebuffers the adapter cannot scan out before the commit starts. */
static int bochs_plane_atomic_check(struct drm_plane *plane, struct drm_plane_state *state)
{
    struct bochs_device   *bdev       = to_bochs_device(plane->dev);
    struct drm_crtc_state *crtc_state = state->crtc_state;

    if (!bdev) return -ENXIO;
    if (!state->fb) return 0;
    if (!bochs_scanout_supported(bdev, state->fb)) return -EINVAL;

    /* The VBE scanout starts at the buffer origin at 1:1; cropping or panning cannot be honoured. */
    if (!drm_plane_state_full_frame(state->fb, state)) return -EINVAL;

    /* The VBE scanout has no scaler, so a mode and its framebuffer must agree. */
    if (crtc_state && crtc_state->active && ((uint32_t)crtc_state->mode.hdisplay != state->fb->width || (uint32_t)crtc_state->mode.vdisplay != state->fb->height)) return -EINVAL;
    return 0;
}

static const struct drm_plane_helper_funcs bochs_plane_helpers = {
    .atomic_check = bochs_plane_atomic_check,
};

/* Allocate a console framebuffer for @mode; released by the caller on failure. */
static int bochs_kms_initial_try_mode(struct bochs_device *bdev, struct drm_display_mode *mode, struct drm_framebuffer **out_fb)
{
    struct drm_device      *dev = bdev->drm_dev;
    struct drm_framebuffer *fb;
    struct drm_gem_object  *obj;
    uint32_t                width  = (uint32_t)mode->hdisplay;
    uint32_t                height = (uint32_t)mode->vdisplay;
    size_t                  pitch;
    size_t                  size;
    int                     ret;

    *out_fb = NULL;

    /* The VBE engine and the scanout stride both require a multiple-of-8 width. */
    if (!width || !height || width % 8 || width > BOCHS_MAX_DIMENSION || height > BOCHS_MAX_DIMENSION) return -EINVAL;

    pitch = (size_t)width * sizeof(uint32_t);
    size  = pitch * height;
    if (size > bdev->fb_size || size > UINT32_MAX) return -EINVAL;

    obj = malloc(sizeof(*obj));
    if (!obj) return -ENOMEM;

    memset(obj, 0, sizeof(*obj));
    if (drm_gem_object_init(dev, obj, size)) {
        free(obj);
        return -EINVAL;
    }

    obj->size     = (uint32_t)size;
    obj->prime_fd = -1;

    /* The kernel console renders straight into this page-aligned system buffer. */
    obj->backing = aligned_alloc(PAGE_4K_SIZE, ALIGN_UP(size, PAGE_4K_SIZE));
    if (!obj->backing) {
        free(obj);
        return -ENOMEM;
    }
    memset(obj->backing, 0, ALIGN_UP(size, PAGE_4K_SIZE));

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
    fb->pitches[0] = (unsigned int)pitch;
    fb->offsets[0] = 0;
    fb->obj[0]     = obj;

    ret = drm_framebuffer_init(dev, fb, &bochs_fb_funcs);
    if (ret) {
        /* Not registered, so nothing else dropped the GEM reference yet. */
        free(fb);
        drm_gem_object_put(obj);
        return ret;
    }

    *out_fb = fb;
    return 0;
}

/* Seed @fb with the boot frame; the first upload would otherwise blank the console. */
static void bochs_fb_seed_from_boot(struct drm_framebuffer *fb)
{
    struct limine_framebuffer *boot;
    const uint8_t             *src;
    uint8_t                   *dst;
    uint32_t                   copy_width;
    uint32_t                   copy_height;
    uint32_t                   y;
    if (!fb || !fb->obj[0] || !fb->obj[0]->backing) return;

    boot = get_framebuffer();
    if (!boot || !boot->address || boot->bpp != BOCHS_BPP) return;
    if (!boot->width || !boot->height || boot->pitch < boot->width * sizeof(uint32_t)) return;

    copy_width  = boot->width < fb->width ? (uint32_t)boot->width : fb->width;
    copy_height = boot->height < fb->height ? (uint32_t)boot->height : fb->height;
    if (!copy_width || !copy_height) return;

    src = (const uint8_t *)boot->address;
    dst = (uint8_t *)fb->obj[0]->backing;
    for (y = 0; y < copy_height; y++) memcpy(dst + (size_t)y * fb->pitches[0], src + (size_t)y * boot->pitch, (size_t)copy_width * sizeof(uint32_t));
}

/* Publish the boot mode to the DRM state and hand the console over to it. */
static int bochs_kms_initial_commit(struct bochs_device *bdev, struct drm_connector *connector, struct drm_display_mode *mode, struct drm_framebuffer *fb)
{
    struct drm_device *dev     = bdev->drm_dev;
    struct drm_crtc   *crtc    = connector->state ? connector->state->crtc : NULL;
    struct drm_plane  *primary = crtc ? crtc->primary : NULL;
    uint32_t           width   = (uint32_t)mode->hdisplay;
    uint32_t           height  = (uint32_t)mode->vdisplay;

    /* Carry the boot screen into the console framebuffer before VRAM is reused. */
    bochs_fb_seed_from_boot(fb);

    /* Program the adapter before the mode becomes visible to userspace. */
    bochs_hw_setmode(bdev, fb);
    bochs_hw_setformat(bdev, fb->format);
    if (bochs_scanout_fb(bdev, fb)) return -EIO;

    /* Lift a blank the firmware or a previous owner may have left behind. */
    bochs_hw_blank(bdev, false);

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
            primary->state->src.x2  = (int32_t)(width << 16);
            primary->state->src.y2  = (int32_t)(height << 16);
            primary->state->dst.x1  = 0;
            primary->state->dst.y1  = 0;
            primary->state->dst.x2  = (int32_t)width;
            primary->state->dst.y2  = (int32_t)height;
            primary->state->visible = true;
        }
        primary->fb_id   = fb->base.id;
        primary->crtc_id = crtc ? crtc->base.id : 0;
    }
    if (connector->state) {
        connector->state->crtc         = crtc;
        connector->state->best_encoder = bdev->encoder;
    }

    /* Attach the console flush hook and hand the console to the DRM core. */
    bochs_console_attach(bdev, fb);
    drm_kms_console_handoff(dev, fb);

    DRM_INFO("initial modeset: %ux%u fb=%d crtc=%d\n", (unsigned)width, (unsigned)height, fb->base.id, crtc ? crtc->base.id : 0);
    return 0;
}

/* Sort EDID modes from the largest pixel area down. */
static void bochs_edid_sort(struct drm_display_mode **modes, int count)
{
    int i;
    int j;

    for (i = 1; i < count; i++) {
        struct drm_display_mode *key = modes[i];
        for (j = i - 1; j >= 0 && bochs_mode_area(modes[j]) < bochs_mode_area(key); j--) modes[j + 1] = modes[j];
        modes[j + 1] = key;
    }
}

/* The EDID preferred mode, else the largest one; @modes must be sorted. */
static struct drm_display_mode *bochs_edid_preferred(struct drm_display_mode **modes, int count)
{
    int i;

    for (i = 0; i < count; i++)
        if (modes[i]->type & DRM_MODE_TYPE_PREFERRED) return modes[i];
    return count ? modes[0] : NULL;
}

/* True when @width x @height was already attempted. */
static bool bochs_geometry_tried(const uint32_t (*tried)[2], unsigned count, uint32_t width, uint32_t height)
{
    unsigned i;

    for (i = 0; i < count; i++)
        if (tried[i][0] == width && tried[i][1] == height) return true;
    return false;
}

/* Try one mode for the console, remembering its geometry. */
static int bochs_kms_try_mode(struct bochs_device *bdev, struct drm_connector *connector, struct drm_display_mode *mode, uint32_t (*tried)[2], unsigned *tried_count, int *last_ret)
{
    struct drm_framebuffer *fb;
    uint32_t                width  = (uint32_t)mode->hdisplay;
    uint32_t                height = (uint32_t)mode->vdisplay;
    int                     ret;

    if (!width || !height) {
        *last_ret = -EINVAL;
        return -EINVAL;
    }
    if (bochs_geometry_tried(tried, *tried_count, width, height)) return 1;
    if (*tried_count < BOCHS_MAX_TRIED_MODES) {
        tried[*tried_count][0] = width;
        tried[*tried_count][1] = height;
        (*tried_count)++;
    }

    ret = bochs_kms_initial_try_mode(bdev, mode, &fb);
    if (ret) {
        *last_ret = ret;
        return ret;
    }

    ret = bochs_kms_initial_commit(bdev, connector, mode, fb);
    if (!ret) return 0;

    /* The failed commit took no reference; unregister and release the buffer. */
    drm_framebuffer_cleanup(fb);
    drm_framebuffer_put(fb);

    *last_ret = ret;
    return ret;
}

/* Try a synthetic mode of the given geometry. */
static int bochs_kms_try_geometry(struct bochs_device *bdev, struct drm_connector *connector, uint32_t width, uint32_t height, uint32_t (*tried)[2], unsigned *tried_count, int *last_ret)
{
    struct drm_display_mode mode;

    if (!width || !height || width > BOCHS_MAX_DIMENSION || height > BOCHS_MAX_DIMENSION) {
        *last_ret = -EINVAL;
        return -EINVAL;
    }

    bochs_mode_fill(&mode, width, height);
    return bochs_kms_try_mode(bdev, connector, &mode, tried, tried_count, last_ret);
}

/* Put the adapter back on the bootloader mode so simpledrm can keep the console. */
static bool bochs_restore_boot_mode(struct bochs_device *bdev)
{
    struct limine_framebuffer *boot;
    struct drm_framebuffer     fb;

    bdev->current_fb = NULL;
    boot             = get_framebuffer();

    if (!boot || !boot->address || boot->bpp != BOCHS_BPP || !boot->width || !boot->height) return false;
    if (boot->width > BOCHS_MAX_DIMENSION || boot->height > BOCHS_MAX_DIMENSION) return false;
    if (boot->pitch < boot->width * sizeof(uint32_t)) return false;
    if ((uint64_t)boot->pitch * boot->height > bdev->fb_size) return false;
    if (boot->pitch / sizeof(uint32_t) > BOCHS_MAX_VIRT_WIDTH) return false;

    /* VBE rounds XRES and VIRT_WIDTH down to 8-pixel steps. */
    if (boot->width % 8 || boot->pitch % (8 * sizeof(uint32_t))) return false;

    memset(&fb, 0, sizeof(fb));
    fb.width      = (uint32_t)boot->width;
    fb.height     = (uint32_t)boot->height;
    fb.pitches[0] = (unsigned int)boot->pitch;

    bochs_hw_setmode(bdev, &fb);
    bochs_hw_setformat(bdev, DRM_FORMAT_XRGB8888);
    bochs_hw_blank(bdev, false);

    return true;
}

/* Boot modeset in fallback order: EDID modes, boot geometry, configured default, reference ladder. */
static int bochs_kms_initial_modeset(struct bochs_device *bdev)
{
    struct drm_device       *dev    = bdev->drm_dev;
    struct drm_mode_config  *config = &dev->mode_config;
    struct drm_connector    *conn   = NULL;
    struct drm_display_mode *preferred;
    uint32_t                 boot_width;
    uint32_t                 boot_height;
    uint32_t                 boot_pitch;
    uint32_t                 tried[BOCHS_MAX_TRIED_MODES][2];
    unsigned                 tried_count = 0;
    ilist_node_t            *node;
    int                      last_ret = -ENODEV;
    int                      i;

    /* Find the connected connector; this adapter exposes exactly one. */
    for (node = config->connector_list.next; node != &config->connector_list; node = node->next) {
        conn = container_of(node, struct drm_connector, head);
        if (conn->status == connector_status_connected) break;
        conn = NULL;
    }
    if (!conn) {
        DRM_INFO("no connected connector, skipping the initial modeset.\n");
        return -ENODEV;
    }

    /* The EDID preferred mode, then every other EDID mode, largest first. */
    bochs_edid_sort(bdev->edid_modes, bdev->edid_mode_count);
    preferred = bochs_edid_preferred(bdev->edid_modes, bdev->edid_mode_count);

    if (preferred && !bochs_kms_try_mode(bdev, conn, preferred, tried, &tried_count, &last_ret)) return 0;
    for (i = 0; i < bdev->edid_mode_count; i++) {
        if (bdev->edid_modes[i] == preferred) continue;
        if (!bochs_kms_try_mode(bdev, conn, bdev->edid_modes[i], tried, &tried_count, &last_ret)) return 0;
    }

    /* The geometry the bootloader left on the adapter. */
    bochs_boot_geometry(&boot_width, &boot_height, &boot_pitch);
    if (!bochs_kms_try_geometry(bdev, conn, boot_width, boot_height, tried, &tried_count, &last_ret)) return 0;

    /* The configured console mode. */
    if (!bochs_kms_try_geometry(bdev, conn, CONFIG_DRM_DEFAULT_WIDTH, CONFIG_DRM_DEFAULT_HEIGHT, tried, &tried_count, &last_ret)) return 0;

    /* The rest of the ladder, smallest first, when the default is too large for VRAM. */
    for (i = 1; i < BOCHS_LADDER_MODE_COUNT; i++)
        if (!bochs_kms_try_geometry(bdev, conn, bochs_reference_modes[i].width, bochs_reference_modes[i].height, tried, &tried_count, &last_ret)) return 0;

    /* No mode worked; put the adapter back on the boot mode for the boot framebuffer. */
    DRM_WARN("initial modeset: no usable mode (%d)\n", last_ret);
    if (!bochs_restore_boot_mode(bdev)) DRM_WARN("initial modeset: cannot restore the boot mode.\n");

    return last_ret;
}

/* Build plane/CRTC/encoder/connector, probe modes and run the boot modeset. */
int bochs_kms_init(struct bochs_device *bdev)
{
    struct drm_device    *dev = bdev->drm_dev;
    struct drm_crtc      *crtc;
    struct drm_plane     *primary;
    struct drm_encoder   *encoder;
    struct drm_connector *connector;
    int                   ret;

    if (!dev) return -EINVAL;

    primary = malloc(sizeof(*primary));
    if (!primary) {
        DRM_ERROR("out of memory allocating primary plane.\n");
        return -ENOMEM;
    }
    memset(primary, 0, sizeof(*primary));
    bdev->primary = primary;

    {
        uint32_t formats[3];
        unsigned count = 0;

        formats[count++] = DRM_FORMAT_XRGB8888;
        formats[count++] = DRM_FORMAT_ARGB8888;

        /* Only the qext block can byte-swap a BGRX8888 scanout. */
        if (bdev->qext_size >= BOCHS_QEXT_MIN_SIZE) formats[count++] = DRM_FORMAT_BGRX8888;
        ret = drm_plane_init(dev, primary, 1, (void *)&bochs_plane_helpers, formats, count, NULL, DRM_PLANE_TYPE_PRIMARY, "bochs-primary");
    }
    if (ret) {
        DRM_ERROR("failed to init the primary plane: %d\n", ret);
        free(primary);
        bdev->primary = NULL;
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

    crtc = malloc(sizeof(*crtc));
    if (!crtc) {
        DRM_ERROR("out of memory allocating CRTC.\n");
        return -ENOMEM;
    }
    memset(crtc, 0, sizeof(*crtc));
    bdev->crtc = crtc;

    ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL, (void *)&bochs_crtc_helpers, "bochs-crtc-0");
    if (ret) {
        DRM_ERROR("failed to init the CRTC: %d\n", ret);
        free(crtc);
        bdev->crtc = NULL;
        return ret;
    }

    /* Software scanout: a flip is complete as soon as the upload returns. */
    dev->mode_config.async_page_flip = true;

    /* The driver composites a cursor itself, so advertise the cursor size. */
    dev->mode_config.cursor_width  = BOCHS_CURSOR_MAX;
    dev->mode_config.cursor_height = BOCHS_CURSOR_MAX;

    crtc->state = malloc(sizeof(*crtc->state));
    if (!crtc->state) {
        DRM_ERROR("out of memory allocating CRTC state.\n");
        return -ENOMEM;
    }
    memset(crtc->state, 0, sizeof(*crtc->state));
    crtc->state->crtc   = crtc;
    crtc->state->active = false;
    crtc->state->enable = false;

    encoder = malloc(sizeof(*encoder));
    if (!encoder) {
        DRM_ERROR("out of memory allocating encoder.\n");
        return -ENOMEM;
    }
    memset(encoder, 0, sizeof(*encoder));
    bdev->encoder = encoder;

    ret = drm_encoder_init(dev, encoder, (void *)&bochs_enc_helpers, DRM_MODE_ENCODER_VIRTUAL, "bochs-encoder-0");
    if (ret) {
        DRM_ERROR("failed to init the encoder: %d\n", ret);
        free(encoder);
        bdev->encoder = NULL;
        return ret;
    }
    encoder->possible_crtcs = 1;
    encoder->crtc           = crtc;

    connector = malloc(sizeof(*connector));
    if (!connector) {
        DRM_ERROR("out of memory allocating connector.\n");
        return -ENOMEM;
    }
    memset(connector, 0, sizeof(*connector));
    bdev->connector = connector;

    ret = drm_connector_init(dev, connector, (void *)&bochs_conn_helpers, DRM_MODE_CONNECTOR_VIRTUAL);
    if (ret) {
        DRM_ERROR("failed to init the connector: %d\n", ret);
        free(connector);
        bdev->connector = NULL;
        return ret;
    }
    connector->status = connector_status_connected;
    connector->state  = malloc(sizeof(*connector->state));

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
        DRM_ERROR("failed to attach the encoder: %d\n", ret);
        return ret;
    }

    /* Add the display modes */
    ret = bochs_connector_get_modes(connector);
    if (ret) {
        DRM_ERROR("failed to add modes: %d\n", ret);
        return ret;
    }

    ret = drm_vblank_init(dev, 1);
    if (ret) {
        DRM_ERROR("failed to init vblank: %d\n", ret);
        return ret;
    }
    drm_connector_register(connector);

    /* The initial modeset runs before drm_dev_register() publishes the node. */
    ret = bochs_kms_initial_modeset(bdev);
    if (ret) return ret;

    return 0;
}

/* Detach the console flush hook, the cursor and the live scanout pointer. */
void bochs_kms_fini(struct bochs_device *bdev)
{
    if (!bdev) return;

    /* Detach the console flush hook before the scanout framebuffer is freed. */
    bochs_console_detach(bdev);

    /* Restore the pointer before the GEM object the core owns goes away. */
    bochs_cursor_cleanup(bdev);
    bdev->current_fb = NULL;
}

#endif
