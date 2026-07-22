/*
 *
 *      drm_file.c
 *      DRM file private helpers
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_hashtab.h>
#include <drivers/drm/drm_print.h>
#include <kernel/errno.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* drm_file_alloc — allocate and initialize a drm_file                 */
/* ------------------------------------------------------------------ */

struct drm_file *drm_file_alloc(struct drm_device *dev)
{
    struct drm_file *file;

    (void)dev;

    file = malloc(sizeof(*file));
    if (!file) { return NULL; }
    memset(file, 0, sizeof(*file));

    drm_idr_init(&file->object_idr);
    ilist_init(&file->fbs_head);
    ilist_init(&file->object_list);

    /* Spinlocks zero-initialized by memset above. */

    if (drm_ht_create(&file->magiclist, 4)) {
        drm_idr_destroy(&file->object_idr);
        free(file);
        return NULL;
    }

    file->authenticated        = false;
    file->universal_planes     = false;
    file->atomic               = false;
    file->aspect_ratio_allowed = false;

    return file;
}

/* ------------------------------------------------------------------ */
/* drm_file_free — cleanup and free a drm_file                         */
/* ------------------------------------------------------------------ */

void drm_file_free(struct drm_file *file)
{
    if (!file) { return; }

    drm_ht_destroy(&file->magiclist);
    drm_idr_destroy(&file->object_idr);
    free(file);
}

/* ------------------------------------------------------------------ */
/* drm_send_event — send a vblank event to userspace (stub for MVP)    */
/* ------------------------------------------------------------------ */

int drm_send_event(struct drm_device *dev, struct drm_pending_vblank_event *e)
{
    (void)dev;
    (void)e;

    /* vblank event delivery will be implemented when the vblank
     * subsystem is integrated. For now, return success. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_read — read events from the drm file (stub for MVP)             */
/* ------------------------------------------------------------------ */

int drm_read(struct drm_file *file_priv, char *buf, size_t count, size_t *offset)
{
    (void)file_priv;
    (void)buf;
    (void)count;
    (void)offset;

    /* Event-based reads will be implemented when the event queue
     * subsystem is integrated. For now, return 0 (no data). */
    return 0;
}