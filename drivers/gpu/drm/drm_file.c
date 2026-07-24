/*
 *
 *      drm_file.c
 *      DRM file private helpers — event queue, read, poll
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

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

    if (drm_ht_create(&file->magiclist, 4)) {
        drm_idr_destroy(&file->object_idr);
        free(file);
        return NULL;
    }

    file->authenticated        = false;
    file->universal_planes     = false;
    file->atomic               = false;
    file->aspect_ratio_allowed = false;
    file->event_list_head      = NULL;
    file->event_list_tail      = NULL;
    file->event_space          = 0;

    return file;
}

/* ------------------------------------------------------------------ */
/* drm_file_free — cleanup and free a drm_file                         */
/* ------------------------------------------------------------------ */

void drm_file_free(struct drm_file *file)
{
    if (!file) { return; }

    /* Free any pending events in the queue. */
    struct drm_event_node *node = file->event_list_head;
    while (node) {
        struct drm_event_node *next = node->next;
        free(node->event);
        free(node);
        node = next;
    }
    file->event_list_head = NULL;
    file->event_list_tail = NULL;

    drm_ht_destroy(&file->magiclist);
    drm_idr_destroy(&file->object_idr);
    free(file);
}

/* ------------------------------------------------------------------ */
/* drm_send_event — enqueue a DRM event for userspace delivery         */
/* ------------------------------------------------------------------ */

int drm_send_event(struct drm_device *dev, struct drm_pending_vblank_event *e)
{
    struct drm_event_node *node;
    struct drm_file       *file_priv;

    (void)dev;

    if (!e) return -EINVAL;

    /* Find the file that owns this event. For vblank events, the
     * event is associated with the file that queued it. In a full
     * implementation we'd track ownership. For now, deliver to the
     * first file in the device's filelist. */
    spin_lock(&dev->filelist_lock);
    {
        ilist_node_t *head = dev->filelist.next;
        if (!head || head == &dev->filelist) {
            spin_unlock(&dev->filelist_lock);
            return -ENOENT;
        }
        file_priv = container_of(head, struct drm_file, head);
    }
    spin_unlock(&dev->filelist_lock);

    /* Allocate a queue node and copy the event. */
    node = malloc(sizeof(*node));
    if (!node) return -ENOMEM;

    node->event = malloc(e->event.base.length);
    if (!node->event) {
        free(node);
        return -ENOMEM;
    }
    memcpy(node->event, &e->event, e->event.base.length);
    node->next = NULL;

    /* Enqueue at tail. */
    spin_lock(&file_priv->event_lock);
    if (file_priv->event_list_tail) {
        file_priv->event_list_tail->next = node;
    } else {
        file_priv->event_list_head = node;
    }
    file_priv->event_list_tail = node;
    file_priv->event_space += (int)e->event.base.length;
    spin_unlock(&file_priv->event_lock);

    /* Call the event's destroy callback if set. */
    if (e->destroy) e->destroy(e);

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_read — read events from the drm file (blocking)                 */
/* ------------------------------------------------------------------ */

int drm_read(struct drm_file *file_priv, char *buf, size_t count, size_t *offset)
{
    struct drm_event_node *node;
    size_t                 copy_size;

    (void)offset;

    if (!file_priv || !buf || count == 0) return -EINVAL;

    /* Wait for an event to be available (busy-wait for now; a
     * proper wait-queue integration is planned). */
    for (int tries = 0; tries < 1000; tries++) {
        spin_lock(&file_priv->event_lock);
        if (file_priv->event_list_head) break;
        spin_unlock(&file_priv->event_lock);
        /* Yield-like delay: in a real kernel we'd block on a
         * waitqueue, but for now we poll with a small delay. */
        for (volatile int d = 0; d < 100000; d++) { /* nothing */
        }
    }

    spin_lock(&file_priv->event_lock);
    node = file_priv->event_list_head;
    if (!node) {
        spin_unlock(&file_priv->event_lock);
        return 0; /* timed out with no event */
    }

    /* Dequeue head. */
    file_priv->event_list_head = node->next;
    if (!file_priv->event_list_head) file_priv->event_list_tail = NULL;
    file_priv->event_space -= (int)node->event->length;

    copy_size = (count < node->event->length) ? count : node->event->length;
    spin_unlock(&file_priv->event_lock);

    memcpy(buf, node->event, copy_size);
    free(node->event);
    free(node);
    return (int)copy_size;
}

/* ------------------------------------------------------------------ */
/* drm_poll — poll for pending events                                  */
/* ------------------------------------------------------------------ */

unsigned int drm_poll(struct drm_file *file_priv, unsigned int events)
{
    unsigned int mask = 0;

    if (!file_priv) return 0;

    spin_lock(&file_priv->event_lock);
    if (file_priv->event_list_head) {
        if (events & 0x0001) mask |= 0x0001; /* POLLIN */
        if (events & 0x0004) mask |= 0x0004; /* POLLRDNORM */
    }
    /* DRM device is always writable (ioctl-based comms). */
    if (events & 0x0004) mask |= 0x0004; /* POLLOUT */
    spin_unlock(&file_priv->event_lock);

    return mask;
}
