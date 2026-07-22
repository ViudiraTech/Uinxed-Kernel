/*
 *
 *      drm_auth.c
 *      DRM magic authentication
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm.h>
#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_hashtab.h>
#include <drivers/drm/drm_print.h>
#include <kernel/errno.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* drm_master — full definition (also defined in drm_drv.c; consistent) */
/* ------------------------------------------------------------------ */

struct drm_master {
        struct drm_device   *dev;
        spinlock_t           lock;
        int                  unique_len;
        char                *unique;
        struct drm_open_hash magiclist;
        ilist_node_t         magicfree;
        int                  refcount;
};

/* ------------------------------------------------------------------ */
/* Static counter for magic number generation.                         */
/* ------------------------------------------------------------------ */

static drm_magic_t magic_counter = 1;

/* ------------------------------------------------------------------ */
/* drm_getmagic — handle DRM_IOCTL_GET_MAGIC                           */
/* ------------------------------------------------------------------ */

int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_auth      *auth;
    struct drm_hash_item *item;

    (void)dev;

    if (!data || !file_priv) { return -EINVAL; }

    auth = (struct drm_auth *)data;

    item = malloc(sizeof(*item));
    if (!item) { return -ENOMEM; }
    memset(item, 0, sizeof(*item));

    spin_lock(&file_priv->magic_lock);
    item->key = (unsigned long)magic_counter++;
    spin_unlock(&file_priv->magic_lock);

    if (drm_ht_insert_item(&file_priv->magiclist, item)) {
        free(item);
        return -ENOMEM;
    }

    auth->magic = (drm_magic_t)item->key;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_authmagic — handle DRM_IOCTL_AUTH_MAGIC                         */
/* ------------------------------------------------------------------ */

int drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_auth      *auth;
    struct drm_hash_item *item;

    (void)dev;

    if (!data || !file_priv) { return -EINVAL; }

    auth = (struct drm_auth *)data;

    spin_lock(&file_priv->magic_lock);
    if (drm_ht_find_item(&file_priv->magiclist, (unsigned long)auth->magic, &item)) {
        spin_unlock(&file_priv->magic_lock);
        return -EINVAL;
    }
    spin_unlock(&file_priv->magic_lock);

    /* Magic found — authenticate this file. */
    file_priv->authenticated = true;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_setmaster — handle DRM_IOCTL_SET_MASTER                         */
/* ------------------------------------------------------------------ */

int drm_setmaster(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_master *master;

    (void)data;

    if (!dev || !file_priv) { return -EINVAL; }

    if (file_priv->master) { return -EINVAL; }

    master = malloc(sizeof(*master));
    if (!master) { return -ENOMEM; }
    memset(master, 0, sizeof(*master));

    master->dev      = dev;
    master->refcount = 1;

    /* Spinlock zero-initialized by memset above. */

    if (drm_ht_create(&master->magiclist, 4)) {
        free(master);
        return -ENOMEM;
    }

    ilist_init(&master->magicfree);

    file_priv->master = master;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_dropmaster — handle DRM_IOCTL_DROP_MASTER                       */
/* ------------------------------------------------------------------ */

int drm_dropmaster(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_master *master;

    (void)dev;
    (void)data;

    if (!file_priv) { return -EINVAL; }

    master = file_priv->master;
    if (!master) { return -EINVAL; }

    drm_ht_destroy(&master->magiclist);
    free(master);
    file_priv->master = NULL;

    return 0;
}