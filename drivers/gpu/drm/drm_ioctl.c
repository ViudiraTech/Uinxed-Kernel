/*
 *
 *      drm_ioctl.c
 *      DRM ioctl dispatch
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Forward declarations for auth functions (defined in drm_auth.c). */
extern int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_setmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_dropmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* ------------------------------------------------------------------ */
/* drm_ioctl_desc — ioctl descriptor (defined here, forward-declared   */
/*                  in drm_device.h)                                   */
/* ------------------------------------------------------------------ */

struct drm_ioctl_desc {
    unsigned int cmd;
    int (*func)(struct drm_device *dev, void *data, struct drm_file *file_priv);
    unsigned int flags;
};

/* ioctl permission flags */
#define DRM_AUTH      0x1
#define DRM_MASTER    0x2
#define DRM_ROOT_ONLY 0x4
#define DRM_UNLOCKED  0x8

/* ------------------------------------------------------------------ */
/* drm_ioctl_permit — check auth / master flags against file_priv      */
/* ------------------------------------------------------------------ */

int drm_ioctl_permit(unsigned int flags, struct drm_file *file_priv)
{
    if (!file_priv) {
        return -EACCES;
    }

    if (flags & DRM_AUTH) {
        if (!file_priv->authenticated) {
            return -EACCES;
        }
    }

    if (flags & DRM_MASTER) {
        if (!file_priv->master) {
            return -EACCES;
        }
    }

    /* DRM_ROOT_ONLY — no root concept in freestanding kernel;
     * always deny for safety. */
    if (flags & DRM_ROOT_ONLY) {
        return -EACCES;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_ioctl — dispatch an ioctl command to the registered handler     */
/* ------------------------------------------------------------------ */

/* Built-in core ioctls that are always available. */
static const struct drm_ioctl_desc drm_core_ioctls[] = {
    { DRM_IOCTL_VERSION,     drm_version,   0 },
    { DRM_IOCTL_GET_UNIQUE,  NULL,          0 },
    { DRM_IOCTL_SET_VERSION, drm_setversion, DRM_MASTER | DRM_AUTH },
    { DRM_IOCTL_SET_MASTER,  drm_setmaster,  DRM_MASTER },
    { DRM_IOCTL_DROP_MASTER, drm_dropmaster, DRM_MASTER },
    { DRM_IOCTL_GET_MAGIC,   drm_getmagic,   DRM_AUTH },
    { DRM_IOCTL_AUTH_MAGIC,  drm_authmagic,  DRM_AUTH },
};

int drm_ioctl(struct drm_device *dev, unsigned int cmd, void *data, struct drm_file *file_priv)
{
    const struct drm_ioctl_desc *descs;
    int num_descs;
    int i;
    int ret;
    unsigned int nr;

    if (!dev || !dev->driver || !file_priv) {
        return -EINVAL;
    }

    nr = _IOC_NR(cmd);

    /* 1. Check driver-specific ioctls first. */
    descs = dev->driver->ioctls;
    num_descs = dev->driver->num_ioctls;
    if (descs && num_descs > 0) {
        for (i = 0; i < num_descs; i++) {
            if (_IOC_NR(descs[i].cmd) == nr) {
                ret = drm_ioctl_permit(descs[i].flags, file_priv);
                if (ret) {
                    return ret;
                }
                if (descs[i].func) {
                    return descs[i].func(dev, data, file_priv);
                }
                return -ENOTTY;
            }
        }
    }

    /* 2. Fall back to core ioctls. */
    descs = drm_core_ioctls;
    num_descs = sizeof(drm_core_ioctls) / sizeof(drm_core_ioctls[0]);
    for (i = 0; i < num_descs; i++) {
        if (_IOC_NR(descs[i].cmd) == nr) {
            ret = drm_ioctl_permit(descs[i].flags, file_priv);
            if (ret) {
                return ret;
            }
            if (descs[i].func) {
                return descs[i].func(dev, data, file_priv);
            }
            return 0; /* NULL func = success (e.g. GET_UNIQUE for now) */
        }
    }

    return -ENOTTY;
}

/* ------------------------------------------------------------------ */
/* drm_version — handle DRM_IOCTL_VERSION                              */
/* ------------------------------------------------------------------ */

int drm_version(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_version *ver;

    (void)file_priv;

    if (!dev || !data) {
        return -EINVAL;
    }

    ver = (struct drm_version *)data;

    memset(ver, 0, sizeof(*ver));

    if (dev->driver) {
        ver->version_major = dev->driver->major;
        ver->version_minor = dev->driver->minor;
        ver->version_patchlevel = dev->driver->patchlevel;

        if (dev->driver->name) {
            ver->name_len = (__u64)strlen(dev->driver->name);
        }
        if (dev->driver->date) {
            ver->date_len = (__u64)strlen(dev->driver->date);
        }
        if (dev->driver->desc) {
            ver->desc_len = (__u64)strlen(dev->driver->desc);
        }
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