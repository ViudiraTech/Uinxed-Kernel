/*
 *
 *      simpledrm.c
 *      Simple software-framebuffer DRM driver (Limine GOP)
 *
 *      2026/8/18 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <drivers/gpu/drm/drm.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_print.h>
#include <drivers/gpu/fbdev/video.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

/* Private per-device state: the bootloader framebuffer plus the KMS objects. */
typedef struct simpledrm_device {
        struct drm_device      *drm;
        struct drm_crtc        *crtc;
        struct drm_plane       *primary;
        struct drm_encoder     *encoder;
        struct drm_connector   *connector;
        void                   *screen; /* GOP framebuffer address   */
        uint32_t                width;
        uint32_t                height;
        uint32_t                screen_pitch; /* GOP pitch, in bytes       */
        struct drm_framebuffer *current_fb;
} simpledrm_device_t;

/* A GOP framebuffer is always present and connected. */
static enum drm_connector_status simpledrm_connector_detect(struct drm_connector *connector, bool force)
{
    (void)connector;
    (void)force;
    return connector_status_connected;
}

/* Publish the single native mode of the physical framebuffer. */
static int simpledrm_connector_get_modes(struct drm_connector *connector)
{
    simpledrm_device_t      *sdev = (simpledrm_device_t *)connector->dev->dev_private;
    struct drm_display_mode *mode;

    mode = drm_mode_create(connector->dev);
    if (!mode) return -ENOMEM;

    (void)snprintf(mode->name, DRM_DISPLAY_MODE_LEN - 1, "%ux%u", sdev->width, sdev->height);
    mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
    mode->hdisplay                       = sdev->width;
    mode->hsync_start                    = sdev->width + 80;
    mode->hsync_end                      = sdev->width + 160;
    mode->htotal                         = sdev->width + 320;
    mode->vdisplay                       = sdev->height;
    mode->vsync_start                    = sdev->height + 3;
    mode->vsync_end                      = sdev->height + 6;
    mode->vtotal                         = sdev->height + 32;
    mode->vrefresh                       = 60;
    mode->clock                          = (int)((uint64_t)mode->htotal * (uint64_t)mode->vtotal * (uint64_t)mode->vrefresh / 1000ULL);
    mode->flags                          = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type                           = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    mode->status                         = MODE_OK;

    drm_mode_probed_add(connector, mode);
    return 1;
}

/* The physical display accepts any framebuffer up to its native size. */
static int simpledrm_connector_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode)
{
    simpledrm_device_t *sdev = (simpledrm_device_t *)connector->dev->dev_private;

    (void)connector;
    if (mode->hdisplay <= 0 || mode->vdisplay <= 0 || mode->hdisplay > (int)sdev->width || mode->vdisplay > (int)sdev->height) return MODE_BAD;
    return MODE_OK;
}

/* The encoder imposes no extra atomic constraints. */
static void simpledrm_encoder_atomic_check(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state, struct drm_connector_state *conn_state)
{
    (void)encoder;
    (void)crtc_state;
    (void)conn_state;
}

/* Copy one framebuffer rectangle into the physical GOP framebuffer. */
static int simpledrm_blit_rect(simpledrm_device_t *sdev, struct drm_framebuffer *fb, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    const uint8_t *src;
    uint8_t       *dst;
    uint32_t       fb_pitch;
    uint64_t       src_end;
    size_t         row_bytes;
    uint32_t       y;

    if (!sdev || !fb || !fb->obj[0] || !fb->obj[0]->backing || !sdev->screen) return -EINVAL;

    /* The GOP framebuffer is 32-bit; reject any other pixel layout. */
    if (fb->format != DRM_FORMAT_XRGB8888 && fb->format != DRM_FORMAT_ARGB8888) {
        DRM_WARN("Scanout: unsupported framebuffer format 0x%x, skipping scanout.\n", (unsigned int)fb->format);
        return -EINVAL;
    }

    if (x2 > fb->width || y2 > fb->height || x1 > x2 || y1 > y2) return -EINVAL;
    if (x2 > sdev->width) x2 = sdev->width;
    if (y2 > sdev->height) y2 = sdev->height;
    if (x1 >= x2 || y1 >= y2) return 0;

    fb_pitch = fb->pitches[0];
    if (fb_pitch < fb->width * sizeof(uint32_t)) return -EINVAL;

    row_bytes = (size_t)(x2 - x1) * sizeof(uint32_t);
    src_end   = (uint64_t)fb->offsets[0] + (uint64_t)(y2 - 1) * fb_pitch + (uint64_t)x1 * sizeof(uint32_t) + row_bytes;
    if (src_end > fb->obj[0]->size) return -EINVAL;

    src = (const uint8_t *)fb->obj[0]->backing + fb->offsets[0];
    dst = (uint8_t *)sdev->screen;

    for (y = y1; y < y2; y++) memcpy(dst + (size_t)y * sdev->screen_pitch + (size_t)x1 * sizeof(uint32_t), src + (size_t)y * fb_pitch + (size_t)x1 * sizeof(uint32_t), row_bytes);
    return 0;
}

/* Copy a committed framebuffer into the physical GOP framebuffer. */
static int simpledrm_scanout_fb(simpledrm_device_t *sdev, struct drm_framebuffer *fb)
{
    uint32_t copy_w, copy_h;
    int      ret;

    if (!sdev) return -EINVAL;
    if (!fb) {
        sdev->current_fb = NULL;
        return 0;
    }

    copy_w = fb->width < sdev->width ? fb->width : sdev->width;
    copy_h = fb->height < sdev->height ? fb->height : sdev->height;
    ret    = simpledrm_blit_rect(sdev, fb, 0, 0, copy_w, copy_h);
    if (ret) return ret;

    sdev->current_fb = fb;
    return 0;
}

/* Flush userspace damage from the current scanout buffer to the GOP. */
static int simpledrm_dirty_fb(struct drm_framebuffer *fb, struct drm_file *file_priv, unsigned int flags, unsigned int color, struct drm_clip_rect *clips, unsigned int num_clips)
{
    simpledrm_device_t *sdev;
    unsigned int        first, step;
    int                 ret;

    (void)file_priv;
    (void)color;

    if (!fb || !fb->obj[0] || !fb->obj[0]->dev) return -EINVAL;
    sdev = (simpledrm_device_t *)fb->obj[0]->dev->dev_private;
    if (!sdev) return -EINVAL;

    /* A later page flip uploads off-screen buffers in full. */
    if (sdev->current_fb != fb) return 0;
    if (!num_clips) return simpledrm_blit_rect(sdev, fb, 0, 0, fb->width, fb->height);

    first = (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) ? 1U : 0U;
    step  = (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) ? 2U : 1U;

    for (unsigned int i = first; i < num_clips; i += step) {
        ret = simpledrm_blit_rect(sdev, fb, clips[i].x1, clips[i].y1, clips[i].x2, clips[i].y2);
        if (ret) return ret;
    }
    return 0;
}

static const struct drm_framebuffer_funcs simpledrm_fb_funcs = {
    .dirty = simpledrm_dirty_fb,
};

/* On a modeset, push the new framebuffer to the physical display. */
static void simpledrm_crtc_mode_set(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    simpledrm_device_t *sdev = (simpledrm_device_t *)crtc->dev->dev_private;
    (void)simpledrm_scanout_fb(sdev, fb);
}

/* Page flip helper for the legacy ioctl and atomic fb-only commits. */
static int simpledrm_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb, struct drm_pending_vblank_event *event, uint32_t flags)
{
    simpledrm_device_t *sdev = (simpledrm_device_t *)crtc->dev->dev_private;

    (void)event;
    (void)flags;

    int ret = simpledrm_scanout_fb(sdev, fb);
    if (ret) return ret;

    /* The legacy page-flip ioctl expects the driver to commit the plane. */
    if (crtc->primary && crtc->primary->state) {
        crtc->primary->state->fb = fb;
        crtc->primary->fb_id     = fb ? fb->base.id : 0;
    }
    return 0;
}

/* Enable the CRTC output and deliver any pending vblank event. */
static void simpledrm_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    simpledrm_device_t *sdev  = (simpledrm_device_t *)crtc->dev->dev_private;
    struct drm_plane   *plane = crtc->primary;

    (void)old_state;

    /*
     * On the initial commit the atomic core also ran mode_set, which already
     * scanned this fb out; skip the redundant full-frame memcpy.
     */
    if (plane && plane->state && plane->state->fb && sdev->current_fb != plane->state->fb) (void)simpledrm_scanout_fb(sdev, plane->state->fb);
    if (crtc->state && crtc->state->event) {
        drm_crtc_send_vblank_event(crtc, crtc->state->event);
        crtc->state->event = NULL;
    }
}

/* Disable the CRTC scanout. */
static void simpledrm_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
    simpledrm_device_t *sdev = (simpledrm_device_t *)crtc->dev->dev_private;

    (void)old_state;
    sdev->current_fb = NULL;
}

/* Plane formats */

static const uint32_t simpledrm_formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
};

/* Import a dma-buf that this device exported earlier. */
static struct drm_gem_object *simpledrm_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    (void)dev;
    if (!obj) return NULL;
    drm_gem_object_get(obj);
    return obj;
}

/* Open callback: nothing to initialize per client yet. */
static int simpledrm_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

/* Post-close callback: no per-client state to release. */
static void simpledrm_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

/* Lastclose callback: scanout persists until release. */
static void simpledrm_lastclose(struct drm_device *dev)
{
    (void)dev;
}

/* Tear down the device and release all driver resources. */
static void simpledrm_release(struct drm_device *dev)
{
    simpledrm_device_t *sdev = (simpledrm_device_t *)dev->dev_private;

    /* drm_dev_put() already removed the device from the core list. */
    if (sdev) {
        /*
         * drm_mode_config_cleanup() first: it unlinks every KMS object from
         * the mode_config lists and frees the crtc/plane/connector states
         * plus their internal members.  Only the driver-allocated outer
         * structs are left for us to release here.
         */
        drm_vblank_cleanup(dev);
        drm_mode_config_cleanup(dev);
        if (sdev->connector) free(sdev->connector);
        if (sdev->encoder) free(sdev->encoder);
        if (sdev->crtc) free(sdev->crtc);
        if (sdev->primary) free(sdev->primary);
        free(sdev);
        dev->dev_private = NULL;
    }
}

/* DRM driver descriptor */

static struct drm_driver simpledrm_drm_driver = {
    .name            = "simpledrm",
    .desc            = "DRM driver for simple-framebuffer platform devices",
    .date            = "20200625",
    .major           = 1,
    .minor           = 0,
    .patchlevel      = 0,
    .driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER | DRIVER_SYNCHRONOUS_FLIP,

    .open      = simpledrm_open,
    .postclose = simpledrm_postclose,
    .lastclose = simpledrm_lastclose,
    .release   = simpledrm_release,

    .gem_prime_import = simpledrm_gem_prime_import,
    .fb_funcs         = &simpledrm_fb_funcs,

    .dumb_create     = drm_gem_dumb_create,
    .dumb_map_offset = drm_gem_dumb_map_offset,
    .dumb_destroy    = drm_gem_dumb_destroy,
};

/* Set up the software-only KMS pipeline: plane, CRTC, encoder, connector. */
static int simpledrm_kms_setup(simpledrm_device_t *sdev)
{
    struct drm_device    *dev = sdev->drm;
    struct drm_crtc      *crtc;
    struct drm_plane     *primary;
    struct drm_encoder   *encoder;
    struct drm_connector *connector;
    int                   ret;

    /* Primary plane */

    primary = malloc(sizeof(*primary));
    if (!primary) {
        DRM_ERROR("out of memory allocating primary plane.\n");
        return -ENOMEM;
    }
    memset(primary, 0, sizeof(*primary));
    sdev->primary = primary;

    ret = drm_plane_init(dev, primary, 1, NULL, simpledrm_formats, sizeof(simpledrm_formats) / sizeof(simpledrm_formats[0]), NULL, DRM_PLANE_TYPE_PRIMARY, "simpledrm-primary");
    if (ret) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        free(primary);
        sdev->primary = NULL;
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
    sdev->crtc = crtc;

    {
        static const struct drm_crtc_helper_funcs crtc_helpers = {
            .mode_set       = simpledrm_crtc_mode_set,
            .page_flip      = simpledrm_crtc_page_flip,
            .atomic_enable  = simpledrm_crtc_atomic_enable,
            .atomic_disable = simpledrm_crtc_atomic_disable,
        };

        ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL, (void *)&crtc_helpers, "simpledrm-crtc-0");
        if (ret) {
            DRM_ERROR("Failed to init CRTC: %d\n", ret);
            free(crtc);
            sdev->crtc = NULL;
            return ret;
        }
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
    sdev->encoder = encoder;

    {
        static const struct drm_encoder_helper_funcs enc_helpers = {
            .atomic_mode_set = simpledrm_encoder_atomic_check,
        };

        ret = drm_encoder_init(dev, encoder, (void *)&enc_helpers, DRM_MODE_ENCODER_NONE, "simpledrm-encoder-0");
        if (ret) {
            DRM_ERROR("Failed to init encoder: %d\n", ret);
            free(encoder);
            sdev->encoder = NULL;
            return ret;
        }
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
    sdev->connector = connector;

    {
        static const struct drm_connector_helper_funcs conn_helpers = {
            .detect     = simpledrm_connector_detect,
            .get_modes  = simpledrm_connector_get_modes,
            .mode_valid = simpledrm_connector_mode_valid,
        };

        ret = drm_connector_init(dev, connector, (void *)&conn_helpers, DRM_MODE_CONNECTOR_Unknown);
        if (ret) {
            DRM_ERROR("Failed to init connector: %d\n", ret);
            free(connector);
            sdev->connector = NULL;
            return ret;
        }
    }
    connector->status = connector_status_connected;
    /* display_info mm stays 0 (Linux simpledrm only fills it from DT size) */

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

    /* Add the native display mode */
    ret = simpledrm_connector_get_modes(connector);
    if (ret <= 0) {
        DRM_ERROR("Failed to add modes: %d\n", ret);
        return ret;
    }

    ret = drm_vblank_init(dev, 1);
    if (ret) {
        DRM_ERROR("Failed to init vblank: %d\n", ret);
        return ret;
    }

    drm_connector_register(connector);

    /* Linux simpledrm pins mode_config bounds to the native framebuffer size */
    dev->mode_config.min_width  = sdev->width;
    dev->mode_config.max_width  = sdev->width;
    dev->mode_config.min_height = sdev->height;
    dev->mode_config.max_height = sdev->height;

    DRM_INFO("simpledrm KMS pipeline: plane-%u + crtc-%u + encoder-%u + connector-%u, mode %ux%u, %u pixel format(s)\n", primary->base.id, crtc->base.id, encoder->base.id, connector->base.id,
             sdev->width, sdev->height, (unsigned int)(sizeof(simpledrm_formats) / sizeof(simpledrm_formats[0])));

    return 0;
}

/* Probe / module init */

/*
 * Probe the bootloader GOP framebuffer and attach a software-scanout DRM
 * device.  This is a framebuffer fallback: the GPU driver bus probes it
 * only when no real GPU driver attached, so it never steals the console.
 */
int simpledrm_probe(void)
{
#if CONFIG_SIMPLEDRM
    struct limine_framebuffer *framebuffer;
    simpledrm_device_t        *sdev;
    int                        ret;

    framebuffer = get_framebuffer();
    if (!framebuffer || !framebuffer->address) {
        plogk("simpledrm: no boot framebuffer, probe skipped.\n");
        return -ENODEV;
    }
    if (framebuffer->bpp != 32) {
        plogk("simpledrm: boot framebuffer is %u bpp (only 32bpp XRGB8888 is supported), probe skipped.\n", (unsigned)framebuffer->bpp);
        return -ENODEV;
    }

    sdev = malloc(sizeof(*sdev));
    if (!sdev) {
        plogk("simpledrm: out of memory allocating device state.\n");
        return -ENOMEM;
    }
    memset(sdev, 0, sizeof(*sdev));

    sdev->screen       = framebuffer->address;
    sdev->width        = (uint32_t)framebuffer->width;
    sdev->height       = (uint32_t)framebuffer->height;
    sdev->screen_pitch = (uint32_t)framebuffer->pitch;

    if (!sdev->width || !sdev->height || !sdev->screen_pitch || sdev->screen_pitch < sdev->width * sizeof(uint32_t)) {
        plogk("simpledrm: invalid framebuffer geometry (%ux%u, pitch=%u)\n", sdev->width, sdev->height, sdev->screen_pitch);
        free(sdev);
        return -ENODEV;
    }

    plogk("simpledrm: %ux%u 32bpp boot framebuffer @ 0x%llx, pitch %u\n", sdev->width, sdev->height, (unsigned long long)sdev->screen, sdev->screen_pitch);

    sdev->drm = drm_dev_alloc(&simpledrm_drm_driver);
    if (!sdev->drm) {
        DRM_ERROR("failed to allocate DRM device.\n");
        free(sdev);
        return -ENOMEM;
    }

    sdev->drm->dev_private = sdev;

    /*
     * Build the KMS pipeline first, then publish the device (Linux order):
     * a registered /dev/dri device must already expose its modes and be
     * fully usable.  drm_dev_register()'s "Initialized ... KMS range" log
     * therefore prints the real bounds.
     */
    ret = simpledrm_kms_setup(sdev);
    if (ret) {
        DRM_ERROR("KMS setup failed: %d\n", ret);
        /*
         * Drop the device: .release() tears down whatever KMS objects were
         * built so far and frees sdev.
         */
        drm_dev_unregister(sdev->drm);
        return ret;
    }

    /*
     * drm_dev_register() publishes the /dev/dri nodes, sysfs and the core
     * device list.
     */
    ret = drm_dev_register(sdev->drm, 0);
    if (ret) {
        DRM_ERROR("failed to register DRM device: %d\n", ret);
        drm_dev_unregister(sdev->drm);
        return ret;
    }

    return 0;
#else
    return -ENODEV;
#endif
}
