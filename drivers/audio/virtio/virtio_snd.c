/*
 *
 *      virtio_snd.c
 *      VirtIO sound (virtio-snd) driver
 *
 *      2026/10/04 By Yinyuan34513
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/audio/core/audio.h>
#include <drivers/audio/virtio/virtio_snd.h>
#include <drivers/bus/virtpci.h>
#include <drivers/firmware/apic.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <process/kthread.h>
#include <process/sched.h>
#include <sync/mutex.h>

#if CONFIG_AUDIO && CONFIG_AUDIO_VIRTIO_SND && CONFIG_VIRTIO_PCI

/* Descriptors asked of each virtqueue (the device may hand back fewer). */
#    define VIRTIO_SND_VQ_NUM 64U

/* I/O messages buffered per direction; every message carries one period. */
#    define VIRTIO_SND_MSGS_MAX 8

/* Bytes of DMA-visible state at the head of a message: xfer + status + pad. */
#    define VIRTIO_SND_MSG_HEAD 16U

/* Event buffers kept posted on the event queue. */
#    define VIRTIO_SND_EVENTS_MAX 64

/* Cap on the streams array so one PCM_INFO reply still fits in a page. */
#    define VIRTIO_SND_STREAMS_MAX 127U

/* Control-message pacing: fast poll first, then a wait the ISR can cut short. */
#    define VIRTIO_SND_CTL_FAST_POLLS    256
#    define VIRTIO_SND_CTL_TIMEOUT_TICKS (5ULL * CONFIG_TIMER_HZ)
#    define VIRTIO_SND_CTL_BOOT_SPINS    10000000

/* Pump deadlines in scheduler ticks: short while a stream may be idle. */
#    define VIRTIO_SND_IDLE_TICKS   4
#    define VIRTIO_SND_SAFETY_TICKS CONFIG_TIMER_HZ

/* Bounded wait for a stopped stream to drain, in milliseconds. */
#    define VIRTIO_SND_DRAIN_TIMEOUT_MS 500U

/* Stream id marking a direction the device does not expose. */
#    define VIRTIO_SND_NO_STREAM (-1)

/* One DMA message: [xfer][status][pad][period payload] in a single block. */
struct virtsnd_msg {
        uint8_t *mem;
        uint64_t phys;
        size_t   pages;
        size_t   length; /* payload bytes staged or captured */
        bool     in_flight;
};

/* Message pool for one direction, protected by snd->lock. */
struct virtsnd_pool {
        struct virtsnd_msg  *msgs;
        struct virtsnd_msg **free;
        int                  count;
        int                  free_count;
        int                  inflight;
};

/* Capabilities of one device stream, as reported by VIRTIO_SND_R_PCM_INFO. */
struct virtsnd_stream {
        uint32_t features;
        uint64_t formats;
        uint64_t rates;
        uint8_t  direction;
        uint8_t  channels_min;
        uint8_t  channels_max;
};

struct virtio_snd {
        struct vp_device         vp;
        struct vp_virtqueue      vq[VIRTIO_SND_VQ_MAX];
        struct virtio_snd_config cfg;
        struct virtsnd_stream   *streams;
        int                      stream_id[2];

        audio_card_t       *card;
        audio_pcm_format_t  fmt[2];
        uint32_t            buffer_bytes[2];
        uint32_t            period_bytes[2];
        bool                params_valid[2];
        bool                running[2];
        struct virtsnd_pool pool[2];

        mutex_t      ops_lock; /* serialises start/stop/drain/set_params */
        mutex_t      ctl_lock; /* serialises control-message exchanges */
        spinlock_t   lock;     /* protects pools, running[] and params */
        wait_queue_t ctl_wait;
        wait_queue_t pump_wait;

        uint8_t *ctl_req;
        uint8_t *ctl_resp;

        struct virtio_snd_event *events;

        pci_irq_state_t irq_state;
        bool            irq_enabled;
        bool            worker_started;
        task_t         *worker;
};

/* The instance the interrupt handler is allowed to dereference. */
static struct virtio_snd *virtsnd_irq_device;

/* Byte size of one interleaved frame of this format. */
static size_t virtsnd_frame_bytes(const audio_pcm_format_t *fmt)
{
    return (size_t)(fmt->bits / 8) * fmt->channels;
}

/* Compare two formats field by field. */
static bool virtsnd_same_format(const audio_pcm_format_t *a, const audio_pcm_format_t *b)
{
    return a->sample_rate == b->sample_rate && a->bits == b->bits && a->channels == b->channels;
}

/*
 * The open file a direction is driven by, or NULL when nothing usable is
 * open.  card->pcm_lock must be held by the caller for the whole lookup and
 * the subsequent use of the returned file: audio_pcm_destroy() takes the same
 * lock before freeing, so the pointer cannot go away underneath us.
 */
static audio_pcm_file_t *virtsnd_file_locked(struct virtio_snd *snd, audio_node_type_t type)
{
    for (audio_pcm_file_t *pf = snd->card->pcm_files; pf; pf = pf->next) {
        if (pf->type == type && pf->state != SNDRV_PCM_STATE_OPEN) return pf;
    }
    return NULL;
}

/* True when a configured file exists for this direction. */
static bool virtsnd_direction_ready(struct virtio_snd *snd, audio_node_type_t type)
{
    uint64_t rflags = spin_lock_irqsave(&snd->card->pcm_lock);
    bool     ready  = virtsnd_file_locked(snd, type) != NULL;
    spin_unlock_irqrestore(&snd->card->pcm_lock, rflags);
    return ready;
}

/*
 * True when the file driving this direction has moved past OPEN and can
 * therefore block waiting for frames.  virtsnd_file_locked() already skips
 * OPEN, so this only has to separate SETUP - a file that has been configured
 * but is not waiting on anything - from PREPARED and later, where the core is
 * allowed to sleep.  Only those later states make a silent no-op fatal.
 */
static bool virtsnd_direction_pending(struct virtio_snd *snd, audio_node_type_t type)
{
    uint64_t          rflags  = spin_lock_irqsave(&snd->card->pcm_lock);
    audio_pcm_file_t *pf      = virtsnd_file_locked(snd, type);
    bool              pending = pf && pf->state >= SNDRV_PCM_STATE_PREPARED;
    spin_unlock_irqrestore(&snd->card->pcm_lock, rflags);
    return pending;
}

/* Allocate one zeroed page of physically contiguous DMA memory. */
static uint8_t *virtsnd_alloc_page(void)
{
    uint64_t phys = alloc_frames(1);
    if (!phys) return NULL;
    uint8_t *page = phys_to_virt(phys);
    memset(page, 0, PAGE_4K_SIZE);
    return page;
}

/* Give back a page obtained from virtsnd_alloc_page(). */
static void virtsnd_free_page(void *page)
{
    if (!page) return;
    free_frames((uint64_t)virt_to_phys((uint64_t)page), 1);
}

/* Drop every DMA block owned by a pool.  The caller guarantees no message is in flight. */
static void virtsnd_pool_release(struct virtsnd_pool *pool)
{
    for (int i = 0; i < pool->count; i++) {
        if (pool->msgs[i].mem) free_frames(pool->msgs[i].phys, pool->msgs[i].pages);
    }
    if (pool->msgs) free(pool->msgs);
    if (pool->free) free(pool->free);
    memset(pool, 0, sizeof(*pool));
}

/* Carve `count` messages of `period_bytes` payload out of fresh DMA blocks. */
static int virtsnd_pool_alloc(struct virtsnd_pool *pool, int count, uint32_t period_bytes)
{
    const size_t block = ALIGN_UP((size_t)VIRTIO_SND_MSG_HEAD + period_bytes, PAGE_4K_SIZE);
    const size_t pages = block / PAGE_4K_SIZE;

    memset(pool, 0, sizeof(*pool));
    pool->msgs = calloc((size_t)count, sizeof(*pool->msgs));
    pool->free = calloc((size_t)count, sizeof(struct virtsnd_msg *));
    if (!pool->msgs || !pool->free) {
        if (pool->msgs) free(pool->msgs);
        if (pool->free) free(pool->free);
        memset(pool, 0, sizeof(*pool));
        return -ENOMEM;
    }
    pool->count = count;

    for (int i = 0; i < count; i++) {
        uint64_t phys = alloc_frames(pages);
        if (!phys) {
            virtsnd_pool_release(pool);
            return -ENOMEM;
        }
        pool->msgs[i].phys             = phys;
        pool->msgs[i].pages            = pages;
        pool->msgs[i].mem              = phys_to_virt(phys);
        pool->msgs[i].in_flight        = false;
        pool->msgs[i].length           = 0;
        pool->free[pool->free_count++] = &pool->msgs[i];
        memset(pool->msgs[i].mem, 0, (size_t)VIRTIO_SND_MSG_HEAD + period_bytes);
    }
    return EOK;
}

/* Turn a device status word into an errno value. */
static int virtsnd_status_to_errno(uint32_t status)
{
    if (status == VIRTIO_SND_S_OK) return EOK;
    if (status == VIRTIO_SND_S_BAD_MSG) return -EINVAL;
    if (status == VIRTIO_SND_S_NOT_SUPP) return -EOPNOTSUPP;
    return -EIO;
}

/* Retire a queue after a control message never came back. */
static int virtsnd_mark_broken(struct vp_virtqueue *vq)
{
    spin_lock(&vq->lock);
    vq->broken = true;
    spin_unlock(&vq->lock);
    return -EIO;
}

/*
 * Send one control request and wait for its response.
 *
 * The request and response bytes live in DMA-safe staging pages owned by the
 * device instance, so callers are serialised by snd->ctl_lock.  `items`
 * optionally receives the trailing item array of `items_len` bytes that some
 * requests append after the four-byte response header.
 */
static int virtsnd_ctl(struct virtio_snd *snd, const void *req, size_t req_len, void *items, size_t items_len)
{
    struct vp_virtq_seg  segs[3];
    struct vp_virtqueue *vq    = &snd->vq[VIRTIO_SND_VQ_CONTROL];
    int                  count = 0;
    int                  ret   = EOK;
    uint32_t             len   = 0;
    void                *cookie;

    if (!req || req_len == 0 || req_len > PAGE_4K_SIZE) return -EINVAL;
    if (items_len + sizeof(struct virtio_snd_hdr) > PAGE_4K_SIZE) return -EINVAL;
    if (!snd->ctl_req || !snd->ctl_resp || vq->broken) return -ENODEV;

    mutex_lock(&snd->ctl_lock);
    memcpy(snd->ctl_req, req, req_len);
    memset(snd->ctl_resp, 0, sizeof(struct virtio_snd_hdr) + items_len);

    segs[count++] = (struct vp_virtq_seg) {.data = snd->ctl_req, .len = (uint32_t)req_len, .write = 0};
    segs[count++] = (struct vp_virtq_seg) {.data = snd->ctl_resp, .len = (uint32_t)sizeof(struct virtio_snd_hdr), .write = 1};
    if (items_len) segs[count++] = (struct vp_virtq_seg) {.data = snd->ctl_resp + sizeof(struct virtio_snd_hdr), .len = (uint32_t)items_len, .write = 1};

    ret = virtqueue_add_chain(vq, snd, segs, count);
    if (ret != EOK) goto out;
    virtqueue_kick(vq);

    {
        /* Boot probes cannot sleep, and a queue without interrupts has nobody to wait on. */
        const bool     interruptible = snd->irq_enabled && __atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE);
        const uint64_t deadline      = interruptible ? sched_ticks() + VIRTIO_SND_CTL_TIMEOUT_TICKS : 0;
        int            fast_polls    = 0;
        int            spins         = 0;

        for (;;) {
            cookie = virtqueue_get_buf(vq, &len);
            if (cookie) break;

            if (interruptible) {
                if (fast_polls++ < VIRTIO_SND_CTL_FAST_POLLS) {
                    cpu_relax();
                    compiler_barrier();
                    continue;
                }
                if (sched_ticks() >= deadline) {
                    ret = virtsnd_mark_broken(vq);
                    goto out;
                }

                wait_queue_prepare(&snd->ctl_wait);

                /* Close the used-ring/prepare race before committing the sleep. */
                cookie = virtqueue_get_buf(vq, &len);
                if (cookie) {
                    wait_queue_cancel(&snd->ctl_wait);
                    break;
                }
                (void)wait_queue_wait_timed(&snd->ctl_wait, deadline);
                continue;
            }

            /* No scheduler or no interrupt: bounded busy poll, as virtgpu does. */
            if (++spins > VIRTIO_SND_CTL_BOOT_SPINS) {
                ret = virtsnd_mark_broken(vq);
                goto out;
            }
            cpu_relax();
        }
    }

    {
        uint32_t status;
        memcpy(&status, snd->ctl_resp, sizeof(status));
        ret = virtsnd_status_to_errno(status);
        if (ret == EOK && items && items_len) memcpy(items, snd->ctl_resp + sizeof(struct virtio_snd_hdr), items_len);
    }

out:
    mutex_unlock(&snd->ctl_lock);
    return ret;
}

/* Send a control request that only carries a request code and a stream id. */
static int virtsnd_pcm_request(struct virtio_snd *snd, uint32_t code, uint32_t stream_id)
{
    struct virtio_snd_pcm_hdr req;

    memset(&req, 0, sizeof(req));
    req.hdr.code  = code;
    req.stream_id = stream_id;
    return virtsnd_ctl(snd, &req, sizeof(req), NULL, 0);
}

/* Push the stored buffer, period, format, rate and channel count to one stream. */
static int virtsnd_pcm_params(struct virtio_snd *snd, int d, uint32_t vformat, uint32_t vrate)
{
    struct virtio_snd_pcm_set_params req;

    memset(&req, 0, sizeof(req));
    req.hdr.hdr.code  = VIRTIO_SND_R_PCM_SET_PARAMS;
    req.hdr.stream_id = (uint32_t)snd->stream_id[d];
    req.buffer_bytes  = snd->buffer_bytes[d];
    req.period_bytes  = snd->period_bytes[d];
    req.features      = 0;
    req.channels      = snd->fmt[d].channels;
    req.format        = (uint8_t)vformat;
    req.rate          = (uint8_t)vrate;
    return virtsnd_ctl(snd, &req, sizeof(req), NULL, 0);
}

/*
 * Translate a core format into virtio-snd indexes and check the device streams
 * support it.  Pass d = -1 to require support on every exposed stream (the
 * core only reports the outcome through set_format, so a format that one
 * direction would reject has to be rejected there rather than silently
 * starting a stream that never produces data).
 */
static int virtsnd_check_format(const struct virtio_snd *snd, int d, const audio_pcm_format_t *fmt, uint32_t *vformat, uint32_t *vrate)
{
    /* Frame rate hertz, indexed so that i + VIRTIO_SND_PCM_RATE_8000 is the bit. */
    static const uint32_t rates_hz[] = {8000U, 11025U, 16000U, 22050U, 32000U, 44100U, 48000U, 64000U, 88200U, 96000U, 176400U, 192000U};
    const size_t          rate_count = sizeof(rates_hz) / sizeof(rates_hz[0]);
    uint32_t              format;
    uint32_t              rate      = 0;
    bool                  have_rate = false;
    int                   checked   = 0;

    if (fmt->bits == 8) {
        format = VIRTIO_SND_PCM_FMT_U8;
    } else if (fmt->bits == 16) {
        format = VIRTIO_SND_PCM_FMT_S16;
    } else {
        return -EINVAL;
    }

    for (size_t i = 0; i < rate_count; i++) {
        if (rates_hz[i] == fmt->sample_rate) {
            rate      = (uint32_t)i + (uint32_t)VIRTIO_SND_PCM_RATE_8000;
            have_rate = true;
            break;
        }
    }
    if (!have_rate) return -EINVAL;

    for (int i = 0; i < 2; i++) {
        if (d >= 0 && i != d) continue;
        if (snd->stream_id[i] == VIRTIO_SND_NO_STREAM) continue;

        const struct virtsnd_stream *s = &snd->streams[snd->stream_id[i]];
        if (!(s->formats & (1ULL << format))) return -EINVAL;
        if (!(s->rates & (1ULL << rate))) return -EINVAL;
        if (fmt->channels < s->channels_min || fmt->channels > s->channels_max) return -EINVAL;
        checked++;
    }
    if (checked == 0) return -EOPNOTSUPP;

    if (vformat) *vformat = format;
    if (vrate) *vrate = rate;
    return EOK;
}

/* Choose a device-side period: whole frames, and no more than half the ring. */
static uint32_t virtsnd_period_for(uint32_t buffer_bytes, uint32_t requested, uint32_t fb)
{
    uint32_t period = requested;

    if (fb == 0) fb = 1;
    if (period == 0 || period > buffer_bytes / 2U) period = buffer_bytes / 4U;
    period -= period % fb;
    if (period == 0) period = fb;
    return period;
}

/* Hand a staged playback message to the TX queue. */
static int virtsnd_submit_tx(struct virtio_snd *snd, struct virtsnd_msg *msg, size_t payload, uint32_t stream_id)
{
    struct virtio_snd_pcm_xfer   *xfer   = (struct virtio_snd_pcm_xfer *)msg->mem;
    struct virtio_snd_pcm_status *status = (struct virtio_snd_pcm_status *)(msg->mem + sizeof(*xfer));
    struct vp_virtq_seg           segs[3];

    xfer->stream_id = stream_id;
    segs[0]         = (struct vp_virtq_seg) {.data = xfer, .len = (uint32_t)sizeof(*xfer), .write = 0};
    segs[1]         = (struct vp_virtq_seg) {.data = msg->mem + VIRTIO_SND_MSG_HEAD, .len = (uint32_t)payload, .write = 0};
    segs[2]         = (struct vp_virtq_seg) {.data = status, .len = (uint32_t)sizeof(*status), .write = 1};
    return virtqueue_add_chain(&snd->vq[VIRTIO_SND_VQ_TX], msg, segs, 3);
}

/* Post an empty capture message to the RX queue. */
static int virtsnd_submit_rx(struct virtio_snd *snd, struct virtsnd_msg *msg, size_t payload, uint32_t stream_id)
{
    struct virtio_snd_pcm_xfer   *xfer   = (struct virtio_snd_pcm_xfer *)msg->mem;
    struct virtio_snd_pcm_status *status = (struct virtio_snd_pcm_status *)(msg->mem + sizeof(*xfer));
    struct vp_virtq_seg           segs[3];

    /*
     * The in side is data first, status second: the device lays the two out
     * end to end over the write-only descriptors (QEMU writes the samples at
     * in offset 0 and the status right behind them), and Linux orders the
     * scatterlist the same way.  A status descriptor in front would leave the
     * samples shifted by eight bytes.
     */
    xfer->stream_id = stream_id;
    segs[0]         = (struct vp_virtq_seg) {.data = xfer, .len = (uint32_t)sizeof(*xfer), .write = 0};
    segs[1]         = (struct vp_virtq_seg) {.data = msg->mem + VIRTIO_SND_MSG_HEAD, .len = (uint32_t)payload, .write = 1};
    segs[2]         = (struct vp_virtq_seg) {.data = status, .len = (uint32_t)sizeof(*status), .write = 1};
    return virtqueue_add_chain(&snd->vq[VIRTIO_SND_VQ_RX], msg, segs, 3);
}

/* Return a message to its pool after the device handed it back. */
static void virtsnd_pool_recycle(struct virtsnd_pool *pool, struct virtsnd_msg *msg)
{
    if (!msg->in_flight) return;
    msg->in_flight = false;
    pool->inflight--;
    pool->free[pool->free_count++] = msg;
}

/* Drain completed event notifications and re-post their buffers. */
static void virtsnd_reap_events(struct virtio_snd *snd)
{
    struct vp_virtqueue *vq = &snd->vq[VIRTIO_SND_VQ_EVENT];
    void                *cookie;
    bool                 reposted = false;

    while ((cookie = virtqueue_get_buf(vq, NULL)) != NULL) {
        struct virtio_snd_event *event = cookie;
        if (virtqueue_add(vq, event, sizeof(*event), 1) == EOK) reposted = true;
    }
    if (reposted) virtqueue_kick(vq);
}

/* Retire completed playback messages: the frames left the ring at submit time. */
static void virtsnd_reap_tx(struct virtio_snd *snd)
{
    struct vp_virtqueue *vq = &snd->vq[VIRTIO_SND_VQ_TX];
    void                *cookie;

    /*
     * pcm_ring_buffer_read_frames() already moved hw_ptr when the message was
     * staged, so there is nothing to account for here beyond handing the DMA
     * block back: re-advancing hw_ptr would count every period twice, and it
     * would corrupt the ring outright if the application re-prepared while a
     * message was still in flight (the core resets both pointers without
     * telling the driver).
     */
    while ((cookie = virtqueue_get_buf(vq, NULL)) != NULL) {
        struct virtsnd_msg *msg = cookie;

        spin_lock(&snd->lock);
        virtsnd_pool_recycle(&snd->pool[VIRTIO_SND_D_OUTPUT], msg);
        spin_unlock(&snd->lock);
    }
}

/* Drain completed capture messages: push the samples into the ring. */
static void virtsnd_reap_rx(struct virtio_snd *snd)
{
    struct vp_virtqueue *vq = &snd->vq[VIRTIO_SND_VQ_RX];
    void                *cookie;
    uint32_t             len;

    while ((cookie = virtqueue_get_buf(vq, &len)) != NULL) {
        struct virtsnd_msg *msg        = cookie;
        const size_t        status_len = sizeof(struct virtio_snd_pcm_status);
        const size_t        payload    = (len > status_len) ? len - status_len : 0;
        size_t              captured   = (payload < msg->length) ? payload : msg->length;

        /*
         * Copy out before recycling: the message goes back on the free list,
         * and the pump may refill its payload the moment it does.
         */
        if (captured) {
            uint64_t          rflags = spin_lock_irqsave(&snd->card->pcm_lock);
            audio_pcm_file_t *pf     = virtsnd_file_locked(snd, audio_node_pcm_capture);
            if (pf) {
                const size_t fb     = virtsnd_frame_bytes(&pf->fmt);
                size_t       frames = fb ? captured / fb : 0;

                spin_lock(&pf->lock);
                if (frames) frames = pcm_ring_buffer_write_frames(pf, msg->mem + VIRTIO_SND_MSG_HEAD, frames);
                spin_unlock(&pf->lock);
                if (frames) wait_queue_wake_all(&pf->read_wait);
            }
            spin_unlock_irqrestore(&snd->card->pcm_lock, rflags);
        }

        spin_lock(&snd->lock);
        virtsnd_pool_recycle(&snd->pool[VIRTIO_SND_D_INPUT], msg);
        spin_unlock(&snd->lock);
    }
}

/* Stage queued playback frames and hand them to the device. */
static void virtsnd_fill_tx(struct virtio_snd *snd)
{
    const uint32_t stream_id = (uint32_t)snd->stream_id[VIRTIO_SND_D_OUTPUT];
    int            submitted = 0;

    /*
     * card->pcm_lock stays held for the whole lookup-and-copy so the file
     * cannot be destroyed while we read its ring.  Lock order everywhere in
     * this driver is card->pcm_lock -> snd->lock -> pf->lock.
     */
    uint64_t          rflags = spin_lock_irqsave(&snd->card->pcm_lock);
    audio_pcm_file_t *pf     = virtsnd_file_locked(snd, audio_node_pcm_playback);

    if (pf && virtsnd_same_format(&pf->fmt, &snd->fmt[VIRTIO_SND_D_OUTPUT])) {
        spin_lock(&snd->lock);
        /*
         * Re-test under snd->lock, the lock stop_direction() clears running
         * with: a fill that slipped past the pump's own check then blocks
         * here until the drain below has finished, instead of racing new
         * messages in after it.
         */
        if (snd->running[VIRTIO_SND_D_OUTPUT]) {
            struct virtsnd_pool *pool   = &snd->pool[VIRTIO_SND_D_OUTPUT];
            const size_t         fb     = virtsnd_frame_bytes(&pf->fmt);
            const uint32_t       period = fb ? snd->period_bytes[VIRTIO_SND_D_OUTPUT] / (uint32_t)fb : 0;

            while (period && pool->free_count) {
                struct virtsnd_msg *msg = pool->free[pool->free_count - 1];
                size_t              frames;

                spin_lock(&pf->lock);
                if (pcm_ring_buffer_avail(pf) < (snd_pcm_sframes_t)period) {
                    spin_unlock(&pf->lock);
                    break;
                }
                frames = pcm_ring_buffer_read_frames(pf, msg->mem + VIRTIO_SND_MSG_HEAD, period);
                spin_unlock(&pf->lock);
                if (frames == 0) break;

                pool->free_count--;
                pool->inflight++;
                msg->in_flight = true;
                msg->length    = frames * fb;

                if (virtsnd_submit_tx(snd, msg, msg->length, stream_id) != EOK) {
                    virtsnd_pool_recycle(pool, msg);
                    break;
                }
                submitted++;
            }
        }
        spin_unlock(&snd->lock);
        /*
         * read_frames() moved hw_ptr, so space opened up in the ring: wake
         * anyone blocked in write().  pf is still pinned by pcm_lock here,
         * and the reaper never touches the ring any more.
         */
        if (submitted) wait_queue_wake_all(&pf->write_wait);
    }
    spin_unlock_irqrestore(&snd->card->pcm_lock, rflags);

    if (submitted) virtqueue_kick(&snd->vq[VIRTIO_SND_VQ_TX]);
}

/* Post empty capture messages wherever the ring still has room. */
static void virtsnd_fill_rx(struct virtio_snd *snd)
{
    const uint32_t stream_id = (uint32_t)snd->stream_id[VIRTIO_SND_D_INPUT];
    int            submitted = 0;

    uint64_t          rflags = spin_lock_irqsave(&snd->card->pcm_lock);
    audio_pcm_file_t *pf     = virtsnd_file_locked(snd, audio_node_pcm_capture);

    if (pf && virtsnd_same_format(&pf->fmt, &snd->fmt[VIRTIO_SND_D_INPUT])) {
        spin_lock(&snd->lock);
        /* See virtsnd_fill_tx(): the re-test closes the same stop/drain race. */
        if (snd->running[VIRTIO_SND_D_INPUT]) {
            struct virtsnd_pool *pool   = &snd->pool[VIRTIO_SND_D_INPUT];
            const size_t         fb     = virtsnd_frame_bytes(&pf->fmt);
            const uint32_t       period = fb ? snd->period_bytes[VIRTIO_SND_D_INPUT] / (uint32_t)fb : 0;

            while (period && pool->free_count) {
                struct virtsnd_msg *msg = pool->free[pool->free_count - 1];

                spin_lock(&pf->lock);
                const bool room = pcm_ring_buffer_space(pf) >= (snd_pcm_sframes_t)period;
                spin_unlock(&pf->lock);
                if (!room) break;

                pool->free_count--;
                pool->inflight++;
                msg->in_flight = true;
                msg->length    = (size_t)period * fb;

                if (virtsnd_submit_rx(snd, msg, msg->length, stream_id) != EOK) {
                    virtsnd_pool_recycle(pool, msg);
                    break;
                }
                submitted++;
            }
        }
        spin_unlock(&snd->lock);
    }
    spin_unlock_irqrestore(&snd->card->pcm_lock, rflags);

    if (submitted) virtqueue_kick(&snd->vq[VIRTIO_SND_VQ_RX]);
}

/* Reap everything the device has finished and top the queues back up. */
static void virtsnd_pump(struct virtio_snd *snd)
{
    if (!snd->card) return; /* Set before the pump can ever be scheduled. */

    virtsnd_reap_events(snd);
    virtsnd_reap_tx(snd);
    virtsnd_reap_rx(snd);

    for (int d = 0; d < 2; d++) {
        spin_lock(&snd->lock);
        const bool running = snd->running[d];
        spin_unlock(&snd->lock);
        if (!running) continue;

        if (d == VIRTIO_SND_D_OUTPUT) {
            virtsnd_fill_tx(snd);
        } else {
            virtsnd_fill_rx(snd);
        }
    }
}

/* Block until a direction owns no in-flight message, or the deadline passes. */
static void virtsnd_wait_inflight(struct virtio_snd *snd, int d)
{
    const uint64_t deadline = sched_ticks() + ((uint64_t)VIRTIO_SND_DRAIN_TIMEOUT_MS * CONFIG_TIMER_HZ) / 1000U;

    for (;;) {
        spin_lock(&snd->lock);
        const int inflight = snd->pool[d].inflight;
        spin_unlock(&snd->lock);
        if (inflight == 0) return;

        if (sched_ticks() >= deadline) {
            plogk("virtio_snd: stream drain timed out with %d message(s) in flight\n", inflight);
            return;
        }

        virtsnd_pump(snd);
        (void)wait_queue_wake_all(&snd->pump_wait);
        usleep(500);
    }
}

/*
 * Take one direction down: mark it stopped, then walk the device through
 * STOP and RELEASE.  RELEASE makes the device complete every message it
 * still holds, which is what the drain below then waits for.
 */
static void virtsnd_stop_direction(struct virtio_snd *snd, int d)
{
    spin_lock(&snd->lock);
    const bool was_running = snd->running[d];
    snd->running[d]        = false;
    spin_unlock(&snd->lock);
    if (!was_running) return;

    if (snd->stream_id[d] != VIRTIO_SND_NO_STREAM) {
        const uint32_t id = (uint32_t)snd->stream_id[d];
        (void)virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_STOP, id);
        (void)virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_RELEASE, id);
    }
    virtsnd_wait_inflight(snd, d);
}

/*
 * Pump worker.  Reaps completions, refills the queues and then sleeps: the
 * interrupt wakes it as soon as the device is done with a message, so a short
 * deadline is only needed while a running stream has gone completely idle and
 * the application may stage new data at any moment.  Without a working
 * interrupt the deadline stays short and the pump degrades to a timer.
 */
static int virtsnd_worker(void *arg)
{
    struct virtio_snd *snd = arg;

    while (!kthread_should_stop()) {
        virtsnd_pump(snd);

        spin_lock(&snd->lock);
        const bool running  = snd->running[VIRTIO_SND_D_OUTPUT] || snd->running[VIRTIO_SND_D_INPUT];
        const int  inflight = snd->pool[VIRTIO_SND_D_OUTPUT].inflight + snd->pool[VIRTIO_SND_D_INPUT].inflight;
        spin_unlock(&snd->lock);

        const bool     idle_stream = running && inflight == 0;
        const uint64_t deadline    = (idle_stream || !snd->irq_enabled) ? sched_ticks() + VIRTIO_SND_IDLE_TICKS : sched_ticks() + VIRTIO_SND_SAFETY_TICKS;

        wait_queue_prepare(&snd->pump_wait);

        /* Recheck the used rings before committing to the sleep. */
        if (virtqueue_enable_cb(&snd->vq[VIRTIO_SND_VQ_TX]) || virtqueue_enable_cb(&snd->vq[VIRTIO_SND_VQ_RX]) || virtqueue_enable_cb(&snd->vq[VIRTIO_SND_VQ_EVENT])) {
            wait_queue_cancel(&snd->pump_wait);
            continue;
        }
        (void)wait_queue_wait_timed(&snd->pump_wait, deadline);
    }
    return EOK;
}

/* audio callback: validate a format against every stream the device exposes. */
static int virtsnd_set_format(audio_card_t *card, const audio_pcm_format_t *format)
{
    struct virtio_snd *snd = card->driver_data;
    int                ret;

    if (!snd || !format) return -EINVAL;

    ret = virtsnd_check_format(snd, VIRTIO_SND_NO_STREAM, format, NULL, NULL);
    if (ret != EOK) return ret;

    card->format = *format;
    return EOK;
}

/* audio callback: adopt buffer/period/format for the direction being set up. */
static int virtsnd_set_params(audio_card_t *card, const audio_pcm_format_t *fmt, size_t buffer_bytes, size_t period_bytes)
{
    struct virtio_snd *snd = card->driver_data;
    int                ret = -EINVAL;
    int                d;

    if (!snd || !fmt || buffer_bytes == 0 || buffer_bytes > UINT32_MAX) return -EINVAL;

    /*
     * The core reports parameters card-wide, but the values come straight
     * from one file's hw_params(), so match them against the open files to
     * recover the direction.  That keeps a reconfiguration of one stream from
     * tearing down the other, and lets each direction keep its own period.
     */
    d               = VIRTIO_SND_NO_STREAM;
    uint64_t rflags = spin_lock_irqsave(&snd->card->pcm_lock);
    for (audio_pcm_file_t *pf = snd->card->pcm_files; pf; pf = pf->next) {
        if (pf->state == SNDRV_PCM_STATE_OPEN) continue;
        if (!virtsnd_same_format(&pf->fmt, fmt)) continue;
        if (pf->ring_buf_size != buffer_bytes || pf->period_bytes != period_bytes) continue;
        d = (pf->type == audio_node_pcm_capture) ? VIRTIO_SND_D_INPUT : VIRTIO_SND_D_OUTPUT;
        break;
    }
    spin_unlock_irqrestore(&snd->card->pcm_lock, rflags);

    mutex_lock(&snd->ops_lock);

    const size_t   fb     = virtsnd_frame_bytes(fmt);
    const uint32_t period = virtsnd_period_for((uint32_t)buffer_bytes, (uint32_t)period_bytes, (uint32_t)fb);

    /* Reconfiguration is always safe on an unknown direction: apply it to both. */
    const int first = (d == VIRTIO_SND_NO_STREAM) ? 0 : d;
    const int last  = (d == VIRTIO_SND_NO_STREAM) ? 1 : d;

    for (int i = first; i <= last; i++) {
        /* Only the direction being reconfigured stops; the other keeps running. */
        virtsnd_stop_direction(snd, i);

        int count = (int)((uint32_t)buffer_bytes / period);
        if (count < 2) count = 2;
        if (count > VIRTIO_SND_MSGS_MAX) count = VIRTIO_SND_MSGS_MAX;

        spin_lock(&snd->lock);
        if (snd->pool[i].inflight) {
            spin_unlock(&snd->lock);
            ret = -EBUSY;
            continue;
        }

        virtsnd_pool_release(&snd->pool[i]);
        ret = virtsnd_pool_alloc(&snd->pool[i], count, period);
        if (ret == EOK) {
            snd->buffer_bytes[i] = (uint32_t)buffer_bytes;
            snd->period_bytes[i] = period;
            snd->fmt[i]          = *fmt;
            snd->params_valid[i] = true;
        } else {
            virtsnd_pool_release(&snd->pool[i]);
            snd->params_valid[i] = false;
            plogk("virtio_snd: failed to allocate %d message(s) of %u bytes: %d\n", count, period, ret);
        }
        spin_unlock(&snd->lock);
    }

    mutex_unlock(&snd->ops_lock);
    return ret;
}

/* audio callback: arm every ready direction on the device. */
static int virtsnd_start(audio_card_t *card)
{
    struct virtio_snd *snd = card->driver_data;
    int                ret = EOK;

    if (!snd) return -ENODEV;

    mutex_lock(&snd->ops_lock);
    if (!snd->worker_started) {
        ret = -EIO;
        goto done;
    }

    for (int d = 0; d < 2; d++) {
        const uint32_t          id      = (uint32_t)snd->stream_id[d];
        const audio_node_type_t type    = (d == VIRTIO_SND_D_OUTPUT) ? audio_node_pcm_playback : audio_node_pcm_capture;
        uint32_t                vformat = 0;
        uint32_t                vrate   = 0;

        if (snd->stream_id[d] == VIRTIO_SND_NO_STREAM) continue;
        if (!virtsnd_direction_ready(snd, type)) continue;

        spin_lock(&snd->lock);
        const bool started = snd->running[d];
        spin_unlock(&snd->lock);
        if (started) continue;

        if (!snd->params_valid[d]) {
            /*
             * SET_PARAMS never reached the device for a file that is already
             * allowed to block.  Skipping silently reports EOK, the core then
             * publishes RUNNING on that strength, and nothing is left to feed
             * the ring: read() sleeps forever without a single line of output.
             * A file still in SETUP cannot wait yet, so leaving it alone keeps
             * an unconfigured capture node from failing someone else's start.
             */
            if (virtsnd_direction_pending(snd, type)) {
                plogk("virtio_snd: %s file needs the stream but stream %u has no parameters\n", d == VIRTIO_SND_D_OUTPUT ? "playback" : "capture", id);
                ret = -EBADFD;
                break;
            }
            continue;
        }

        /*
         * Device sequence: SET_PARAMS -> PREPARE -> START, and running[] is
         * raised only once START was accepted.
         *
         * The device completes TX buffers queued on a stopped stream without
         * playing them, which would silently drop the first period, and it
         * rejects STOP for a stream that was never started (a protocol error
         * that takes the whole device down).  So arming the pump early loses
         * audio on the happy path and poisons the state machine on a failed
         * start; doing it last costs only the microseconds between START and
         * the first fill.
         */
        ret = virtsnd_check_format(snd, d, &snd->fmt[d], &vformat, &vrate);
        if (ret == EOK) ret = virtsnd_pcm_params(snd, d, vformat, vrate);
        if (ret == EOK) ret = virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_PREPARE, id);
        if (ret == EOK) ret = virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_START, id);

        if (ret != EOK) {
            /* Nothing was armed, so there is no buffer queued to unwind. */
            plogk("virtio_snd: failed to start %s stream %u: %d\n", d == VIRTIO_SND_D_OUTPUT ? "playback" : "capture", id, ret);
            break;
        }

        spin_lock(&snd->lock);
        snd->running[d] = true;
        spin_unlock(&snd->lock);

        /* First capture buffers go out now; the device picks them up on its next tick. */
        if (d == VIRTIO_SND_D_INPUT) virtsnd_fill_rx(snd);
    }

    (void)wait_queue_wake_all(&snd->pump_wait);
done:
    mutex_unlock(&snd->ops_lock);
    return ret;
}

/* audio callback: stop both directions and wait for the device to let go. */
static int virtsnd_stop(audio_card_t *card)
{
    struct virtio_snd *snd = card->driver_data;

    if (!snd) return -ENODEV;

    mutex_lock(&snd->ops_lock);
    virtsnd_stop_direction(snd, VIRTIO_SND_D_OUTPUT);
    virtsnd_stop_direction(snd, VIRTIO_SND_D_INPUT);
    (void)wait_queue_wake_all(&snd->pump_wait);
    mutex_unlock(&snd->ops_lock);
    return EOK;
}

/* audio callback: wait for staged playback to drain, then stop the stream. */
static int virtsnd_drain(audio_card_t *card)
{
    struct virtio_snd *snd = card->driver_data;

    if (!snd) return -ENODEV;

    mutex_lock(&snd->ops_lock);
    virtsnd_wait_inflight(snd, VIRTIO_SND_D_OUTPUT);
    virtsnd_stop_direction(snd, VIRTIO_SND_D_OUTPUT);
    (void)wait_queue_wake_all(&snd->pump_wait);
    mutex_unlock(&snd->ops_lock);
    return EOK;
}

static const audio_card_ops_t virtsnd_audio_ops = {
    .set_format = virtsnd_set_format,
    .start      = virtsnd_start,
    .stop       = virtsnd_stop,
    .drain      = virtsnd_drain,
    .set_params = virtsnd_set_params,
};

/* Read the ISR status to clear the line and wake everything waiting on it. */
INTERRUPT_BEGIN static void virtsnd_irq_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    struct virtio_snd *snd = __atomic_load_n(&virtsnd_irq_device, __ATOMIC_ACQUIRE);
    if (snd) {
        if (snd->vp.isr) (void)*snd->vp.isr;
        (void)wait_queue_wake_all(&snd->ctl_wait);
        (void)wait_queue_wake_all(&snd->pump_wait);
    }
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Program the virtio MSI-X queue/config vector registers after MSI-X is enabled. */
static int virtsnd_msix_prepare(pci_device_cache_t *pci_dev, void *context)
{
    struct virtio_snd *snd = context;
    struct vp_device  *vp  = &snd->vp;

    (void)pci_dev;
    if (!vp->common) return -ENODEV;

    for (int i = 0; i < VIRTIO_SND_VQ_MAX; i++) {
        vp->common->queue_select      = (uint16_t)i;
        vp->common->queue_msix_vector = 0;
        if (vp->common->queue_msix_vector == UINT16_MAX) return -ENODEV;
    }
    vp->common->msix_config = 0;
    if (vp->common->msix_config == UINT16_MAX) return -ENODEV;
    return 0;
}

/* Install one shared completion vector for the four virtqueues. */
static void virtsnd_irq_init(struct virtio_snd *snd)
{
    if (!snd->vp.pci_dev || !snd->vp.common) return;

    pci_irq_request_t request = {
        .modes        = PCI_IRQ_MSI | PCI_IRQ_MSIX,
        .idt_handler  = (void *)virtsnd_irq_handler,
        .msix_setup   = virtsnd_msix_prepare,
        .msix_context = snd,
    };
    if (pci_request_irq(snd->vp.pci_dev, &request, &snd->irq_state) < 0) {
        plogk("virtio_snd: interrupt setup failed, falling back to timed polling\n");
        return;
    }

    __atomic_store_n(&virtsnd_irq_device, snd, __ATOMIC_RELEASE);
    snd->irq_enabled = true;
}

/* Set up the four device virtqueues plus their staging buffers. */
static int virtsnd_vq_init(struct virtio_snd *snd)
{
    int setup = 0;
    int ret   = EOK;

    for (int i = 0; i < VIRTIO_SND_VQ_MAX; i++) {
        ret = vp_setup_vq(&snd->vp, i, (int)VIRTIO_SND_VQ_NUM, &snd->vq[i]);
        if (ret != EOK) {
            plogk("virtio_snd: failed to set up virtqueue %d: %d\n", i, ret);
            break;
        }
        setup++;
    }
    if (setup != VIRTIO_SND_VQ_MAX) goto fail;

    /* Control staging pages are reused for every exchange, under snd->ctl_lock. */
    snd->ctl_req  = virtsnd_alloc_page();
    snd->ctl_resp = virtsnd_alloc_page();
    if (!snd->ctl_req || !snd->ctl_resp) {
        ret = -ENOMEM;
        goto fail;
    }

    /* Pre-post event buffers so the device never blocks trying to notify us. */
    {
        struct vp_virtqueue *vq    = &snd->vq[VIRTIO_SND_VQ_EVENT];
        int                  count = vq->num_max < VIRTIO_SND_EVENTS_MAX ? vq->num_max : VIRTIO_SND_EVENTS_MAX;

        if ((size_t)count * sizeof(struct virtio_snd_event) > PAGE_4K_SIZE) count = (int)(PAGE_4K_SIZE / sizeof(struct virtio_snd_event));
        if (count > 0) {
            snd->events = (struct virtio_snd_event *)virtsnd_alloc_page();
            if (!snd->events) {
                ret = -ENOMEM;
                goto fail;
            }
            for (int i = 0; i < count; i++) {
                if (virtqueue_add(vq, &snd->events[i], sizeof(struct virtio_snd_event), 1) != EOK) break;
            }
            virtqueue_kick(vq);
        }
    }

    return EOK;

fail:
    if (snd->events) {
        virtsnd_free_page(snd->events);
        snd->events = NULL;
    }
    virtsnd_free_page(snd->ctl_req);
    virtsnd_free_page(snd->ctl_resp);
    snd->ctl_req  = NULL;
    snd->ctl_resp = NULL;
    for (int i = 0; i < setup; i++) vp_del_vq(&snd->vq[i]);
    return ret;
}

/* Ask the device for every PCM stream and pick one per direction. */
static int virtsnd_query_streams(struct virtio_snd *snd)
{
    const size_t                 count = snd->cfg.streams;
    struct virtio_snd_query_info req;
    struct virtio_snd_pcm_info  *info;
    int                          ret;

    snd->streams = calloc(count, sizeof(*snd->streams));
    if (!snd->streams) return -ENOMEM;

    info = calloc(count, sizeof(*info));
    if (!info) return -ENOMEM;

    memset(&req, 0, sizeof(req));
    req.hdr.code = VIRTIO_SND_R_PCM_INFO;
    req.start_id = 0;
    req.count    = (uint32_t)count;
    req.size     = (uint32_t)sizeof(struct virtio_snd_pcm_info);

    ret = virtsnd_ctl(snd, &req, sizeof(req), info, count * sizeof(*info));
    if (ret == EOK) {
        for (size_t i = 0; i < count; i++) {
            snd->streams[i].features     = info[i].features;
            snd->streams[i].formats      = info[i].formats;
            snd->streams[i].rates        = info[i].rates;
            snd->streams[i].direction    = info[i].direction;
            snd->streams[i].channels_min = info[i].channels_min;
            snd->streams[i].channels_max = info[i].channels_max;

            if (info[i].direction == VIRTIO_SND_D_OUTPUT && snd->stream_id[VIRTIO_SND_D_OUTPUT] == VIRTIO_SND_NO_STREAM) {
                snd->stream_id[VIRTIO_SND_D_OUTPUT] = (int)i;
            } else if (info[i].direction == VIRTIO_SND_D_INPUT && snd->stream_id[VIRTIO_SND_D_INPUT] == VIRTIO_SND_NO_STREAM) {
                snd->stream_id[VIRTIO_SND_D_INPUT] = (int)i;
            }
        }
    }

    free(info);
    return ret;
}

/* Give back everything held before the sound card was published. */
static void virtsnd_cleanup(struct virtio_snd *snd)
{
    virtsnd_pool_release(&snd->pool[VIRTIO_SND_D_OUTPUT]);
    virtsnd_pool_release(&snd->pool[VIRTIO_SND_D_INPUT]);
    virtsnd_free_page(snd->ctl_req);
    virtsnd_free_page(snd->ctl_resp);
    virtsnd_free_page(snd->events);
    snd->ctl_req  = NULL;
    snd->ctl_resp = NULL;
    snd->events   = NULL;
    for (int i = 0; i < VIRTIO_SND_VQ_MAX; i++) {
        if (snd->vq[i].desc) vp_del_vq(&snd->vq[i]);
    }
    if (snd->streams) free(snd->streams);
    snd->streams = NULL;
    vp_release_device(&snd->vp);
}

/* Probe a virtio-snd device, publish a card for it and start its pump. */
void virtio_snd_init(void)
{
    struct virtio_snd *snd;
    audio_pcm_format_t fmt;
    uint64_t           features = 0;
    int                ret      = EOK;
    int                card_id;

    snd = malloc(sizeof(*snd));
    if (!snd) return;
    memset(snd, 0, sizeof(*snd));

    snd->stream_id[VIRTIO_SND_D_OUTPUT] = VIRTIO_SND_NO_STREAM;
    snd->stream_id[VIRTIO_SND_D_INPUT]  = VIRTIO_SND_NO_STREAM;
    snd->lock.lock                      = 0;
    snd->lock.rflags                    = 0;
    mutex_init(&snd->ops_lock);
    mutex_init(&snd->ctl_lock);
    wait_queue_init(&snd->ctl_wait);
    wait_queue_init(&snd->pump_wait);

    ret = vp_find_device(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_VIRTIO_SOUND, &snd->vp);
    if (ret != EOK) ret = vp_find_device(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_VIRTIO_SOUND_LEGACY, &snd->vp);
    if (ret != EOK) {
        free(snd);
        return; /* No virtio-snd device: probe quietly, like hda_init(). */
    }

    /*
     * The virtqueues are only usable once the device can perform DMA, so
     * enable memory decoding and bus mastering before any ring is programmed.
     */
    pci_enable_device(snd->vp.pci_dev, PCI_CMD_MEM | PCI_CMD_BUSMASTER);

    vp_setup_device(&snd->vp);

    /* virtio-snd only exists on a version-1 device (specification 5.14.6). */
    ret = vp_negotiate_features(&snd->vp, (1ULL << VIRTIO_F_VERSION_1), &features);
    if (ret != EOK || !(features & (1ULL << VIRTIO_F_VERSION_1))) {
        plogk("virtio_snd: device does not offer VIRTIO_F_VERSION_1 (features 0x%016llx)\n", features);
        goto fail_device;
    }

    /* FEATURES_OK has to be set and read back before the queues come up. */
    vp_set_status(&snd->vp, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    compiler_barrier();
    if (!(vp_get_status(&snd->vp) & VIRTIO_STATUS_FEATURES_OK)) {
        plogk("virtio_snd: device rejected feature negotiation (status 0x%02x)\n", vp_get_status(&snd->vp));
        goto fail_device;
    }

    ret = virtsnd_vq_init(snd);
    if (ret != EOK) goto fail_device;

    /* DRIVER_OK last: the device starts using the queues from here on. */
    compiler_barrier();
    vp_set_status(&snd->vp, vp_get_status(&snd->vp) | VIRTIO_STATUS_DRIVER_OK);
    compiler_barrier();

    vp_read_device_config(&snd->vp, &snd->cfg, 0, sizeof(snd->cfg));
    if (snd->cfg.streams == 0 || snd->cfg.streams > VIRTIO_SND_STREAMS_MAX) {
        plogk("virtio_snd: unsupported stream count %u\n", snd->cfg.streams);
        goto fail_queues;
    }

    ret = virtsnd_query_streams(snd);
    if (ret != EOK) {
        plogk("virtio_snd: VIRTIO_SND_R_PCM_INFO failed: %d\n", ret);
        goto fail_queues;
    }
    if (snd->stream_id[VIRTIO_SND_D_OUTPUT] == VIRTIO_SND_NO_STREAM && snd->stream_id[VIRTIO_SND_D_INPUT] == VIRTIO_SND_NO_STREAM) {
        plogk("virtio_snd: device exposes no playback or capture stream\n");
        goto fail_queues;
    }

    fmt.sample_rate = 48000;
    fmt.bits        = 16;
    fmt.channels    = 2;

    card_id = audio_register_card("VirtIO Sound", &fmt, &virtsnd_audio_ops, snd);
    if (card_id < 0) goto fail_queues;
    snd->card = audio_get_card((uint32_t)card_id);
    if (!snd->card) {
        /*
         * Impossible: the id was just handed out.  Keep the device alive with
         * the data path disabled rather than freeing memory the registered
         * card still points at.
         */
        plogk("virtio_snd: card %d vanished right after registration\n", card_id);
        return;
    }

    ret = kernel_worker_register("virtio-snd-pump", virtsnd_worker, snd, &snd->worker);
    if (ret == EOK) {
        snd->worker_started = true;
    } else {
        /* The card exists but nothing would pump it: refuse I/O instead of stalling. */
        plogk("virtio_snd: pump worker not registered (%d), PCM data path disabled\n", ret);
    }

    virtsnd_irq_init(snd); /* Best effort: the pump polls on a short deadline without it. */

    plogk("virtio_snd: %u stream(s), playback id %d, capture id %d, %s interrupts\n", snd->cfg.streams, snd->stream_id[VIRTIO_SND_D_OUTPUT], snd->stream_id[VIRTIO_SND_D_INPUT],
          snd->irq_enabled ? "MSI-X" : "timed");
    return;

fail_queues:
    virtsnd_cleanup(snd);
    free(snd);
    return;

fail_device:
    vp_release_device(&snd->vp);
    free(snd);
}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_VIRTIO_SND && CONFIG_VIRTIO_PCI
