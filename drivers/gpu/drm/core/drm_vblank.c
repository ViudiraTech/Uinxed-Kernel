/*
 *
 *      drm_vblank.c
 *      DRM vblank management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_idr.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_modeset_lock.h>
#include <drivers/gpu/drm/drm_print.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/process.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>

/*
 * Global count of CRTCs with vblank enabled; lets drm_vblank_tick early-return
 * when nothing is waiting on a vblank instead of scanning every device and
 * churning refcounts on every 1 kHz timer tick.
 */
static uint32_t   drm_vblank_enabled_total;
static spinlock_t drm_vblank_enabled_lock = {.lock = 0, .rflags = 0};
static uint64_t   drm_vblank_next_ns      = UINT64_MAX;
static uint64_t   drm_vblank_generation;

/* IRQ-safe deadline hint for the software-vblank bottom half. */
bool drm_vblank_deferred_due(uint64_t monotonic_ns)
{
    if (!__atomic_load_n(&drm_vblank_enabled_total, __ATOMIC_ACQUIRE)) return false;
    return monotonic_ns >= __atomic_load_n(&drm_vblank_next_ns, __ATOMIC_ACQUIRE);
}

/* drm_vblank_init: initialize vblank subsystem for @num_crtcs CRTCs */
int drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs)
{
    struct drm_vblank_crtc *vblank;
    unsigned int            i;

    if (!dev || num_crtcs == 0) {
        DRM_ERROR("Vblank_init with invalid args (dev=%p, num_crtcs=%u), returning -EINVAL\n", dev, num_crtcs);
        return -EINVAL;
    }

    vblank = malloc(sizeof(*vblank) * num_crtcs);
    if (!vblank) {
        DRM_ERROR("Vblank_init: array allocation failed (num_crtcs=%u), returning -ENOMEM\n", num_crtcs);
        return -ENOMEM;
    }
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

    dev->vblank_array = vblank;
    dev->num_crtc     = (int)num_crtcs;

    return 0;
}

/* drm_crtc_vblank_count: get vblank count for a CRTC */
uint32_t drm_crtc_vblank_count(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        DRM_ERROR("Vblank_count with invalid crtc (crtc=%p), returning 0\n", crtc);
        return 0;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        DRM_ERROR("Vblank_count: crtc index %d out of range (num_crtc=%d), returning 0\n", crtc->index, dev->num_crtc);
        return 0;
    }

    vblank = &dev->vblank_array[crtc->index];

    return vblank->count;
}

/* drm_crtc_vblank_get: acquire a vblank reference for this CRTC */
int drm_crtc_vblank_get(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        DRM_ERROR("Vblank_get with invalid crtc (crtc=%p), returning -EINVAL\n", crtc);
        return -EINVAL;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        DRM_ERROR("Vblank_get: crtc index %d out of range (num_crtc=%d), returning -EINVAL\n", crtc->index, dev->num_crtc);
        return -EINVAL;
    }

    vblank = &dev->vblank_array[crtc->index];

    spin_lock(&vblank->lock);
    vblank->crtc = crtc;
    vblank->refcount++;
    spin_unlock(&vblank->lock);

    return 0;
}

/* drm_crtc_vblank_put: release a vblank reference for this CRTC */
void drm_crtc_vblank_put(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        DRM_ERROR("Vblank_put with invalid crtc (crtc=%p)\n", crtc);
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        DRM_ERROR("Vblank_put: crtc index %d out of range (num_crtc=%d)\n", crtc->index, dev->num_crtc);
        return;
    }

    vblank = &dev->vblank_array[crtc->index];

    spin_lock(&vblank->lock);
    if (vblank->refcount) vblank->refcount--;
    spin_unlock(&vblank->lock);
}

/* drm_crtc_arm_vblank_event: queue a vblank event to the CRTC */
void drm_crtc_arm_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev || !e) {
        DRM_ERROR("Arm_vblank_event with invalid args (crtc=%p, e=%p)\n", crtc, e);
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        DRM_ERROR("Arm_vblank_event: crtc index %d out of range (num_crtc=%d)\n", crtc->index, dev->num_crtc);
        return;
    }

    vblank = &dev->vblank_array[crtc->index];

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

        while (cur->next != NULL && cur->next->sequence <= e->sequence) cur = cur->next;
        e->next   = cur->next;
        cur->next = e;
    }

    spin_unlock(&vblank->lock);
}

/* drm_crtc_send_vblank_event: stamp and send an event to its owner */
void drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e)
{
    struct drm_vblank_crtc *vblank;
    uint64_t                timestamp;

    if (!e || !e->dev) {
        DRM_ERROR("Send_vblank_event with invalid args (e=%p, dev=%p)\n", e, e ? e->dev : NULL);
        return;
    }
    if (!crtc) crtc = e->crtc;
    if (!crtc || crtc->index < 0 || crtc->index >= e->dev->num_crtc) {
        DRM_ERROR("Send_vblank_event: crtc invalid (crtc=%p, index=%d, num_crtc=%d), dropping event.\n", crtc, crtc ? crtc->index : -1, e->dev->num_crtc);
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

    vblank            = &e->dev->vblank_array[crtc->index];
    timestamp         = vblank->timestamp_ns ? vblank->timestamp_ns : timer_monotonic_ns();
    e->event.sequence = (uint32_t)e->sequence;
    e->event.crtc_id  = crtc->base.id;
    e->event.tv_sec   = (uint32_t)(timestamp / 1000000000ULL);
    e->event.tv_usec  = (uint32_t)((timestamp / 1000ULL) % 1000000ULL);
    if (drm_send_event(e->dev, e)) free(e);
}

/* drm_crtc_vblank_off: turn off vblank for a CRTC */
void drm_crtc_vblank_off(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;

    if (!crtc || !crtc->dev) {
        DRM_ERROR("Vblank_off with invalid crtc (crtc=%p)\n", crtc);
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        DRM_ERROR("Vblank_off: crtc index %d out of range (num_crtc=%d)\n", crtc->index, dev->num_crtc);
        return;
    }

    vblank = &dev->vblank_array[crtc->index];

    spin_lock(&vblank->lock);
    if (vblank->enabled) {
        vblank->enabled = false;
        spin_lock(&drm_vblank_enabled_lock);
        if (drm_vblank_enabled_total) drm_vblank_enabled_total--;
        __atomic_add_fetch(&drm_vblank_generation, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&drm_vblank_next_ns, drm_vblank_enabled_total ? 0 : UINT64_MAX, __ATOMIC_RELEASE);
        spin_unlock(&drm_vblank_enabled_lock);
    }
    vblank->next_vblank_ns = 0;
    vblank->crtc           = NULL;
    spin_unlock(&vblank->lock);
}

/* drm_crtc_vblank_on: turn on vblank for a CRTC */
void drm_crtc_vblank_on(struct drm_crtc *crtc)
{
    struct drm_device      *dev;
    struct drm_vblank_crtc *vblank;
    int                     refresh;
    uint64_t                period_ns;

    if (!crtc || !crtc->dev) {
        DRM_ERROR("Vblank_on with invalid crtc (crtc=%p)\n", crtc);
        return;
    }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) {
        DRM_ERROR("Vblank_on: crtc index %d out of range (num_crtc=%d)\n", crtc->index, dev->num_crtc);
        return;
    }

    vblank = &dev->vblank_array[crtc->index];

    /*
     * Software-emulated vblank must follow the active mode.  Weston derives
     * repaint timing from the same clock/totals, so keeping the old hard-coded
     * period after a modeset causes frame pacing drift and event bursts.
     */
    refresh = drm_mode_vrefresh(&crtc->mode);
    if (refresh <= 0) refresh = crtc->mode.vrefresh;
    if (refresh <= 0 || refresh > 1000) refresh = 60;
    period_ns = (TIMER_NSEC_PER_SEC + (uint64_t)refresh / 2ULL) / (uint64_t)refresh;

    spin_lock(&vblank->lock);
    vblank->crtc = crtc;
    if (vblank->period_ns != period_ns) {
        vblank->period_ns      = period_ns;
        vblank->next_vblank_ns = timer_monotonic_ns() + period_ns;
    }
    if (!vblank->enabled) {
        vblank->enabled = true;
        spin_lock(&drm_vblank_enabled_lock);
        drm_vblank_enabled_total++;
        spin_unlock(&drm_vblank_enabled_lock);
    }
    if (!vblank->next_vblank_ns) vblank->next_vblank_ns = timer_monotonic_ns() + period_ns;
    uint64_t next_vblank_ns = vblank->next_vblank_ns;
    spin_unlock(&vblank->lock);

    spin_lock(&drm_vblank_enabled_lock);
    __atomic_add_fetch(&drm_vblank_generation, 1, __ATOMIC_RELEASE);
    uint64_t published = __atomic_load_n(&drm_vblank_next_ns, __ATOMIC_ACQUIRE);
    if (next_vblank_ns < published) __atomic_store_n(&drm_vblank_next_ns, next_vblank_ns, __ATOMIC_RELEASE);
    spin_unlock(&drm_vblank_enabled_lock);
}

/* drm_handle_vblank: handle a vblank interrupt for the given pipe */
void drm_handle_vblank(struct drm_device *dev, unsigned int pipe)
{
    struct drm_vblank_crtc           *vblank;
    struct drm_pending_vblank_event  *ready = NULL;
    struct drm_pending_vblank_event **tail  = &ready;
    struct drm_crtc_helper_funcs     *helpers;

    if (!dev || (int)pipe >= dev->num_crtc) {
        DRM_ERROR("Handle_vblank with invalid pipe (dev=%p, pipe=%u, num_crtc=%d)\n", dev, pipe, dev ? dev->num_crtc : -1);
        return;
    }

    vblank = &dev->vblank_array[pipe];

    spin_lock(&vblank->lock);

    vblank->count++;
    vblank->last         = vblank->count;
    vblank->timestamp_ns = timer_monotonic_ns();

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

/* Timer-driven vblank tick used to emulate vblank interrupts. */
void drm_vblank_tick(void)
{
    uint64_t           now;
    uint64_t           next       = UINT64_MAX;
    uint64_t           generation = __atomic_load_n(&drm_vblank_generation, __ATOMIC_ACQUIRE);
    struct drm_device *devs[DRM_MAX_DEVICES];
    int                ndev;

    /*
     * Fast path: no CRTC has vblank enabled, so skip the device-list scan,
     * refcount churn and per-CRTC spinlocks that cost nothing to avoid.
     */
    if (!__atomic_load_n(&drm_vblank_enabled_total, __ATOMIC_ACQUIRE)) return;

    now  = timer_monotonic_ns();
    ndev = drm_device_list_collect(devs, DRM_MAX_DEVICES);

    /*
     * Service every registered device, not just the first: each GPU owns a
     * separate set of CRTCs and its own vblank bookkeeping.  The references
     * taken by the collect keep each device alive for the whole tick; the
     * global list lock is acquired once rather than once per device.
     */
    for (int d = 0; d < ndev; d++) {
        struct drm_device *dev = devs[d];

        if (dev->vblank_array) {
            for (int i = 0; i < dev->num_crtc; i++) {
                struct drm_vblank_crtc *vblank = &dev->vblank_array[i];
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
                if (vblank->next_vblank_ns < next) next = vblank->next_vblank_ns;
                spin_unlock(&vblank->lock);
                if (due) drm_handle_vblank(dev, (unsigned int)i);
            }
        }
        drm_dev_put(dev);
    }

    spin_lock(&drm_vblank_enabled_lock);
    if (!drm_vblank_enabled_total)
        __atomic_store_n(&drm_vblank_next_ns, UINT64_MAX, __ATOMIC_RELEASE);
    else if (__atomic_load_n(&drm_vblank_generation, __ATOMIC_ACQUIRE) != generation)
        __atomic_store_n(&drm_vblank_next_ns, 0, __ATOMIC_RELEASE);
    else
        __atomic_store_n(&drm_vblank_next_ns, next, __ATOMIC_RELEASE);
    spin_unlock(&drm_vblank_enabled_lock);
}

/* drm_wait_vblank_ioctl: handle DRM_IOCTL_WAIT_VBLANK */
int drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    union drm_wait_vblank  *vblwait = (union drm_wait_vblank *)data;
    unsigned int            pipe;
    unsigned int            flags;
    struct drm_vblank_crtc *vblank;
    uint32_t                target;
    uint32_t                current;
    uint32_t                allowed;

    if (!dev || !vblwait) {
        DRM_ERROR("WAIT_VBLANK with invalid args (dev=%p, vblwait=%p), returning -EINVAL\n", dev, vblwait);
        return -EINVAL;
    }
    flags   = vblwait->request.type;
    allowed = _DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK | _DRM_VBLANK_HIGH_CRTC_MASK;
    if (flags & ~allowed) {
        DRM_ERROR("WAIT_VBLANK: unsupported flags 0x%x, returning -EINVAL\n", flags);
        return -EINVAL;
    }
    if (flags & (_DRM_VBLANK_SIGNAL | _DRM_VBLANK_FLIP)) {
        DRM_ERROR("WAIT_VBLANK: SIGNAL/FLIP not supported (flags=0x%x), returning -EINVAL\n", flags);
        return -EINVAL;
    }

    pipe = (flags & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
    if ((flags & _DRM_VBLANK_SECONDARY) && !pipe) pipe = 1;

    if (pipe >= (unsigned int)dev->num_crtc) {
        DRM_ERROR("WAIT_VBLANK: pipe %u out of range (num_crtc=%d), returning -EINVAL\n", pipe, dev->num_crtc);
        return -EINVAL;
    }

    vblank = &dev->vblank_array[pipe];
    if (!vblank->crtc) {
        DRM_ERROR("WAIT_VBLANK: no CRTC bound to pipe %u, returning -EINVAL\n", pipe);
        return -EINVAL;
    }

    spin_lock(&vblank->lock);
    current = vblank->count;
    target  = (flags & _DRM_VBLANK_RELATIVE) ? current + vblwait->request.sequence : vblwait->request.sequence;
    if ((flags & _DRM_VBLANK_NEXTONMISS) && (int32_t)(current - target) >= 0) target = current + 1;
    spin_unlock(&vblank->lock);

    /* Handle event request */
    if (flags & _DRM_VBLANK_EVENT) {
        struct drm_pending_vblank_event *e;

        e = malloc(sizeof(*e));
        if (!e) {
            DRM_ERROR("WAIT_VBLANK: event allocation failed, returning -ENOMEM\n");
            return -ENOMEM;
        }
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
            uint32_t crtc_id = e->crtc->base.id;
            free(e);
            DRM_ERROR("WAIT_VBLANK: vblank get failed for crtc %u, returning -EINVAL\n", crtc_id);
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

    if (drm_crtc_vblank_get(vblank->crtc)) {
        DRM_ERROR("WAIT_VBLANK: vblank get failed for pipe %u, returning -EINVAL\n", pipe);
        return -EINVAL;
    }
    for (;;) {
        spin_lock(&vblank->lock);
        current = vblank->count;
        if ((int32_t)(current - target) >= 0) {
            spin_unlock(&vblank->lock);
            break;
        }
        process_t *proc = process_current();
        if (proc) {
            spin_lock(&proc->signal.lock);
            bool interrupted = signal_has_interrupting_pending(&proc->signal);
            spin_unlock(&proc->signal.lock);
            if (interrupted) {
                spin_unlock(&vblank->lock);
                drm_crtc_vblank_put(vblank->crtc);
                return -ERESTARTSYS;
            }
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

/* Drop all pending vblank events owned by @file_priv. */
void drm_vblank_cancel_pending(struct drm_device *dev, struct drm_file *file_priv)
{
    if (!dev || !file_priv || !dev->vblank_array) {
        DRM_ERROR("Vblank_cancel_pending with invalid args (dev=%p, file_priv=%p)\n", dev, file_priv);
        return;
    }

    for (int i = 0; i < dev->num_crtc; i++) {
        struct drm_vblank_crtc           *vblank = &dev->vblank_array[i];
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
        spin_unlock(&vblank->lock);
    }
}

/* drm_vblank_cleanup: free the vblank array */
void drm_vblank_cleanup(struct drm_device *dev)
{
    if (!dev || !dev->vblank_array) {
        DRM_INFO("Vblank_cleanup called without vblank array initialized (dev=%p)\n", dev);
        return;
    }

    /* Free any pending events */
    {
        int i;

        for (i = 0; i < dev->num_crtc; i++) {
            struct drm_vblank_crtc          *vblank = &dev->vblank_array[i];
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

    free(dev->vblank_array);
    dev->vblank_array = NULL;
    dev->num_crtc     = 0;
}
