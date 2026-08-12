/*
 *
 *      drm_file.c
 *      DRM file private helpers - event queue, read, poll
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_hashtab.h>
#include <drivers/gpu/drm/drm_print.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/list/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>

/* drm_file_alloc - allocate and initialize a drm_file */

struct drm_file *drm_file_alloc(struct drm_device *dev)
{
    struct drm_file *file;

    (void)dev;

    file = malloc(sizeof(*file));
    if (!file) {
        plogk("drm: File_alloc: out of memory.\n");
        return NULL;
    }
    memset(file, 0, sizeof(*file));

    drm_idr_init(&file->object_idr);
    ilist_init(&file->fbs_head);
    ilist_init(&file->object_list);
    ilist_init(&file->blobs_head);

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
    file->event_closing        = false;
    wait_queue_init(&file->event_wait);

    return file;
}

/* drm_file_free - cleanup and free a drm_file */

void drm_file_free(struct drm_file *file)
{
    if (!file) return;

    spin_lock(&file->event_lock);
    file->event_closing = true;
    while (file->event_refs) {
        wait_queue_prepare(&file->event_wait);
        spin_unlock(&file->event_lock);
        wait_queue_sleep();
        spin_lock(&file->event_lock);
    }
    spin_unlock(&file->event_lock);

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

/* drm_send_event - enqueue a DRM event for userspace delivery */

static void drm_event_release_file_ref(struct drm_pending_vblank_event *e)
{
    struct drm_file *file_priv;
    if (!e || !e->file_ref || !e->file_priv) return;
    file_priv = e->file_priv;
    spin_lock(&file_priv->event_lock);
    if (file_priv->event_refs) file_priv->event_refs--;
    e->file_ref = false;
    spin_unlock(&file_priv->event_lock);
    wait_queue_wake_all(&file_priv->event_wait);
}

int drm_send_event(struct drm_device *dev, struct drm_pending_vblank_event *e)
{
    struct drm_event_node *node;
    struct drm_file       *file_priv;

    if (!e) {
        plogk("drm: Send_event: NULL event.\n");
        return -EINVAL;
    }

    file_priv = e->file_priv;
    if (!file_priv) {
        spin_lock(&dev->filelist_lock);
        ilist_node_t *head = dev->filelist.next;
        if (!head || head == &dev->filelist) {
            spin_unlock(&dev->filelist_lock);
            plogk("drm: Send_event: no open drm_file to deliver event.\n");
            return -ENOENT;
        }
        file_priv = container_of(head, struct drm_file, head);
        spin_unlock(&dev->filelist_lock);
    }

    /* Allocate a queue node and copy the event. */
    node = malloc(sizeof(*node));
    if (!node) {
        plogk("drm: Send_event: out of memory allocating event queue node.\n");
        drm_event_release_file_ref(e);
        return -ENOMEM;
    }

    node->event = malloc(e->event.base.length);
    if (!node->event) {
        plogk("drm: Send_event: out of memory allocating event payload (%u bytes)\n", e->event.base.length);
        free(node);
        drm_event_release_file_ref(e);
        return -ENOMEM;
    }
    memcpy(node->event, &e->event, e->event.base.length);
    node->next = NULL;

    /* Enqueue at tail. */
    spin_lock(&file_priv->event_lock);
    if (file_priv->event_closing) {
        if (e->file_ref && file_priv->event_refs) file_priv->event_refs--;
        e->file_ref = false;
        spin_unlock(&file_priv->event_lock);
        wait_queue_wake_all(&file_priv->event_wait);
        free(node->event);
        free(node);
        if (e->destroy)
            e->destroy(e);
        else
            free(e);
        return 0;
    }
    if (file_priv->event_list_tail)
        file_priv->event_list_tail->next = node;
    else
        file_priv->event_list_head = node;
    file_priv->event_list_tail = node;
    file_priv->event_space += (int)e->event.base.length;
    if (e->file_ref && file_priv->event_refs) file_priv->event_refs--;
    e->file_ref = false;
    spin_unlock(&file_priv->event_lock);
    wait_queue_wake_all(&file_priv->event_wait);
    /*
     * Weston waits for page-flip completion through epoll on /dev/dri/card0.
     * Waking only event_wait reaches blocking drm_read() callers but leaves
     * VFS poll subscribers asleep forever, so publish POLLIN as well.
     */
    if (file_priv->filp_unused) vfs_poll_notify((vfs_node_t)file_priv->filp_unused, 0x0001);

    if (e->destroy)
        e->destroy(e);
    else
        free(e);

    return 0;
}

/* drm_read - read events from the drm file (blocking) */

int drm_read(struct drm_file *file_priv, char *buf, size_t count, size_t *offset, bool nonblock)
{
    struct drm_event_node *node;
    size_t                 copy_size;

    (void)offset;

    if (!file_priv || !buf || count == 0) {
        plogk("drm: Read: invalid arguments.\n");
        return -EINVAL;
    }

    for (;;) {
        spin_lock(&file_priv->event_lock);
        if (file_priv->event_list_head) break;
        if (file_priv->event_closing) {
            spin_unlock(&file_priv->event_lock);
            return 0;
        }
        if (nonblock) {
            spin_unlock(&file_priv->event_lock);
            return -EAGAIN;
        }
        process_t *proc = process_current();
        if (proc) {
            spin_lock(&proc->signal.lock);
            bool interrupted = signal_has_interrupting_pending(&proc->signal);
            spin_unlock(&proc->signal.lock);
            if (interrupted) {
                spin_unlock(&file_priv->event_lock);
                return -ERESTARTSYS;
            }
        }
        wait_queue_prepare(&file_priv->event_wait);
        spin_unlock(&file_priv->event_lock);
        wait_queue_sleep();
    }
    node = file_priv->event_list_head;
    if (count < node->event->length) {
        spin_unlock(&file_priv->event_lock);
        plogk("drm: Read: buffer too small for event (%zu < %u)\n", count, node->event->length);
        return -EINVAL;
    }

    /* Dequeue head. */
    file_priv->event_list_head = node->next;
    if (!file_priv->event_list_head) file_priv->event_list_tail = NULL;
    file_priv->event_space -= (int)node->event->length;

    copy_size = node->event->length;
    spin_unlock(&file_priv->event_lock);

    /*
     * VFS file_read callbacks receive a kernel bounce buffer from
     * sys_read(); the syscall layer performs the single copy_to_user after
     * this function returns.  Treating buf as a user pointer here makes the
     * copy fail, drops the dequeued page-flip event, and leaves Weston
     * waiting forever for its initial repaint completion.
     */
    memcpy(buf, node->event, copy_size);
    free(node->event);
    free(node);
    return (int)copy_size;
}

/* drm_poll - poll for pending events */

unsigned int drm_poll(struct drm_file *file_priv, unsigned int events)
{
    unsigned int mask = 0;

    if (!file_priv) return 0;

    spin_lock(&file_priv->event_lock);
    if (file_priv->event_list_head) {
        if (events & 0x0001) mask |= 0x0001; // POLLIN
        if (events & 0x0040) mask |= 0x0040; // POLLRDNORM
    }
    /* DRM device is always writable (ioctl-based comms). */
    if (events & 0x0004) mask |= 0x0004; // POLLOUT
    spin_unlock(&file_priv->event_lock);

    return mask;
}
