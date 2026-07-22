/*
 *
 *      drm_init.c
 *      DRM subsystem initialization entry point
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Creates a singleton DRM device, registers it, and exposes
 *  /dev/dri/card0 via devtmpfs. Designed to be called once from
 *  kernel_entry() after VFS/devtmpfs are available.
 *
 */

#include <alloc.h>
#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_init.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>
#include <errno.h>
#include <printk.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vfs.h>

extern int drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs);
extern struct drm_display_mode *drm_mode_create(struct drm_device *dev);
extern void drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode);

/* ------------------------------------------------------------------ */
/* Singleton device                                                    */
/* ------------------------------------------------------------------ */

static struct drm_device *drm_singleton;

struct drm_device *drm_get_singleton(void)
{
    return drm_singleton;
}

/* ------------------------------------------------------------------ */
/* Dummy driver for the built-in DRM node                              */
/* ------------------------------------------------------------------ */

static int drm_dummy_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

static void drm_dummy_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

static void drm_dummy_lastclose(struct drm_device *dev)
{
    (void)dev;
}

static void drm_dummy_gem_free_object(struct drm_gem_object *obj)
{
    if (obj) {
        free(obj->backing);
        obj->backing = NULL;
        free(obj->dma_buf);
        obj->dma_buf = NULL;
    }
}

static struct drm_gem_object *drm_dummy_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    /* For the dummy driver, we can only import buffers that were
     * exported by ourselves. The dma_buf pointer is actually a
     * drm_gem_object pointer. */
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    (void)dev;
    if (!obj) {
        return NULL;
    }
    drm_gem_object_get(obj);
    return obj;
}

static const struct drm_ioctl_desc drm_dummy_ioctls[] = {
    { DRM_IOCTL_VERSION,                  drm_version,            0 },
    { DRM_IOCTL_GET_MAGIC,                drm_getmagic,           DRM_AUTH },
    { DRM_IOCTL_SET_VERSION,              drm_setversion,         DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_SET_MASTER,               drm_setmaster,          DRM_MASTER },
    { DRM_IOCTL_DROP_MASTER,              drm_dropmaster,         DRM_MASTER },
    { DRM_IOCTL_AUTH_MAGIC,               drm_authmagic,          DRM_AUTH },
    { DRM_IOCTL_GEM_CLOSE,                drm_gem_close_ioctl,    DRM_AUTH },
    { DRM_IOCTL_GEM_FLINK,                drm_gem_flink_ioctl,    DRM_AUTH },
    { DRM_IOCTL_GEM_OPEN,                 drm_gem_open_ioctl,     DRM_AUTH },
    { DRM_IOCTL_GET_CAP,                  drm_get_cap,            0 },
    { DRM_IOCTL_SET_CLIENT_CAP,           drm_set_client_cap,     0 },
    { DRM_IOCTL_WAIT_VBLANK,              drm_wait_vblank_ioctl,  0 },
    { DRM_IOCTL_MODE_GETRESOURCES,        drm_mode_getresources,  DRM_AUTH },
    { DRM_IOCTL_MODE_GETCRTC,             drm_mode_getcrtc,       DRM_AUTH },
    { DRM_IOCTL_MODE_SETCRTC,             drm_mode_setcrtc,       DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_CURSOR,              drm_mode_cursor_ioctl,  DRM_AUTH },
    { DRM_IOCTL_MODE_GETENCODER,          drm_mode_getencoder,    DRM_AUTH },
    { DRM_IOCTL_MODE_GETCONNECTOR,        drm_mode_getconnector,  DRM_AUTH },
    { DRM_IOCTL_MODE_GETPROPERTY,         drm_mode_getproperty_ioctl, DRM_AUTH },
    { DRM_IOCTL_MODE_GETFB,               drm_mode_getfb,         DRM_AUTH },
    { DRM_IOCTL_MODE_ADDFB,               drm_mode_addfb,         DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_RMFB,                drm_mode_rmfb,          DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_PAGE_FLIP,           drm_mode_page_flip_ioctl, DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_DIRTYFB,             drm_mode_dirtyfb,       DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_GETPLANERESOURCES,   drm_mode_getplane_res,  DRM_AUTH },
    { DRM_IOCTL_MODE_GETPLANE,            drm_mode_getplane,      DRM_AUTH },
    { DRM_IOCTL_MODE_SETPLANE,            drm_mode_setplane,      DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_ADDFB2,              drm_mode_addfb2,        DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_OBJ_GETPROPERTIES,   drm_mode_obj_getproperties_ioctl, DRM_AUTH },
    { DRM_IOCTL_MODE_OBJ_SETPROPERTY,     drm_mode_obj_setproperty_ioctl,   DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_CURSOR2,             drm_mode_cursor2_ioctl, DRM_AUTH },
    { DRM_IOCTL_MODE_ATOMIC,              drm_mode_atomic_ioctl,  DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_MODE_GETFB2,              drm_mode_getfb2_ioctl,  DRM_AUTH },
};

static struct drm_driver drm_dummy_driver = {
    .name          = "drm",
    .desc          = "Uinxed DRM",
    .date          = "20260722",
    .major         = 1,
    .minor         = 0,
    .patchlevel    = 0,
    .driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_PRIME | DRIVER_RENDER,
    .open          = drm_dummy_open,
    .postclose     = drm_dummy_postclose,
    .lastclose     = drm_dummy_lastclose,
    .gem_free_object = drm_dummy_gem_free_object,
    .gem_prime_import = drm_dummy_gem_prime_import,
    .dumb_create   = drm_gem_dumb_create,
    .dumb_map_offset = drm_gem_dumb_map_offset,
    .dumb_destroy  = drm_gem_dumb_destroy,
    .ioctls        = drm_dummy_ioctls,
    .num_ioctls    = sizeof(drm_dummy_ioctls) / sizeof(drm_dummy_ioctls[0]),
};

/* ------------------------------------------------------------------ */
/* KMS pipeline setup for the dummy driver                              */
/* ------------------------------------------------------------------ */

static struct drm_crtc     pipeline_crtc;
static struct drm_plane    pipeline_primary_plane;
static struct drm_encoder  pipeline_encoder;
static struct drm_connector pipeline_connector;

/* ------------------------------------------------------------------ */
/* Configurable mode table — data-driven, not hardcoded in logic       */
/* ------------------------------------------------------------------ */

struct dummy_mode_cfg {
    const char *name;
    int         clock;
    int         hdisplay;
    int         hsync_start;
    int         hsync_end;
    int         htotal;
    int         vdisplay;
    int         vsync_start;
    int         vsync_end;
    int         vtotal;
    int         vrefresh;
    unsigned    flags;
    unsigned    type;
};

static const struct dummy_mode_cfg dummy_modes[] = {
    {
        .name        = "1920x1080",
        .clock       = 148500,
        .hdisplay    = 1920, .hsync_start = 2008, .hsync_end = 2052, .htotal = 2200,
        .vdisplay    = 1080, .vsync_start = 1084, .vsync_end = 1089, .vtotal = 1125,
        .vrefresh    = 60,
        .flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
        .type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER,
    },
    {
        .name        = "1280x720",
        .clock       = 74250,
        .hdisplay    = 1280, .hsync_start = 1390, .hsync_end = 1430, .htotal = 1650,
        .vdisplay    = 720,  .vsync_start = 725,  .vsync_end = 730,  .vtotal = 750,
        .vrefresh    = 60,
        .flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
        .type        = DRM_MODE_TYPE_DRIVER,
    },
};

static int drm_dummy_kms_add_modes(struct drm_device *dev, struct drm_connector *connector)
{
    unsigned int i;

    (void)dev;

    for (i = 0; i < sizeof(dummy_modes) / sizeof(dummy_modes[0]); i++) {
        const struct dummy_mode_cfg *cfg = &dummy_modes[i];
        struct drm_display_mode *mode;

        mode = drm_mode_create(dev);
        if (!mode) {
            return -ENOMEM;
        }

        strncpy(mode->name, cfg->name, DRM_DISPLAY_MODE_LEN - 1);
        mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
        mode->clock       = cfg->clock;
        mode->hdisplay    = cfg->hdisplay;
        mode->hsync_start = cfg->hsync_start;
        mode->hsync_end   = cfg->hsync_end;
        mode->htotal      = cfg->htotal;
        mode->vdisplay    = cfg->vdisplay;
        mode->vsync_start = cfg->vsync_start;
        mode->vsync_end   = cfg->vsync_end;
        mode->vtotal      = cfg->vtotal;
        mode->vrefresh    = cfg->vrefresh;
        mode->flags       = cfg->flags;
        mode->type        = cfg->type;
        mode->status      = MODE_OK;

        drm_mode_probed_add(connector, mode);
    }

    return 0;
}

static int drm_dummy_kms_setup(struct drm_device *dev)
{
    static const uint32_t primary_formats[] = {
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ARGB8888,
        DRM_FORMAT_RGB888,
        DRM_FORMAT_RGB565,
    };
    int ret;

    memset(&pipeline_crtc, 0, sizeof(pipeline_crtc));
    memset(&pipeline_primary_plane, 0, sizeof(pipeline_primary_plane));
    memset(&pipeline_encoder, 0, sizeof(pipeline_encoder));
    memset(&pipeline_connector, 0, sizeof(pipeline_connector));

    /* Create primary plane (can only be driven by CRTC 0) */
    ret = drm_plane_init(dev, &pipeline_primary_plane,
                         1, /* possible_crtcs = bit 0 */
                         NULL,
                         primary_formats,
                         sizeof(primary_formats) / sizeof(primary_formats[0]),
                         NULL,
                         DRM_PLANE_TYPE_PRIMARY,
                         "primary");
    if (ret) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        return ret;
    }

    /* Allocate and initialise the primary plane state */
    pipeline_primary_plane.state = malloc(sizeof(*pipeline_primary_plane.state));
    if (!pipeline_primary_plane.state) {
        DRM_ERROR("Failed to alloc primary plane state\n");
        return -ENOMEM;
    }
    memset(pipeline_primary_plane.state, 0, sizeof(*pipeline_primary_plane.state));
    pipeline_primary_plane.state->plane = &pipeline_primary_plane;
    pipeline_primary_plane.state->crtc = &pipeline_crtc;
    pipeline_primary_plane.state->rotation = 0;
    pipeline_primary_plane.state->alpha = 0xFFFF;
    pipeline_primary_plane.state->pixel_blend_mode = 0;
    pipeline_primary_plane.state->visible = true;

    /* Create CRTC with the primary plane */
    ret = drm_crtc_init_with_planes(dev, &pipeline_crtc,
                                    &pipeline_primary_plane,
                                    NULL, NULL, "CRTC-0");
    if (ret) {
        DRM_ERROR("Failed to init CRTC: %d\n", ret);
        return ret;
    }

    /* Allocate and initialise the CRTC state */
    pipeline_crtc.state = malloc(sizeof(*pipeline_crtc.state));
    if (!pipeline_crtc.state) {
        DRM_ERROR("Failed to alloc CRTC state\n");
        return -ENOMEM;
    }
    memset(pipeline_crtc.state, 0, sizeof(*pipeline_crtc.state));
    pipeline_crtc.state->crtc = &pipeline_crtc;
    pipeline_crtc.state->active = false;
    pipeline_crtc.state->enable = false;

    /* Create encoder (VIRTUAL type for software-only output) */
    ret = drm_encoder_init(dev, &pipeline_encoder,
                           NULL, DRM_MODE_ENCODER_VIRTUAL, "encoder-0");
    if (ret) {
        DRM_ERROR("Failed to init encoder: %d\n", ret);
        return ret;
    }
    pipeline_encoder.possible_crtcs = 1;
    pipeline_encoder.crtc = &pipeline_crtc;

    /* Create connector (VIRTUAL, initially connected) */
    ret = drm_connector_init(dev, &pipeline_connector,
                             NULL, DRM_MODE_CONNECTOR_VIRTUAL);
    if (ret) {
        DRM_ERROR("Failed to init connector: %d\n", ret);
        return ret;
    }
    pipeline_connector.status = connector_status_connected;
    pipeline_connector.display_info_width_mm = 500;
    pipeline_connector.display_info_height_mm = 280;

    /* Allocate and initialise the connector state */
    pipeline_connector.state = malloc(sizeof(*pipeline_connector.state));
    if (!pipeline_connector.state) {
        DRM_ERROR("Failed to alloc connector state\n");
        return -ENOMEM;
    }
    memset(pipeline_connector.state, 0, sizeof(*pipeline_connector.state));
    pipeline_connector.state->connector = &pipeline_connector;
    pipeline_connector.state->crtc = &pipeline_crtc;
    pipeline_connector.state->best_encoder = &pipeline_encoder;

    /* Attach encoder to connector */
    ret = drm_connector_attach_encoder(&pipeline_connector, &pipeline_encoder);
    if (ret) {
        DRM_ERROR("Failed to attach encoder: %d\n", ret);
        return ret;
    }

    /* Add display modes from the configurable table */
    ret = drm_dummy_kms_add_modes(dev, &pipeline_connector);
    if (ret) {
        DRM_ERROR("Failed to add modes: %d\n", ret);
        return ret;
    }

    /* Initialise vblank for this CRTC */
    ret = drm_vblank_init(dev, 1);
    if (ret) {
        DRM_ERROR("Failed to init vblank: %d\n", ret);
        return ret;
    }

    drm_connector_register(&pipeline_connector);

    DRM_INFO("KMS pipeline: CRTC-%u + primary plane-%u + encoder-%u + connector-%u (%u modes)\n",
             pipeline_crtc.base.id, pipeline_primary_plane.base.id,
             pipeline_encoder.base.id, pipeline_connector.base.id,
             sizeof(dummy_modes) / sizeof(dummy_modes[0]));

    return 0;
}

/* ------------------------------------------------------------------ */
/* DRM VFS ioctl wrapper                                               */
/* ------------------------------------------------------------------ */

size_t drm_dev_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

int drm_dev_ioctl(void *file, size_t req, void *arg)
{
    /* file is the drm_file pointer stored as ctx */
    struct drm_file *file_priv = (struct drm_file *)file;

    if (!file_priv || !drm_singleton) {
        return -ENODEV;
    }

    return drm_ioctl(drm_singleton, (unsigned int)req, arg, file_priv);
}

int drm_dev_poll(void *file, size_t events)
{
    (void)file;
    (void)events;
    return 0;
}

void *drm_dev_mmap(void *file, size_t offset, size_t size, int flags)
{
    struct drm_file *file_priv = (struct drm_file *)file;
    struct drm_gem_object *obj;
    void *result;

    (void)flags;
    (void)size;

    if (!file_priv || !drm_singleton) {
        return NULL;
    }

    /* Look up the GEM object by the mmap offset that was returned
     * from MAP_DUMB. Its backing memory is identity-mapped (physical
     * == virtual) so we can return the pointer directly. */
    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);
    if (!obj) {
        return NULL;
    }

    result = obj->backing;
    drm_gem_object_put(obj);
    return result;
}



/* ------------------------------------------------------------------ */
/* DRM open / release callbacks for devtmpfs                           */
/* ------------------------------------------------------------------ */

/*
 * When userspace opens /dev/dri/card0, tmpfs calls this open callback.
 * We allocate a drm_file and bind it to the VFS node's handle.
 */
void drm_vfs_open_cb(void *parent, const char *name, void *node_ptr)
{
    (void)parent;
    (void)name;

    if (!drm_singleton) {
        return;
    }

    vfs_node_t node = (vfs_node_t)node_ptr;
    if (!node) {
        return;
    }

    struct drm_file *file = malloc(sizeof(*file));
    if (!file) {
        return;
    }

    memset(file, 0, sizeof(*file));
    int ret = drm_open(drm_singleton, file);
    if (ret != 0) {
        free(file);
        return;
    }

    /* Store the drm_file as the node's file-private handle */
    node->handle = file;
}

void drm_vfs_close_cb(void *current)
{
    vfs_node_t node = (vfs_node_t)current;

    if (!node || !node->handle) {
        return;
    }

    struct drm_file *file = (struct drm_file *)node->handle;

    drm_release(file);
    node->handle = NULL;
}

/* ------------------------------------------------------------------ */
/* Public init                                                         */
/* ------------------------------------------------------------------ */

int drm_init(void)
{
    struct drm_device *dev;

    dev = drm_dev_alloc(&drm_dummy_driver);
    if (!dev) {
        DRM_ERROR("Failed to allocate DRM device\n");
        return -ENOMEM;
    }

    int ret = drm_dev_register(dev, 0);
    if (ret != 0) {
        DRM_ERROR("Failed to register DRM device: %d\n", ret);
        free(dev);
        return ret;
    }

    /* Set up the minimal KMS display pipeline */
    ret = drm_dummy_kms_setup(dev);
    if (ret != 0) {
        DRM_ERROR("Failed to set up KMS pipeline: %d\n", ret);
        free(dev);
        return ret;
    }

    drm_singleton = dev;

    DRM_INFO("DRM subsystem initialized (device: /dev/dri/card0)\n");
    return 0;
}