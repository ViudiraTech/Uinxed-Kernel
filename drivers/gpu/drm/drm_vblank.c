/*
 *
 *      drm_vblank.c
 *      DRM vblank management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_device.h>
#include <drm/drm_mode.h>
#include <drm/drm_idr.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <alloc.h>
#include <errno.h>
#include <string.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Locally-defined vblank structs (forward-declared in drm_device.h)   */
/* ------------------------------------------------------------------ */

struct drm_event {
    uint32_t type;
    uint32_t length;
};

struct drm_event_vblank {
    struct drm_event base;
    uint32_t crtc_id;
    uint32_t seq;
    uint32_t time;
};

struct drm_pending_vblank_event {
    struct drm_device *dev;
    unsigned int pipe;
    uint64_t sequence;
    struct drm_event_vblank event;
    void (*destroy)(struct drm_pending_vblank_event *e);
    struct drm_pending_vblank_event *next;
};

struct drm_vblank_crtc {
    struct drm_device *dev;
    spinlock_t lock;
    unsigned int pipe;
    uint32_t count;
    uint32_t last;
    bool enabled;
    bool inmodeset;
    uint32_t max_vblank_count;
    struct drm_pending_vblank_event *event_queue;
};

/* ------------------------------------------------------------------ */
/* drm_vblank_init: initialize vblank subsystem for @num_crtcs CRTCs   */
/* ------------------------------------------------------------------ */

int drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs)
{
    struct drm_vblank_crtc *vblank;
    unsigned int i;

    if (!dev || num_crtcs == 0) {
        return -EINVAL;
    }

    vblank = malloc(sizeof(*vblank) * num_crtcs);
    if (!vblank) {
        return -ENOMEM;
    }
    memset(vblank, 0, sizeof(*vblank) * num_crtcs);

    for (i = 0; i < num_crtcs; i++) {
        vblank[i].dev = dev;
        vblank[i].lock.lock = 0;
        vblank[i].lock.rflags = 0;
        vblank[i].pipe = i;
        vblank[i].count = 0;
        vblank[i].last = 0;
        vblank[i].enabled = false;
        vblank[i].inmodeset = false;
        vblank[i].max_vblank_count = 0;
        vblank[i].event_queue = NULL;
    }

    dev->vblank_unused_array = vblank;
    dev->num_crtc = (int)num_crtcs;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_count: get vblank count for a CRTC                  */
/* ------------------------------------------------------------------ */

uint32_t drm_crtc_vblank_count(struct drm_crtc *crtc)
{
    struct drm_device *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        return 0;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        return 0;
    }

    vblank = &dev->vblank_unused_array[crtc->index];

    return vblank->count;
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_get: enable vblank for this CRTC                    */
/* ------------------------------------------------------------------ */

int drm_crtc_vblank_get(struct drm_crtc *crtc)
{
    struct drm_device *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        return -EINVAL;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        return -EINVAL;
    }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->enabled = true;
    spin_unlock(&vblank->lock);

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_put: disable vblank for this CRTC                   */
/* ------------------------------------------------------------------ */

void drm_crtc_vblank_put(struct drm_crtc *crtc)
{
    struct drm_device *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        return;
    }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->enabled = false;
    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_crtc_arm_vblank_event: queue a vblank event to the CRTC         */
/* ------------------------------------------------------------------ */

void drm_crtc_arm_vblank_event(struct drm_crtc *crtc,
                                struct drm_pending_vblank_event *e)
{
    struct drm_device *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev || !e) {
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        return;
    }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);

    e->pipe = crtc->index;
    e->next = NULL;

    if (vblank->event_queue == NULL) {
        vblank->event_queue = e;
    } else {
        struct drm_pending_vblank_event *cur = vblank->event_queue;

        while (cur->next != NULL) {
            cur = cur->next;
        }
        cur->next = e;
    }

    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_crtc_send_vblank_event: send event to userspace (MVP: free)     */
/* ------------------------------------------------------------------ */

void drm_crtc_send_vblank_event(struct drm_crtc *crtc,
                                 struct drm_pending_vblank_event *e)
{
    (void)crtc;

    /* MVP: free the event since userspace delivery is not implemented */
    if (e) {
        free(e);
    }
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_off: turn off vblank for a CRTC                     */
/* ------------------------------------------------------------------ */

void drm_crtc_vblank_off(struct drm_crtc *crtc)
{
    struct drm_device *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        return;
    }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->enabled = false;
    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_crtc_vblank_on: turn on vblank for a CRTC                       */
/* ------------------------------------------------------------------ */

void drm_crtc_vblank_on(struct drm_crtc *crtc)
{
    struct drm_device *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        return;
    }

    vblank = &dev->vblank_unused_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->enabled = true;
    vblank->count++;
    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_handle_vblank: handle a vblank interrupt for the given pipe     */
/* ------------------------------------------------------------------ */

void drm_handle_vblank(struct drm_device *dev, unsigned int pipe)
{
    struct drm_vblank_crtc *vblank;

    if (!dev || (int)pipe >= dev->num_crtc) {
        return;
    }

    vblank = &dev->vblank_unused_array[pipe];

    spin_lock(&vblank->lock);

    vblank->count++;
    vblank->last = vblank->count;

    /* Process pending events */
    if (vblank->event_queue) {
        struct drm_pending_vblank_event *e = vblank->event_queue;

        vblank->event_queue = e->next;
        e->sequence = vblank->count;

        spin_unlock(&vblank->lock);

        drm_crtc_send_vblank_event(NULL, e);

        return;
    }

    spin_unlock(&vblank->lock);
}

/* ------------------------------------------------------------------ */
/* drm_wait_vblank_ioctl: handle DRM_IOCTL_WAIT_VBLANK                  */
/* ------------------------------------------------------------------ */

int drm_wait_vblank_ioctl(struct drm_device *dev, void *data,
                           struct drm_file *file_priv)
{
    union drm_wait_vblank *vblwait = (union drm_wait_vblank *)data;
    unsigned int pipe;
    unsigned int flags;
    struct drm_vblank_crtc *vblank;

    (void)file_priv;

    flags = vblwait->request.type;
    pipe = (flags & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT;

    if (pipe >= (unsigned int)dev->num_crtc) {
        return -EINVAL;
    }

    vblank = &dev->vblank_unused_array[pipe];

    /* Handle event request */
    if (flags & _DRM_VBLANK_EVENT) {
        struct drm_pending_vblank_event *e;

        e = malloc(sizeof(*e));
        if (!e) {
            return -ENOMEM;
        }
        memset(e, 0, sizeof(*e));

        e->dev = dev;
        e->pipe = pipe;
        e->event.base.type = 0; /* DRM_EVENT_VBLANK */
        e->event.base.length = sizeof(e->event);
        e->event.crtc_id = 0;

        if (flags & _DRM_VBLANK_RELATIVE) {
            e->sequence = vblank->count + vblwait->request.sequence;
        } else {
            e->sequence = vblwait->request.sequence;
        }

        /* Queue the event */
        spin_lock(&vblank->lock);

        e->next = NULL;
        if (vblank->event_queue == NULL) {
            vblank->event_queue = e;
        } else {
            struct drm_pending_vblank_event *cur = vblank->event_queue;

            while (cur->next != NULL) {
                cur = cur->next;
            }
            cur->next = e;
        }

        spin_unlock(&vblank->lock);

        /* Fill reply */
        vblwait->reply.sequence = vblank->count;
        vblwait->reply.tval_sec = 0;
        vblwait->reply.tval_usec = 0;

        return 0;
    }

    /* For non-event wait, just return the current count */
    vblwait->reply.sequence = vblank->count;
    vblwait->reply.tval_sec = 0;
    vblwait->reply.tval_usec = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_vblank_cleanup: free the vblank array                           */
/* ------------------------------------------------------------------ */

void drm_vblank_cleanup(struct drm_device *dev)
{
    if (!dev || !dev->vblank_unused_array) {
        return;
    }

    /* Free any pending events */
    {
        int i;

        for (i = 0; i < dev->num_crtc; i++) {
            struct drm_vblank_crtc *vblank = &dev->vblank_unused_array[i];
            struct drm_pending_vblank_event *e = vblank->event_queue;

            while (e) {
                struct drm_pending_vblank_event *next = e->next;

                free(e);
                e = next;
            }
            vblank->event_queue = NULL;
        }
    }

    free(dev->vblank_unused_array);
    dev->vblank_unused_array = NULL;
    dev->num_crtc = 0;
}