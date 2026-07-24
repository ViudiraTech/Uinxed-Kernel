/*
 *
 *      drm_ioctl.c
 *      DRM ioctl dispatch
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm.h>
#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_mode.h>
#include <kernel/errno.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

/* Forward declarations for all ioctl handler functions. */

/* auth (drm_auth.c) */
extern int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_setmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_dropmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* GEM (drm_gem.c) */
extern int drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS CRTC (drm_crtc.c) */
extern int drm_mode_getcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS encoder (drm_encoder.c) */
extern int drm_mode_getencoder(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS connector (drm_connector.c) */
extern int drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS plane (drm_plane.c) */
extern int drm_mode_getplane_res(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_getplane(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS framebuffer (drm_framebuffer.c) */
extern int drm_mode_addfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_addfb2(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_rmfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_getfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_dirtyfb(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS cursor / page-flip / atomic (drm_atomic_uapi.c) */
extern int drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS vblank (drm_vblank.c) */
extern int drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS resources (drm_mode_config.c) */
extern int drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS property (drm_property.c) */
extern int drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_obj_getproperties_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_obj_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS getfb2 (drm_framebuffer.c) */
extern int drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* ------------------------------------------------------------------ */
/* drm_ioctl_permit — check auth / master flags against file_priv      */
/* ------------------------------------------------------------------ */

int drm_ioctl_permit(unsigned int flags, struct drm_file *file_priv)
{
    if (!file_priv) { return -EACCES; }

    if (flags & DRM_AUTH) {
        if (!file_priv->authenticated) { return -EACCES; }
    }

    if (flags & DRM_MASTER) {
        if (!file_priv->master) { return -EACCES; }
    }

    /* DRM_ROOT_ONLY — no root concept in freestanding kernel;
     * always deny for safety. */
    if (flags & DRM_ROOT_ONLY) { return -EACCES; }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_get_cap / drm_set_client_cap — handlers                         */
/* ------------------------------------------------------------------ */

int drm_get_cap(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_get_cap *cap = (struct drm_get_cap *)data;

    (void)file_priv;

    if (!dev || !cap) { return -EINVAL; }

    switch (cap->capability) {
        case DRM_CAP_DUMB_BUFFER :
            cap->value = 1;
            break;
        case DRM_CAP_VBLANK_HIGH_CRTC :
            cap->value = 1;
            break;
        case DRM_CAP_DUMB_PREFERRED_DEPTH :
            cap->value = 32;
            break;
        case DRM_CAP_DUMB_PREFER_SHADOW :
            cap->value = 0;
            break;
        case DRM_CAP_PRIME :
            cap->value = DRM_PRIME_CAP_EXPORT | DRM_PRIME_CAP_IMPORT;
            break;
        case DRM_CAP_TIMESTAMP_MONOTONIC :
            cap->value = 1;
            break;
        case DRM_CAP_ASYNC_PAGE_FLIP :
            cap->value = 0;
            break;
        case DRM_CAP_CURSOR_WIDTH :
            cap->value = dev->mode_config.cursor_width;
            break;
        case DRM_CAP_CURSOR_HEIGHT :
            cap->value = dev->mode_config.cursor_height;
            break;
        case DRM_CAP_ADDFB2_MODIFIERS :
            cap->value = 0;
            break;
        case DRM_CAP_PAGE_FLIP_TARGET :
            cap->value = 0;
            break;
        case DRM_CAP_CRTC_IN_VBLANK_EVENT :
            cap->value = 0;
            break;
        case DRM_CAP_SYNCOBJ :
            cap->value = 0;
            break;
        case DRM_CAP_SYNCOBJ_TIMELINE :
            cap->value = 0;
            break;
        case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP :
            cap->value = 0;
            break;
        default :
            cap->value = 0;
            break;
    }

    return 0;
}

int drm_set_client_cap(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_set_client_cap *cap = (struct drm_set_client_cap *)data;

    (void)dev;

    if (!data || !file_priv) { return -EINVAL; }

    switch (cap->capability) {
        case DRM_CLIENT_CAP_STEREO_3D :
            file_priv->stereo3d_allowed_unused = (cap->value != 0);
            break;
        case DRM_CLIENT_CAP_UNIVERSAL_PLANES :
            file_priv->universal_planes = (cap->value != 0);
            break;
        case DRM_CLIENT_CAP_ATOMIC :
            file_priv->atomic = (cap->value != 0);
            break;
        case DRM_CLIENT_CAP_ASPECT_RATIO :
            file_priv->aspect_ratio_allowed = (cap->value != 0);
            break;
        case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS :
            file_priv->writeback_connectors_allowed_unused = (cap->value != 0);
            break;
        case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT :
            break;
        default :
            return -EINVAL;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_ioctl — dispatch an ioctl command to the registered handler     */
/* ------------------------------------------------------------------ */

/* Built-in core ioctls that are always available. */
static const struct drm_ioctl_desc drm_core_ioctls[] = {
    /* 0x00 - 0x0d: core / GEM / cap */
    {DRM_IOCTL_VERSION,                drm_version,                      0                    },
    {DRM_IOCTL_GET_UNIQUE,             NULL,                             0                    },
    {DRM_IOCTL_GET_MAGIC,              drm_getmagic,                     DRM_AUTH             },
    {DRM_IOCTL_SET_VERSION,            drm_setversion,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_SET_MASTER,             drm_setmaster,                    DRM_MASTER           },
    {DRM_IOCTL_DROP_MASTER,            drm_dropmaster,                   DRM_MASTER           },
    {DRM_IOCTL_AUTH_MAGIC,             drm_authmagic,                    DRM_AUTH             },
    {DRM_IOCTL_GEM_CLOSE,              drm_gem_close_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_FLINK,              drm_gem_flink_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_OPEN,               drm_gem_open_ioctl,               DRM_AUTH             },
    {DRM_IOCTL_GET_CAP,                drm_get_cap,                      0                    },
    {DRM_IOCTL_SET_CLIENT_CAP,         drm_set_client_cap,               0                    },

    /* 0x2d - 0x2e: PRIME */
    {DRM_IOCTL_PRIME_HANDLE_TO_FD,     NULL,                             DRM_AUTH             },
    {DRM_IOCTL_PRIME_FD_TO_HANDLE,     NULL,                             DRM_AUTH             },

    /* 0x3a: vblank */
    {DRM_IOCTL_WAIT_VBLANK,            drm_wait_vblank_ioctl,            0                    },

    /* 0xA0 - 0xBF: KMS */
    {DRM_IOCTL_MODE_GETRESOURCES,      drm_mode_getresources,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCRTC,           drm_mode_getcrtc,                 DRM_AUTH             },
    {DRM_IOCTL_MODE_SETCRTC,           drm_mode_setcrtc,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR,            drm_mode_cursor_ioctl,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETENCODER,        drm_mode_getencoder,              DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCONNECTOR,      drm_mode_getconnector,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPROPERTY,       drm_mode_getproperty_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPROPERTY,       NULL,                             DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETPROPBLOB,       NULL,                             DRM_AUTH             },
    {DRM_IOCTL_MODE_GETFB,             drm_mode_getfb,                   DRM_AUTH             },
    {DRM_IOCTL_MODE_ADDFB,             drm_mode_addfb,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_RMFB,              drm_mode_rmfb,                    DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_PAGE_FLIP,         drm_mode_page_flip_ioctl,         DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_DIRTYFB,           drm_mode_dirtyfb,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CREATE_DUMB,       NULL,                             DRM_AUTH             },
    {DRM_IOCTL_MODE_MAP_DUMB,          NULL,                             DRM_AUTH             },
    {DRM_IOCTL_MODE_DESTROY_DUMB,      NULL,                             DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPLANE,          drm_mode_getplane,                DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPLANE,          drm_mode_setplane,                DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB2,            drm_mode_addfb2,                  DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_mode_obj_getproperties_ioctl, DRM_AUTH             },
    {DRM_IOCTL_MODE_OBJ_SETPROPERTY,   drm_mode_obj_setproperty_ioctl,   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR2,           drm_mode_cursor2_ioctl,           DRM_AUTH             },
    {DRM_IOCTL_MODE_ATOMIC,            drm_mode_atomic_ioctl,            DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB2,            drm_mode_getfb2_ioctl,            DRM_AUTH             },
};

/* ------------------------------------------------------------------ */
/* ioctl descriptor lookup — full command match                        */
/* ------------------------------------------------------------------ */

static const struct drm_ioctl_desc *find_ioctl_desc(unsigned int cmd, const struct drm_ioctl_desc *table, int count)
{
    for (int i = 0; i < count; i++) {
        if (table[i].cmd == cmd) return &table[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* drm_ioctl — validated dispatch                                     */
/* ------------------------------------------------------------------ */

int drm_ioctl(struct drm_device *dev, unsigned int cmd, void *user_data, struct drm_file *file_priv)
{
    const struct drm_ioctl_desc *desc  = NULL;
    void                        *kdata = NULL;
    unsigned int                 dir;
    unsigned int                 size;
    int                          ret;

    if (!dev || !dev->driver || !file_priv) return -EINVAL;

    /* 1. Validate DRM magic type byte. */
    if (_IOC_TYPE(cmd) != DRM_IOCTL_BASE) return -ENOTTY;

    /* 2. Validate direction bits. */
    dir = _IOC_DIR(cmd);
    if (dir & ~(_IOC_READ | _IOC_WRITE)) return -EINVAL;

    /* 3. Validate size is reasonable (max 16 KB). */
    size = _IOC_SIZE(cmd);
    if (size > 0x4000) return -EINVAL;

    /* 4. Allocate kernel buffer and copy from user if needed. */
    if (size > 0) {
        kdata = malloc(size);
        if (!kdata) return -ENOMEM;
        if (dir & _IOC_WRITE) {
            /* copy_from_user: kernel and user share the same address
             * space in this freestanding kernel, but we still make a
             * private copy so the handler cannot scribble on user
             * memory. */
            memcpy(kdata, user_data, size);
        } else {
            memset(kdata, 0, size);
        }
    }

    /* 5. Search driver ioctls (full cmd match). */
    if (dev->driver->ioctls && dev->driver->num_ioctls > 0) { desc = find_ioctl_desc(cmd, dev->driver->ioctls, dev->driver->num_ioctls); }

    /* 6. Dumb-buffer / PRIME fallback dispatch.
     *    These are handled separately because the core table has NULL
     *    func entries and we dispatch through the driver callbacks. */
    if (!desc) {
        if (cmd == DRM_IOCTL_MODE_CREATE_DUMB) {
            ret = drm_ioctl_permit(DRM_AUTH, file_priv);
            if (ret) goto out;
            ret = drm_gem_dumb_create(file_priv, dev, (struct drm_mode_create_dumb *)kdata);
            goto copy_out;
        }
        if (cmd == DRM_IOCTL_MODE_MAP_DUMB) {
            struct drm_mode_map_dumb *args = (struct drm_mode_map_dumb *)kdata;
            ret                            = drm_ioctl_permit(DRM_AUTH, file_priv);
            if (ret) goto out;
            ret = drm_gem_dumb_map_offset(file_priv, dev, args->handle, &args->offset);
            goto copy_out;
        }
        if (cmd == DRM_IOCTL_MODE_DESTROY_DUMB) {
            struct drm_mode_destroy_dumb *args = (struct drm_mode_destroy_dumb *)kdata;
            ret                                = drm_ioctl_permit(DRM_AUTH, file_priv);
            if (ret) goto out;
            ret = drm_gem_dumb_destroy(file_priv, dev, args->handle);
            goto out;
        }
        if (cmd == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
            struct drm_prime_handle *args = (struct drm_prime_handle *)kdata;
            ret                           = drm_ioctl_permit(DRM_AUTH, file_priv);
            if (ret) goto out;
            ret = drm_gem_prime_handle_to_fd(dev, file_priv, args->handle, args->flags, &args->fd);
            goto copy_out;
        }
        if (cmd == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
            struct drm_prime_handle *args = (struct drm_prime_handle *)kdata;
            ret                           = drm_ioctl_permit(DRM_AUTH, file_priv);
            if (ret) goto out;
            ret = drm_gem_prime_fd_to_handle(dev, file_priv, args->fd, &args->handle);
            goto copy_out;
        }
    }

    /* 7. Fall back to core ioctls. */
    if (!desc) { desc = find_ioctl_desc(cmd, drm_core_ioctls, sizeof(drm_core_ioctls) / sizeof(drm_core_ioctls[0])); }

    if (!desc) {
        ret = -ENOTTY;
        goto out;
    }

    /* 8. Permission check + dispatch. */
    ret = drm_ioctl_permit(desc->flags, file_priv);
    if (ret) goto out;

    if (!desc->func) {
        /* NULL func = no-op success (e.g. GET_UNIQUE stub). */
        ret = 0;
        goto out;
    }

    ret = desc->func(dev, kdata, file_priv);

copy_out:
    /* 9. Copy results back to user if the ioctl reads data. */
    if (ret == 0 && kdata && (dir & _IOC_READ)) memcpy(user_data, kdata, size);

out:
    free(kdata);
    return ret;
}

/* ------------------------------------------------------------------ */
/* drm_version — handle DRM_IOCTL_VERSION                              */
/* ------------------------------------------------------------------ */

int drm_version(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_version *ver;

    (void)file_priv;

    if (!dev || !data) { return -EINVAL; }

    ver = (struct drm_version *)data;

    memset(ver, 0, sizeof(*ver));

    if (dev->driver) {
        ver->version_major      = dev->driver->major;
        ver->version_minor      = dev->driver->minor;
        ver->version_patchlevel = dev->driver->patchlevel;

        if (dev->driver->name) { ver->name_len = (__u64)strlen(dev->driver->name); }
        if (dev->driver->date) { ver->date_len = (__u64)strlen(dev->driver->date); }
        if (dev->driver->desc) { ver->desc_len = (__u64)strlen(dev->driver->desc); }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_setversion — handle DRM_IOCTL_SET_VERSION (accept any version)  */
/* ------------------------------------------------------------------ */

int drm_setversion(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    (void)dev;
    (void)data;
    (void)file_priv;
    return 0;
}
