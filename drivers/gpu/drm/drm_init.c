/*
 *
 *      drm_init.c
 *      DRM subsystem initialization entry point
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/gpu/drm/drm.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_print.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/process.h>
#include <syscall/fcntl.h>

/* Global DRM device list (replaces singleton) */

static struct drm_device *drm_device_list[DRM_MAX_DEVICES];
static spinlock_t         drm_device_list_lock = {.lock = 0, .rflags = 0};

/* Register a device in the global DRM device list. */
void drm_device_list_add(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (!drm_device_list[i]) {
            drm_device_list[i] = dev;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

/* Remove a device from the global DRM device list. */
void drm_device_list_remove(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i] == dev) {
            drm_device_list[i] = NULL;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

/*
 * Look up a registered device by its minor type and index.  The returned
 * device holds a caller reference which must be dropped with drm_dev_put().
 * Taking the reference under the list lock keeps the device alive even if
 * a concurrent final drm_dev_put is tearing it down.
 */
struct drm_device *drm_get_device_by_minor(int type, int index)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];
        if (!dev) continue;
        if (type == DRM_MINOR_PRIMARY && dev->primary && dev->primary->index == index) {
            if (drm_dev_get(dev)) {
                spin_unlock(&drm_device_list_lock);
                return dev;
            }
            break; // Minor indices are unique; a dying device is the only match.
        }
        if (type == DRM_MINOR_RENDER && dev->render && dev->render->index == index) {
            if (drm_dev_get(dev)) {
                spin_unlock(&drm_device_list_lock);
                return dev;
            }
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
    return NULL;
}

/*
 * Iterate the registered device list.  @idx starts at 0.  The returned
 * device holds a caller reference which must be dropped with drm_dev_put().
 */
struct drm_device *drm_device_list_iter(int *idx)
{
    struct drm_device *dev = NULL;
    int                i;

    if (!idx || *idx < 0) return NULL;

    spin_lock(&drm_device_list_lock);
    for (i = *idx; i < DRM_MAX_DEVICES; i++) {
        if (!drm_device_list[i]) continue;
        dev = drm_dev_get(drm_device_list[i]);
        if (!dev) continue;
        *idx = i + 1;
        break;
    }
    spin_unlock(&drm_device_list_lock);
    return dev;
}

/*
 * Collect up to @max registered devices into @out, each holding a caller
 * reference which must be dropped with drm_dev_put().  Returns the number
 * collected.  Takes the device list lock once instead of once per device,
 * which matters for per-tick callers like the vblank emulation timer.
 */
int drm_device_list_collect(struct drm_device **out, int max)
{
    int n = 0;

    if (!out || max <= 0) return 0;

    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES && n < max; i++) {
        struct drm_device *dev = drm_device_list[i];
        if (!dev) continue;
        dev = drm_dev_get(dev);
        if (!dev) continue;
        out[n++] = dev;
    }
    spin_unlock(&drm_device_list_lock);
    return n;
}

/* GPU driver registry: built-in drivers register a probe with the core. */

static struct drm_gpu_driver drm_gpu_drivers[DRM_MAX_GPU_DRIVERS];
static int                   drm_gpu_driver_count;
static spinlock_t            drm_gpu_driver_lock = {.lock = 0, .rflags = 0};

/* Register a built-in GPU driver probe callback. */
int drm_gpu_driver_register(const char *name, int (*probe)(void))
{
    if (!name || !probe) return -EINVAL;

    spin_lock(&drm_gpu_driver_lock);
    if (drm_gpu_driver_count >= DRM_MAX_GPU_DRIVERS) {
        spin_unlock(&drm_gpu_driver_lock);
        plogk("drm: GPU driver registry full, ignoring \"%s\"\n", name);
        return -ENOSPC;
    }
    drm_gpu_drivers[drm_gpu_driver_count].name  = name;
    drm_gpu_drivers[drm_gpu_driver_count].probe = probe;
    drm_gpu_driver_count++;
    spin_unlock(&drm_gpu_driver_lock);
    plogk("drm: Registered GPU driver \"%s\"\n", name);
    return 0;
}

/*
 * Probe every registered GPU driver in registration order and attach any
 * that find hardware, so a machine with several GPUs gets one /dev/dri node
 * per driver rather than a single election.  Returns 0 if at least one GPU
 * attached, otherwise the last probe error (a real failure such as -ENOMEM
 * is thus distinguishable from "no device found"); the boot path is then
 * expected to call drm_init_fallback() for the software framebuffer device.
 *
 * This runs once from the single-threaded boot path after gpu_drivers_init(),
 * so no lock is needed to walk the registry.  A probe callback may block
 * (device setup), which also precludes holding the registry spinlock here.
 */
int drm_gpu_probe_all(void)
{
#if CONFIG_DRM
    int attached   = 0;
    int last_error = -ENODEV;

    for (int i = 0; i < drm_gpu_driver_count; i++) {
        const char *name   = drm_gpu_drivers[i].name;
        int (*probe)(void) = drm_gpu_drivers[i].probe;

        if (!probe) continue;
        int ret = probe();
        if (ret == 0) {
            attached++;
            plogk("drm: GPU driver \"%s\" attached.\n", name ? name : "?");
        } else {
            plogk("drm: GPU driver \"%s\" did not attach (err=%d)\n", name ? name : "?", ret);
            last_error = ret;
        }
    }
    return attached > 0 ? 0 : last_error;
#else
    return -ENODEV;
#endif
}

/* Dummy driver for the built-in DRM node */

static int drm_dummy_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

/* Dummy driver postclose callback. */
static void drm_dummy_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

/* Dummy driver lastclose callback. */
static void drm_dummy_lastclose(struct drm_device *dev)
{
    (void)dev;
}

/* Dummy driver GEM object free callback. */
static void drm_dummy_gem_free_object(struct drm_gem_object *obj)
{
    if (obj) {
        free(obj->backing);
        obj->backing = NULL;
        free(obj->dma_buf);
        obj->dma_buf = NULL;
    }
}

/* Dummy driver PRIME import callback. */
static struct drm_gem_object *drm_dummy_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    /*
     * For the dummy driver, we can only import buffers that were
     * exported by ourselves. The dma_buf pointer is actually a
     * drm_gem_object pointer.
     */
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    (void)dev;
    if (!obj) return NULL;
    drm_gem_object_get(obj);
    return obj;
}

static const struct drm_ioctl_desc drm_dummy_ioctls[] = {
    {DRM_IOCTL_VERSION,                drm_version,                      0                    },
    {DRM_IOCTL_GET_MAGIC,              drm_getmagic,                     DRM_AUTH             },
    {DRM_IOCTL_SET_VERSION,            drm_setversion,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_SET_MASTER,             drm_setmaster,                    0                    },
    {DRM_IOCTL_DROP_MASTER,            drm_dropmaster,                   0                    },
    {DRM_IOCTL_AUTH_MAGIC,             drm_authmagic,                    DRM_AUTH             },
    {DRM_IOCTL_GEM_CLOSE,              drm_gem_close_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_FLINK,              drm_gem_flink_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_OPEN,               drm_gem_open_ioctl,               DRM_AUTH             },
    {DRM_IOCTL_GET_CAP,                drm_get_cap,                      0                    },
    {DRM_IOCTL_SET_CLIENT_CAP,         drm_set_client_cap,               0                    },
    {DRM_IOCTL_WAIT_VBLANK,            drm_wait_vblank_ioctl,            0                    },
    {DRM_IOCTL_MODE_GETRESOURCES,      drm_mode_getresources,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCRTC,           drm_mode_getcrtc,                 DRM_AUTH             },
    {DRM_IOCTL_MODE_SETCRTC,           drm_mode_setcrtc,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR,            drm_mode_cursor_ioctl,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETENCODER,        drm_mode_getencoder,              DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCONNECTOR,      drm_mode_getconnector,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPROPERTY,       drm_mode_getproperty_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_GETFB,             drm_mode_getfb,                   DRM_AUTH             },
    {DRM_IOCTL_MODE_ADDFB,             drm_mode_addfb,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_RMFB,              drm_mode_rmfb,                    DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_PAGE_FLIP,         drm_mode_page_flip_ioctl,         DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_DIRTYFB,           drm_mode_dirtyfb,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPLANE,          drm_mode_getplane,                DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPLANE,          drm_mode_setplane,                DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB2,            drm_mode_addfb2,                  DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_mode_obj_getproperties_ioctl, DRM_AUTH             },
    {DRM_IOCTL_MODE_OBJ_SETPROPERTY,   drm_mode_obj_setproperty_ioctl,   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_SETPROPERTY,       drm_connector_property_set_ioctl, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR2,           drm_mode_cursor2_ioctl,           DRM_AUTH             },
    {DRM_IOCTL_MODE_ATOMIC,            drm_mode_atomic_ioctl,            DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB2,            drm_mode_getfb2_ioctl,            DRM_AUTH             },
};

static struct drm_driver drm_dummy_driver = {
    .name             = "drm",
    .desc             = "Uinxed DRM",
    .date             = "20260722",
    .major            = 1,
    .minor            = 0,
    .patchlevel       = 0,
    .driver_features  = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_PRIME | DRIVER_RENDER,
    .open             = drm_dummy_open,
    .postclose        = drm_dummy_postclose,
    .lastclose        = drm_dummy_lastclose,
    .gem_free_object  = drm_dummy_gem_free_object,
    .gem_prime_import = drm_dummy_gem_prime_import,
    .dumb_create      = drm_gem_dumb_create,
    .dumb_map_offset  = drm_gem_dumb_map_offset,
    .dumb_destroy     = drm_gem_dumb_destroy,
    .ioctls           = drm_dummy_ioctls,
    .num_ioctls       = sizeof(drm_dummy_ioctls) / sizeof(drm_dummy_ioctls[0]),
};

/* KMS pipeline setup for the dummy driver */

static struct drm_crtc      pipeline_crtc;
static struct drm_plane     pipeline_primary_plane;
static struct drm_encoder   pipeline_encoder;
static struct drm_connector pipeline_connector;

/* Configurable mode table - data-driven, not hardcoded in logic */

typedef struct dummy_mode_cfg {
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
} dummy_mode_cfg_t;

static const dummy_mode_cfg_t dummy_modes[] = {
    {
     .name        = "1920x1080",
     .clock       = 148500,
     .hdisplay    = 1920,
     .hsync_start = 2008,
     .hsync_end   = 2052,
     .htotal      = 2200,
     .vdisplay    = 1080,
     .vsync_start = 1084,
     .vsync_end   = 1089,
     .vtotal      = 1125,
     .vrefresh    = 60,
     .flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
     .type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER,
     },
    {
     .name        = "1280x720",
     .clock       = 74250,
     .hdisplay    = 1280,
     .hsync_start = 1390,
     .hsync_end   = 1430,
     .htotal      = 1650,
     .vdisplay    = 720,
     .vsync_start = 725,
     .vsync_end   = 730,
     .vtotal      = 750,
     .vrefresh    = 60,
     .flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
     .type        = DRM_MODE_TYPE_DRIVER,
     },
};

/* Add the configurable mode table to the dummy connector. */
static int drm_dummy_kms_add_modes(struct drm_device *dev, struct drm_connector *connector)
{
    unsigned int i;

    (void)dev;

    for (i = 0; i < sizeof(dummy_modes) / sizeof(dummy_modes[0]); i++) {
        const dummy_mode_cfg_t  *cfg = &dummy_modes[i];
        struct drm_display_mode *mode;

        mode = drm_mode_create(dev);
        if (!mode) return -ENOMEM;

        strncpy(mode->name, cfg->name, DRM_DISPLAY_MODE_LEN - 1);
        mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
        mode->clock                          = cfg->clock;
        mode->hdisplay                       = cfg->hdisplay;
        mode->hsync_start                    = cfg->hsync_start;
        mode->hsync_end                      = cfg->hsync_end;
        mode->htotal                         = cfg->htotal;
        mode->vdisplay                       = cfg->vdisplay;
        mode->vsync_start                    = cfg->vsync_start;
        mode->vsync_end                      = cfg->vsync_end;
        mode->vtotal                         = cfg->vtotal;
        mode->vrefresh                       = cfg->vrefresh;
        mode->flags                          = cfg->flags;
        mode->type                           = cfg->type;
        mode->status                         = MODE_OK;

        drm_mode_probed_add(connector, mode);
    }

    return 0;
}

/* Build the software-only KMS pipeline: plane, CRTC, encoder, connector. */
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
    ret = drm_plane_init(dev, &pipeline_primary_plane, 1, // possible_crtcs = bit 0
                         NULL, primary_formats, sizeof(primary_formats) / sizeof(primary_formats[0]), NULL, DRM_PLANE_TYPE_PRIMARY, "primary");
    if (ret) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        return ret;
    }

    /* Allocate and initialise the primary plane state */
    pipeline_primary_plane.state = malloc(sizeof(*pipeline_primary_plane.state));
    if (!pipeline_primary_plane.state) {
        DRM_ERROR("Failed to alloc primary plane state.\n");
        return -ENOMEM;
    }
    memset(pipeline_primary_plane.state, 0, sizeof(*pipeline_primary_plane.state));
    pipeline_primary_plane.state->plane            = &pipeline_primary_plane;
    pipeline_primary_plane.state->crtc             = &pipeline_crtc;
    pipeline_primary_plane.state->rotation         = 0;
    pipeline_primary_plane.state->alpha            = 0xFFFF;
    pipeline_primary_plane.state->pixel_blend_mode = 0;
    pipeline_primary_plane.state->visible          = true;

    /* Create CRTC with the primary plane */
    ret = drm_crtc_init_with_planes(dev, &pipeline_crtc, &pipeline_primary_plane, NULL, NULL, "CRTC-0");
    if (ret) {
        DRM_ERROR("Failed to init CRTC: %d\n", ret);
        return ret;
    }

    /* Allocate and initialise the CRTC state */
    pipeline_crtc.state = malloc(sizeof(*pipeline_crtc.state));
    if (!pipeline_crtc.state) {
        DRM_ERROR("Failed to alloc CRTC state.\n");
        return -ENOMEM;
    }
    memset(pipeline_crtc.state, 0, sizeof(*pipeline_crtc.state));
    pipeline_crtc.state->crtc   = &pipeline_crtc;
    pipeline_crtc.state->active = false;
    pipeline_crtc.state->enable = false;

    /* Create encoder (VIRTUAL type for software-only output) */
    ret = drm_encoder_init(dev, &pipeline_encoder, NULL, DRM_MODE_ENCODER_VIRTUAL, "encoder-0");
    if (ret) {
        DRM_ERROR("Failed to init encoder: %d\n", ret);
        return ret;
    }
    pipeline_encoder.possible_crtcs = 1;
    pipeline_encoder.crtc           = &pipeline_crtc;

    /* Create connector (VIRTUAL, initially connected) */
    ret = drm_connector_init(dev, &pipeline_connector, NULL, DRM_MODE_CONNECTOR_VIRTUAL);
    if (ret) {
        DRM_ERROR("Failed to init connector: %d\n", ret);
        return ret;
    }
    pipeline_connector.status                 = connector_status_connected;
    pipeline_connector.display_info_width_mm  = 500;
    pipeline_connector.display_info_height_mm = 280;

    /* Allocate and initialise the connector state */
    pipeline_connector.state = malloc(sizeof(*pipeline_connector.state));
    if (!pipeline_connector.state) {
        DRM_ERROR("Failed to alloc connector state.\n");
        return -ENOMEM;
    }
    memset(pipeline_connector.state, 0, sizeof(*pipeline_connector.state));
    pipeline_connector.state->connector    = &pipeline_connector;
    pipeline_connector.state->crtc         = &pipeline_crtc;
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
    DRM_INFO("KMS pipeline: CRTC-%u + primary plane-%u + encoder-%u + connector-%u (%u modes)\n", pipeline_crtc.base.id, pipeline_primary_plane.base.id, pipeline_encoder.base.id,
             pipeline_connector.base.id, sizeof(dummy_modes) / sizeof(dummy_modes[0]));

    return 0;
}

/* DRM VFS ioctl wrapper */

/* VFS read callback: deliver pending DRM events to the caller. */
size_t drm_dev_read(void *file, void *addr, size_t offset, size_t size)
{
    struct drm_file *file_priv = (struct drm_file *)file;
    size_t           position  = offset;

    if (!file_priv) return (size_t)-1;
    int ret = drm_read(file_priv, (char *)addr, size, &position, false);
    return ret < 0 ? (size_t)-1 : (size_t)ret;
}

/* VFS write wrapper for /dev/dri nodes. */
size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

/* VFS ioctl callback: dispatch to the DRM ioctl handler. */
int drm_dev_ioctl(void *file, size_t req, void *arg)
{
    struct drm_device *dev;
    struct drm_file   *file_priv = (struct drm_file *)file;

    if (!file_priv) return -ENODEV;

    /* Route to the device bound to this open file, not a global singleton. */
    dev = (struct drm_device *)file_priv->minor_unused;
    if (!dev) return -ENODEV;

    return drm_ioctl(dev, (unsigned int)req, arg, file_priv);
}

/* Parse a strictly numeric node suffix.  Returns -1 for an empty,
 * non-numeric, or overflowing suffix instead of atoi()'s silent 0. */
static int drm_parse_minor(const char *s)
{
    int minor = 0;

    if (!s || !*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        minor = minor * 10 + (*s - '0');
        if (minor < 0) return -1; /* signed overflow */
    }
    return minor;
}

/*
 * Resolve the DRM device that owns a /dev/dri node from its name.  Nodes
 * are named "cardN" (primary minor N) or "renderD128+N" (render minor N)
 * by drm_dev_register(); no single-device assumption is made here.  The
 * returned device holds a caller reference which must be dropped with
 * drm_dev_put().
 */
static struct drm_device *drm_device_from_node(void *node_ptr)
{
    vfs_node_t node = (vfs_node_t)node_ptr;
    int        minor;

    if (!node || !node->name || !node->name[0]) return NULL;

    if (!strncmp(node->name, "card", 4)) {
        minor = drm_parse_minor(node->name + 4);
        if (minor < 0) return NULL;
        return drm_get_device_by_minor(DRM_MINOR_PRIMARY, minor);
    }
    if (!strncmp(node->name, "renderD", 7)) {
        minor = drm_parse_minor(node->name + 7);
        if (minor < 128) return NULL;
        return drm_get_device_by_minor(DRM_MINOR_RENDER, minor - 128);
    }
    return NULL;
}

/*
 * tmpfs/devtmpfs per-open bridge. A VFS node is shared by all processes, so
 * storing drm_file in node->handle is incorrect: one close could release
 * another client's state.
 */
int drm_dev_open(void *node_ptr, uint64_t flags, void **private_data)
{
    struct drm_device *dev;
    struct drm_file   *file;
    int                ret;

    (void)flags;
    if (!private_data) return -EINVAL;
    *private_data = NULL;

    /* Bind the open to whichever GPU registered this node. */
    dev = drm_device_from_node(node_ptr);
    if (!dev) return -ENODEV;
    file = malloc(sizeof(*file));
    if (!file) {
        drm_dev_put(dev);
        return -ENOMEM;
    }
    memset(file, 0, sizeof(*file));
    ret = drm_open(dev, file);

    /* Drop the lookup reference; the file holds its own via drm_open(). */
    drm_dev_put(dev);
    if (ret) {
        free(file);
        return ret;
    }

    /*
     * A root compositor opening the primary node is already trusted for
     * DRM_AUTH ioctls.  Weston performs GETRESOURCES immediately after the
     * open (before issuing SET_MASTER); leaving this bit clear makes the
     * otherwise valid KMS device look absent to its DRM backend.  Render
     * nodes intentionally keep the normal unauthenticated state.
     */
    vfs_node_t node = (vfs_node_t)node_ptr;
    process_t *proc = process_current();
    if (node && node->name && !strncmp(node->name, "card", 4) && proc && proc->uid == 0) file->authenticated = true;
    /*
     * drm_send_event() uses this stable device node to wake the VFS poll
     * source watched by Weston's epoll loop.  Event readiness itself remains
     * per-open and is checked through drm_poll(file, ...).
     */
    file->filp_unused = node;
    *private_data     = file;
    return 0;
}

/* VFS release callback for /dev/dri nodes. */
void drm_dev_release(void *node_ptr, void *private_data)
{
    (void)node_ptr;
    /* drm_release() tears down and frees the drm_file itself. */
    if (private_data) drm_release((struct drm_file *)private_data);
}

/* VFS ioctl wrapper for /dev/dri files. */
int drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    (void)ctx;
    (void)flags;
    return drm_dev_ioctl(private_data, req, arg);
}

/* VFS read wrapper for /dev/dri files. */
int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    size_t position = offset;
    return drm_read((struct drm_file *)private_data, (char *)addr, size, &position, (flags & O_NONBLOCK) != 0);
}

/* VFS write wrapper for /dev/dri files. */
int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;
    return (int64_t)drm_dev_write(private_data, addr, offset, size);
}

/* VFS poll wrapper for /dev/dri files. */
int drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;
    return drm_dev_poll(private_data, events);
}

/* VFS poll callback for /dev/dri nodes. */
int drm_dev_poll(void *file, size_t events)
{
    return (int)drm_poll((struct drm_file *)file, (unsigned int)events);
}

/* Legacy VFS mmap callback: map the GEM backing memory by offset. */
void *drm_dev_mmap(void *file, size_t offset, size_t size, int flags)
{
    struct drm_device     *dev;
    struct drm_file       *file_priv = (struct drm_file *)file;
    struct drm_gem_object *obj;
    void                  *result;

    (void)flags;
    (void)size;

    if (!file_priv) return NULL;

    /* Route to the device bound to this open file, not a global singleton. */
    dev = (struct drm_device *)file_priv->minor_unused;
    if (!dev) return NULL;

    /*
     * Look up the GEM object by the mmap offset that was returned
     * from MAP_DUMB. Its backing memory is identity-mapped (physical
     * == virtual) so we can return the pointer directly.
     */
    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);
    if (!obj) return NULL;

    result = obj->backing;
    drm_gem_object_put(obj);
    return result;
}

/* DRM per-open mmap callback (VMA-aware GEM mmap) */

void *drm_dev_file_mmap(void *ctx, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    struct drm_device     *dev       = (struct drm_device *)ctx;
    struct drm_file       *file_priv = (struct drm_file *)private_data;
    struct drm_gem_object *obj;

    (void)size;
    (void)flags;

    /* ctx carries the owning device; fall back to the file's bound device. */
    if (!dev) dev = file_priv ? (struct drm_device *)file_priv->minor_unused : NULL;
    if (!dev || !file_priv || !vma) return NULL;

    /* Look up the GEM object by its mmap offset. */
    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);
    if (!obj || !obj->backing) return NULL;

    /*
     * Store GEM object in VMA for lifetime tracking.
     * process_munmap will call drm_gem_object_put when the
     * mapping is torn down.
     */
    vma->vm_private_data = obj;

    /*
     * Identity-mapped physical memory: return the backing pointer.
     * The syscall mmap layer handles PTE creation using this pointer.
     */
    return obj->backing;
}

/* Public init */

/* Allocate and register the software fallback DRM device. */
int drm_init_fallback(void)
{
#if CONFIG_DRM
    struct drm_device *dev;

    dev = drm_dev_alloc(&drm_dummy_driver);
    if (!dev) {
        DRM_ERROR("Failed to allocate DRM device.\n");
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

    drm_device_list_add(dev);

    return 0;
#else
    return -ENODEV;
#endif
}
