/*
 *
 *      drm_vblank.c
 *      DRM vblank management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/firmware/acpi.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_idr.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_modeset_lock.h>
#include <drivers/gpu/drm/drm_print.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* drm_vblank_init: initialize vblank subsystem for @num_crtcs CRTCs   */
/* ------------------------------------------------------------------ */

int drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs)
{
    struct drm_vblank_crtc *vblank;
    unsigned int            i;

    if (!dev || num_crtcs == 0) { return -EINVAL; }

    vblank = malloc(sizeof(*vblank) * num_crtcs);
    if (!vblank) { return -ENOMEM; }
    memset(vblank, 0, sizeof(*vblank) * num_crtcs);

    for (i = 0; i < num_crtcs; i++) {
        vblank[i].dev              = dev;
        vblank[i].lock.lock        = 0;
        vblank[i].lock.rflags      = 0;
        vblank[i].pipe             = i;
        vblank[i].count            = 0;
        vblank[i].last             = 0;
        vblank[i].enabled          = false;
        vblank[i].inmodeset        = false;
        vblank[i].max_vblank_count = 0;
        vblank[i].event_queue      = NULL;
        vblank[i].refcount         = 0;
        vblank[i].period_ns        = 16666667ULL;
        vblank[i].next_vblank_ns   = 0;
        vblank[i].timestamp_ns     = 0;
        vblank[i].crtc             = NULL;
        wait_queue_init(&vblank[i].wait);
    }

    dev->vblank_unused_array = vblank;
    dev->num_crtc            = (int)num_crtcs;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_count: get vblank count for a CRTC                  */
/* ------------------------------------------------------------------ */

uint32_t drm_crtc_vblank_count(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) { return 0; }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) { return 0; }

    vblank = &dev->vblank_unused_array[crtc->index];

    return vblank->count;
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_get: enable vblank for this CRTC                    */
/* ------------------------------------------------------------------ */

int drm_crtc_vblank_get(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) { return -EINVAL; }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) { return -EINVAL; }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->crtc = crtc;
    vblank->refcount++;
    vblank->enabled = true;
    spin_unlock(&vblank->lock);

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_put: disable vblank for this CRTC                   */
/* ------------------------------------------------------------------ */

void drm_crtc_vblank_put(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) { return; }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) { return; }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    if (vblank->refcount) vblank->refcount--;
    if (!vblank->refcount && !vblank->event_queue) vblank->enabled = false;
    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_crtc_arm_vblank_event: queue a vblank event to the CRTC         */
/* ------------------------------------------------------------------ */

void drm_crtc_arm_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev || !e) { return; }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) { return; }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);

    e->pipe      = crtc->index;
    e->crtc      = crtc;
    vblank->crtc = crtc;
    e->next      = NULL;
    if (e->file_priv && !e->file_ref) {
        spin_lock(&e->file_priv->event_lock);
        if (e->file_priv->event_closing) {
            spin_unlock(&e->file_priv->event_lock);
            if (e->vblank_ref && vblank->refcount) {
                vblank->refcount--;
                e->vblank_ref = false;
            }
            spin_unlock(&vblank->lock);
            free(e);
            return;
        }
        e->file_priv->event_refs++;
        e->file_ref = true;
        spin_unlock(&e->file_priv->event_lock);
    }

    if (vblank->event_queue == NULL || e->sequence < vblank->event_queue->sequence) {
        e->next             = vblank->event_queue;
        vblank->event_queue = e;
    } else {
        struct drm_pending_vblank_event *cur = vblank->event_queue;

        while (cur->next != NULL && cur->next->sequence <= e->sequence) { cur = cur->next; }
        e->next   = cur->next;
        cur->next = e;
    }

    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_crtc_send_vblank_event: stamp and send an event to its owner    */
/* ------------------------------------------------------------------ */

void drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e)
{
    struct drm_vblank_crtc *vblank;
    uint64_t                timestamp;

    if (!e || !e->dev) return;
    if (!crtc) crtc = e->crtc;
    if (!crtc || crtc->index < 0 || crtc->index >= e->dev->num_crtc) {
        if (e->vblank_ref && e->crtc) {
            drm_crtc_vblank_put(e->crtc);
            e->vblank_ref = false;
        }
        if (e->file_ref && e->file_priv) {
            spin_lock(&e->file_priv->event_lock);
            if (e->file_priv->event_refs) e->file_priv->event_refs--;
            e->file_ref = false;
            spin_unlock(&e->file_priv->event_lock);
            wait_queue_wake_all(&e->file_priv->event_wait);
        }
        free(e);
        return;
    }

    vblank            = &e->dev->vblank_unused_array[crtc->index];
    timestamp         = vblank->timestamp_ns ? vblank->timestamp_ns : nano_time();
    e->event.sequence = (uint32_t)e->sequence;
    e->event.crtc_id  = crtc->base.id;
    e->event.tv_sec   = (uint32_t)(timestamp / 1000000000ULL);
    e->event.tv_usec  = (uint32_t)((timestamp / 1000ULL) % 1000000ULL);
    if (drm_send_event(e->dev, e)) free(e);
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_off: turn off vblank for a CRTC                     */
/* ------------------------------------------------------------------ */

static void drm_crtc_vblank_off(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) { return; }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) { return; }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->enabled        = false;
    vblank->next_vblank_ns = 0;
    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_on: turn on vblank for a CRTC                       */
/* ------------------------------------------------------------------ */

static void drm_crtc_vblank_on(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) { return; }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) { return; }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->enabled = true;
    if (!vblank->next_vblank_ns) vblank->next_vblank_ns = nano_time() + vblank->period_ns;
    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_handle_vblank: handle a vblank interrupt for the given pipe     */
/* ------------------------------------------------------------------ */

void drm_handle_vblank(struct drm_device *dev, unsigned int pipe)
{
    struct drm_vblank_crtc           *vblank;
    struct drm_pending_vblank_event  *ready = NULL;
    struct drm_pending_vblank_event **tail  = &ready;
    struct drm_crtc_helper_funcs     *helpers;

    if (!dev || (int)pipe >= dev->num_crtc) { return; }

    vblank = &dev->vblank_unused_array[pipe];

    spin_lock(&vblank->lock);

    vblank->count++;
    vblank->last         = vblank->count;
    vblank->timestamp_ns = nano_time();

    while (vblank->event_queue && vblank->event_queue->sequence <= vblank->count) {
        struct drm_pending_vblank_event *e = vblank->event_queue;
        vblank->event_queue                = e->next;
        e->next                            = NULL;
        *tail                              = e;
        tail                               = &e->next;
    }
    spin_unlock(&vblank->lock);
    wait_queue_wake_all(&vblank->wait);

    helpers = vblank->crtc ? (struct drm_crtc_helper_funcs *)vblank->crtc->helper_private : NULL;
    if (helpers && helpers->vblank) helpers->vblank(vblank->crtc);

    if (vblank->crtc) {
        bool completed_flip = false;
        spin_lock(&vblank->crtc->commit_lock);
        if (vblank->crtc->page_flip_pending && vblank->crtc->page_flip_target <= vblank->count) {
            vblank->crtc->page_flip_pending = false;
            vblank->crtc->page_flip_target  = 0;
            completed_flip                  = true;
        }
        spin_unlock(&vblank->crtc->commit_lock);
        if (completed_flip) drm_crtc_vblank_put(vblank->crtc);
    }

    while (ready) {
        struct drm_pending_vblank_event *e = ready;
        ready                              = e->next;
        if (e->vblank_ref && e->crtc) {
            e->vblank_ref = false;
            drm_crtc_vblank_put(e->crtc);
        }
        drm_crtc_send_vblank_event(e->crtc, e);
    }
}

void drm_vblank_tick(void)
{
    extern struct drm_device *drm_get_singleton(void);
    struct drm_device        *dev = drm_get_singleton();
    uint64_t                  now = nano_time();

    if (!dev || !dev->vblank_unused_array) return;
    for (int i = 0; i < dev->num_crtc; i++) {
        struct drm_vblank_crtc *vblank = &dev->vblank_unused_array[i];
        bool                    due;

        spin_lock(&vblank->lock);
        if (!vblank->enabled) {
            spin_unlock(&vblank->lock);
            continue;
        }
        if (!vblank->next_vblank_ns) vblank->next_vblank_ns = now + vblank->period_ns;
        due = now >= vblank->next_vblank_ns;
        if (due) {
            do {
                vblank->next_vblank_ns += vblank->period_ns;
            } while (now >= vblank->next_vblank_ns);
        }
        spin_unlock(&vblank->lock);
        if (due) drm_handle_vblank(dev, (unsigned int)i);
    }
}

/* ------------------------------------------------------------------ */
/* drm_wait_vblank_ioctl: handle DRM_IOCTL_WAIT_VBLANK                  */
/* ------------------------------------------------------------------ */

int drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    union drm_wait_vblank  *vblwait = (union drm_wait_vblank *)data;
    unsigned int            pipe;
    unsigned int            flags;
    struct drm_vblank_crtc *vblank;
    uint32_t                target;
    uint32_t                current;
    uint32_t                allowed;

    if (!dev || !vblwait) return -EINVAL;
    flags   = vblwait->request.type;
    allowed = _DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK | _DRM_VBLANK_HIGH_CRTC_MASK;
    if (flags & ~allowed) return -EINVAL;
    if (flags & (_DRM_VBLANK_SIGNAL | _DRM_VBLANK_FLIP)) return -EINVAL;

    pipe = (flags & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
    if ((flags & _DRM_VBLANK_SECONDARY) && !pipe) pipe = 1;

    if (pipe >= (unsigned int)dev->num_crtc) { return -EINVAL; }

    vblank = &dev->vblank_unused_array[pipe];
    if (!vblank->crtc) return -EINVAL;

    spin_lock(&vblank->lock);
    current = vblank->count;
    target  = (flags & _DRM_VBLANK_RELATIVE) ? current + vblwait->request.sequence : vblwait->request.sequence;
    if ((flags & _DRM_VBLANK_NEXTONMISS) && (int32_t)(current - target) >= 0) target = current + 1;
    spin_unlock(&vblank->lock);

    /* Handle event request */
    if (flags & _DRM_VBLANK_EVENT) {
        struct drm_pending_vblank_event *e;

        e = malloc(sizeof(*e));
        if (!e) { return -ENOMEM; }
        memset(e, 0, sizeof(*e));

        e->dev               = dev;
        e->file_priv         = file_priv;
        e->pipe              = pipe;
        e->crtc              = vblank->crtc;
        e->event.base.type   = DRM_EVENT_VBLANK;
        e->event.base.length = sizeof(e->event);
        e->event.user_data   = vblwait->request.signal;
        e->event.crtc_id     = e->crtc->base.id;
        e->sequence          = target;

        if (drm_crtc_vblank_get(e->crtc)) {
            free(e);
            return -EINVAL;
        }
        e->vblank_ref = true;
        if ((int32_t)(current - target) >= 0) {
            e->sequence = current;
            drm_crtc_vblank_put(e->crtc);
            e->vblank_ref = false;
            drm_crtc_send_vblank_event(e->crtc, e);
        } else {
            drm_crtc_arm_vblank_event(e->crtc, e);
        }

        /* Fill reply */
        vblwait->reply.sequence  = vblank->count;
        vblwait->reply.tval_sec  = (int)(vblank->timestamp_ns / 1000000000ULL);
        vblwait->reply.tval_usec = (int)((vblank->timestamp_ns / 1000ULL) % 1000000ULL);

        return 0;
    }

    if (drm_crtc_vblank_get(vblank->crtc)) return -EINVAL;
    for (;;) {
        spin_lock(&vblank->lock);
        current = vblank->count;
        if ((int32_t)(current - target) >= 0) {
            spin_unlock(&vblank->lock);
            break;
        }
        wait_queue_prepare(&vblank->wait);
        spin_unlock(&vblank->lock);
        wait_queue_sleep();
    }
    drm_crtc_vblank_put(vblank->crtc);

    vblwait->reply.sequence  = current;
    vblwait->reply.tval_sec  = (int)(vblank->timestamp_ns / 1000000000ULL);
    vblwait->reply.tval_usec = (int)((vblank->timestamp_ns / 1000ULL) % 1000000ULL);

    return 0;
}

void drm_vblank_cancel_pending(struct drm_device *dev, struct drm_file *file_priv)
{
    if (!dev || !file_priv || !dev->vblank_unused_array) return;

    for (int i = 0; i < dev->num_crtc; i++) {
        struct drm_vblank_crtc           *vblank = &dev->vblank_unused_array[i];
        struct drm_pending_vblank_event **link;

        spin_lock(&vblank->lock);
        link = &vblank->event_queue;
        while (*link) {
            struct drm_pending_vblank_event *event = *link;
            if (event->file_priv != file_priv) {
                link = &event->next;
                continue;
            }
            *link = event->next;
            if (event->vblank_ref && vblank->refcount) vblank->refcount--;
            if (event->file_ref) {
                spin_lock(&file_priv->event_lock);
                if (file_priv->event_refs) file_priv->event_refs--;
                event->file_ref = false;
                spin_unlock(&file_priv->event_lock);
                wait_queue_wake_all(&file_priv->event_wait);
            }
            free(event);
        }
        if (!vblank->refcount && !vblank->event_queue) vblank->enabled = false;
        spin_unlock(&vblank->lock);
    }
}

/* ------------------------------------------------------------------ */
/* drm_vblank_cleanup: free the vblank array                           */
/* ------------------------------------------------------------------ */

void drm_vblank_cleanup(struct drm_device *dev)
{
    if (!dev || !dev->vblank_unused_array) { return; }

    /* Free any pending events */
    {
        int i;

        for (i = 0; i < dev->num_crtc; i++) {
            struct drm_vblank_crtc          *vblank = &dev->vblank_unused_array[i];
            struct drm_pending_vblank_event *e      = vblank->event_queue;

            while (e) {
                struct drm_pending_vblank_event *next = e->next;

                free(e);
                e = next;
            }
            vblank->event_queue = NULL;
            wait_queue_wake_all(&vblank->wait);
        }
    }

    free(dev->vblank_unused_array);
    dev->vblank_unused_array = NULL;
    dev->num_crtc            = 0;
}
