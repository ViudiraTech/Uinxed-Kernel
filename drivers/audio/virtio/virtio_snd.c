/*
 *
 *      virtio_snd.c
 *      VirtIO sound (virtio-snd) driver
 *
 *      2026/10/04 By Yinyuan34513
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <drivers/audio/ulsa/ulsa.h>
#include <drivers/audio/virtio/virtio_snd.h>
#include <drivers/bus/virtpci.h>
#include <drivers/firmware/apic.h>
#include <kernel/debug/printk.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <process/kthread.h>
#include <process/sched.h>
#include <sync/mutex.h>

#if CONFIG_VIRTIO && CONFIG_AUDIO && CONFIG_AUDIO_VIRTIO_SND && CONFIG_VIRTIO_PCI

/* Descriptors asked of each virtqueue (the device may hand back fewer). */
#    define VIRTIO_SND_VQ_NUM 64U

/* Cap concurrent I/O messages per direction: 16 * 3 segments = 48 <= 64 descriptors. */
#    define VIRTIO_SND_MSGS_MAX 16

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

/* Pump deadlines in scheduler ticks: short while a stream may be idle or unmasked. */
#    define VIRTIO_SND_IDLE_TICKS   4
#    define VIRTIO_SND_SAFETY_TICKS CONFIG_TIMER_HZ

/* Bounded wait for a stopped stream to drain, in milliseconds. */
#    define VIRTIO_SND_DRAIN_TIMEOUT_MS 500U

/* Stream id marking a direction the device does not expose. */
#    define VIRTIO_SND_NO_STREAM (-1)

/* Mirrors the QEMU/VirtIO 1.2 device stream state machine. */
enum virtsnd_dev_state {
    VIRTSND_DEV_UNINIT = 0,
    VIRTSND_DEV_PARAMS_SET,
    VIRTSND_DEV_PREPARED,
    VIRTSND_DEV_STARTED,
    VIRTSND_DEV_STOPPED,
    VIRTSND_DEV_RELEASED,
};

struct virtsnd_fmt_map {
        uint8_t          vbit;
        snd_pcm_format_t alsa_fmt;
};

struct virtsnd_rate_map {
        uint8_t      vbit;
        unsigned int alsa_bit;
        unsigned int hz;
};

static const struct virtsnd_fmt_map virtsnd_fmt_table[] = {
    {VIRTIO_SND_PCM_FMT_MU_LAW,  SNDRV_PCM_FORMAT_MU_LAW    },
    {VIRTIO_SND_PCM_FMT_A_LAW,   SNDRV_PCM_FORMAT_A_LAW     },
    {VIRTIO_SND_PCM_FMT_S8,      SNDRV_PCM_FORMAT_S8        },
    {VIRTIO_SND_PCM_FMT_U8,      SNDRV_PCM_FORMAT_U8        },
    {VIRTIO_SND_PCM_FMT_S16,     SNDRV_PCM_FORMAT_S16_LE    },
    {VIRTIO_SND_PCM_FMT_U16,     SNDRV_PCM_FORMAT_U16_LE    },
    {VIRTIO_SND_PCM_FMT_S18_3,   SNDRV_PCM_FORMAT_S18_3LE   },
    {VIRTIO_SND_PCM_FMT_U18_3,   SNDRV_PCM_FORMAT_U18_3LE   },
    {VIRTIO_SND_PCM_FMT_S20_3,   SNDRV_PCM_FORMAT_S20_3LE   },
    {VIRTIO_SND_PCM_FMT_U20_3,   SNDRV_PCM_FORMAT_U20_3LE   },
    {VIRTIO_SND_PCM_FMT_S24_3,   SNDRV_PCM_FORMAT_S24_3LE   },
    {VIRTIO_SND_PCM_FMT_U24_3,   SNDRV_PCM_FORMAT_U24_3LE   },
    {VIRTIO_SND_PCM_FMT_S20,     SNDRV_PCM_FORMAT_S20_LE    },
    {VIRTIO_SND_PCM_FMT_U20,     SNDRV_PCM_FORMAT_U20_LE    },
    {VIRTIO_SND_PCM_FMT_S24,     SNDRV_PCM_FORMAT_S24_LE    },
    {VIRTIO_SND_PCM_FMT_U24,     SNDRV_PCM_FORMAT_U24_LE    },
    {VIRTIO_SND_PCM_FMT_S32,     SNDRV_PCM_FORMAT_S32_LE    },
    {VIRTIO_SND_PCM_FMT_U32,     SNDRV_PCM_FORMAT_U32_LE    },
    {VIRTIO_SND_PCM_FMT_FLOAT,   SNDRV_PCM_FORMAT_FLOAT_LE  },
    {VIRTIO_SND_PCM_FMT_FLOAT64, SNDRV_PCM_FORMAT_FLOAT64_LE},
};

static const struct virtsnd_rate_map virtsnd_rate_table[] = {
    {VIRTIO_SND_PCM_RATE_5512,   ULSA_PCM_RATE_5512,   5512U  },
    {VIRTIO_SND_PCM_RATE_8000,   ULSA_PCM_RATE_8000,   8000U  },
    {VIRTIO_SND_PCM_RATE_11025,  ULSA_PCM_RATE_11025,  11025U },
    {VIRTIO_SND_PCM_RATE_16000,  ULSA_PCM_RATE_16000,  16000U },
    {VIRTIO_SND_PCM_RATE_22050,  ULSA_PCM_RATE_22050,  22050U },
    {VIRTIO_SND_PCM_RATE_32000,  ULSA_PCM_RATE_32000,  32000U },
    {VIRTIO_SND_PCM_RATE_44100,  ULSA_PCM_RATE_44100,  44100U },
    {VIRTIO_SND_PCM_RATE_48000,  ULSA_PCM_RATE_48000,  48000U },
    {VIRTIO_SND_PCM_RATE_64000,  ULSA_PCM_RATE_64000,  64000U },
    {VIRTIO_SND_PCM_RATE_88200,  ULSA_PCM_RATE_88200,  88200U },
    {VIRTIO_SND_PCM_RATE_96000,  ULSA_PCM_RATE_96000,  96000U },
    {VIRTIO_SND_PCM_RATE_176400, ULSA_PCM_RATE_176400, 176400U},
    {VIRTIO_SND_PCM_RATE_192000, ULSA_PCM_RATE_192000, 192000U},
    {VIRTIO_SND_PCM_RATE_384000, ULSA_PCM_RATE_384000, 384000U},
};

/* One DMA message: [xfer][status][pad][period payload] in a single block. */
struct virtsnd_msg {
        uint8_t *mem;
        uint64_t phys;
        size_t   pages;
        size_t   frames;
        size_t   length;
        uint64_t gen;
        uint32_t dir;
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

/* Driver state for one direction (VIRTIO_SND_D_OUTPUT or VIRTIO_SND_D_INPUT). */
struct virtsnd_dir {
        int                    stream_id;
        enum virtsnd_dev_state dev_state;
        ulsa_pcm_substream_t  *substream;
        struct virtsnd_pool    pool;

        uint32_t buffer_bytes;
        uint32_t period_bytes;
        uint32_t frame_bytes;
        uint32_t buffer_frames;
        uint32_t period_frames;

        uint32_t hw_pos;
        uint32_t submit_pos;
        size_t   inflight_frames;
        uint64_t gen;

        bool params_valid;
        bool want_running;
        bool tearing_down;
        bool kick;
};

struct virtio_snd {
        struct vp_device         vp;
        struct vp_virtqueue      vq[VIRTIO_SND_VQ_MAX];
        struct virtio_snd_config cfg;
        struct virtsnd_stream   *streams;
        struct virtsnd_dir       dir[2];

        ulsa_card_t *card;

        /* Lock hierarchy: ops_lock -> substream->lock -> snd->lock; ctl_lock is leaf under ops_lock. */
        mutex_t      ops_lock;
        mutex_t      ctl_lock;
        spinlock_t   lock;
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

/* Map an ALSA stream direction to a VirtIO sound direction index. */
static int virtsnd_dir_index(const ulsa_pcm_substream_t *substream)
{
    return substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? VIRTIO_SND_D_OUTPUT : VIRTIO_SND_D_INPUT;
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
static int virtsnd_pool_alloc(struct virtsnd_pool *pool, int count, uint32_t period_bytes, uint32_t dir)
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
        pool->msgs[i].frames           = 0;
        pool->msgs[i].dir              = dir;
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

/* Send one control request on VQ 0 and wait synchronously for its status response. */
static int virtsnd_ctl(struct virtio_snd *snd, const void *req, size_t req_len, void *items, size_t items_len)
{
    struct vp_virtq_seg  segs[3];
    struct vp_virtqueue *vq    = &snd->vq[VIRTIO_SND_VQ_CONTROL];
    int                  count = 0;
    int                  ret   = EOK;
    uint32_t             len   = 0;
    void                *cookie;
    bool                 timed = false;

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
                    ret   = virtsnd_mark_broken(vq);
                    timed = true;
                    goto out;
                }

                wait_queue_prepare(&snd->ctl_wait);
                cookie = virtqueue_get_buf(vq, &len);
                if (cookie) {
                    wait_queue_cancel(&snd->ctl_wait);
                    break;
                }
                (void)wait_queue_wait_timed(&snd->ctl_wait, deadline);
                continue;
            }

            if (++spins > VIRTIO_SND_CTL_BOOT_SPINS) {
                ret   = virtsnd_mark_broken(vq);
                timed = true;
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
    if (timed) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("virtio_snd: control queue %d stopped responding, card unusable.\n", vq->index);
    }
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

/* Translate a VirtIO format bitmask into ALSA SNDRV_PCM_FMTBIT_* mask. */
static uint64_t virtsnd_build_formats(uint64_t vformats)
{
    uint64_t formats = 0;

    for (size_t i = 0; i < sizeof(virtsnd_fmt_table) / sizeof(virtsnd_fmt_table[0]); i++) {
        if (vformats & (1ULL << virtsnd_fmt_table[i].vbit)) formats |= ULSA_PCM_FMTBIT(virtsnd_fmt_table[i].alsa_fmt);
    }
    return formats;
}

/* Translate a VirtIO rate bitmask into ALSA ULSA_PCM_RATE_* mask and min/max bounds. */
static unsigned int virtsnd_build_rates(uint64_t vrates, unsigned int *min_hz, unsigned int *max_hz)
{
    unsigned int rates = 0;
    unsigned int lo    = ~0U;
    unsigned int hi    = 0;

    for (size_t i = 0; i < sizeof(virtsnd_rate_table) / sizeof(virtsnd_rate_table[0]); i++) {
        if (vrates & (1ULL << virtsnd_rate_table[i].vbit)) {
            rates |= virtsnd_rate_table[i].alsa_bit;
            if (virtsnd_rate_table[i].hz < lo) lo = virtsnd_rate_table[i].hz;
            if (virtsnd_rate_table[i].hz > hi) hi = virtsnd_rate_table[i].hz;
        }
    }
    if (min_hz) *min_hz = hi ? lo : 0;
    if (max_hz) *max_hz = hi;
    return rates;
}

/* Look up the VirtIO format index for an ALSA format, or -EINVAL. */
static int virtsnd_to_vformat(snd_pcm_format_t alsa_fmt)
{
    for (size_t i = 0; i < sizeof(virtsnd_fmt_table) / sizeof(virtsnd_fmt_table[0]); i++) {
        if (virtsnd_fmt_table[i].alsa_fmt == alsa_fmt) return (int)virtsnd_fmt_table[i].vbit;
    }
    return -EINVAL;
}

/* Look up the VirtIO rate index for a rate in Hz, or -EINVAL. */
static int virtsnd_to_vrate(unsigned int hz)
{
    for (size_t i = 0; i < sizeof(virtsnd_rate_table) / sizeof(virtsnd_rate_table[0]); i++) {
        if (virtsnd_rate_table[i].hz == hz) return (int)virtsnd_rate_table[i].vbit;
    }
    return -EINVAL;
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

    /* Data segment precedes status on write-only descriptors per VirtIO spec & Linux. */
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

/* Copy `frames` starting at `ring_pos` out of `dma_area` with wrap-around. */
static void virtsnd_copy_from_dma(void *dst, const ulsa_pcm_runtime_t *runtime, uint32_t ring_pos, size_t frames, uint32_t fb, uint32_t buf_frames)
{
    uint32_t first = buf_frames - ring_pos;

    if (frames <= first) {
        memcpy(dst, runtime->dma_area + (size_t)ring_pos * fb, frames * fb);
    } else {
        memcpy(dst, runtime->dma_area + (size_t)ring_pos * fb, (size_t)first * fb);
        memcpy((uint8_t *)dst + (size_t)first * fb, runtime->dma_area, (frames - first) * fb);
    }
}

/* Copy `frames` into `dma_area` starting at `ring_pos` with wrap-around. */
static void virtsnd_copy_to_dma(ulsa_pcm_runtime_t *runtime, uint32_t ring_pos, const void *src, size_t frames, uint32_t fb, uint32_t buf_frames)
{
    uint32_t first = buf_frames - ring_pos;

    if (frames <= first) {
        memcpy(runtime->dma_area + (size_t)ring_pos * fb, src, frames * fb);
    } else {
        memcpy(runtime->dma_area + (size_t)ring_pos * fb, src, (size_t)first * fb);
        memcpy(runtime->dma_area, (const uint8_t *)src + (size_t)first * fb, (frames - first) * fb);
    }
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

/* Retire completed playback messages, advance hw_pos, and notify ULSA per period. */
static void virtsnd_reap_tx(struct virtio_snd *snd)
{
    struct vp_virtqueue *vq  = &snd->vq[VIRTIO_SND_VQ_TX];
    struct virtsnd_dir  *dir = &snd->dir[VIRTIO_SND_D_OUTPUT];
    void                *cookie;

    while ((cookie = virtqueue_get_buf(vq, NULL)) != NULL) {
        struct virtsnd_msg   *msg       = cookie;
        ulsa_pcm_substream_t *substream = NULL;

        spin_lock(&snd->lock);
        virtsnd_pool_recycle(&dir->pool, msg);
        if (msg->gen == dir->gen && dir->buffer_frames) {
            if (dir->inflight_frames >= msg->frames) {
                dir->inflight_frames -= msg->frames;
            } else {
                dir->inflight_frames = 0;
            }
            if (!dir->tearing_down && dir->dev_state == VIRTSND_DEV_STARTED && dir->substream) {
                dir->hw_pos = (dir->hw_pos + (uint32_t)msg->frames) % dir->buffer_frames;
                substream   = dir->substream;
            }
        }
        spin_unlock(&snd->lock);

        if (substream) ulsa_pcm_period_elapsed(substream);
    }
}

/* Drain completed capture messages, write captured samples into dma_area, and notify ULSA. */
static void virtsnd_reap_rx(struct virtio_snd *snd)
{
    struct vp_virtqueue *vq  = &snd->vq[VIRTIO_SND_VQ_RX];
    struct virtsnd_dir  *dir = &snd->dir[VIRTIO_SND_D_INPUT];
    void                *cookie;
    uint32_t             len = 0;

    while ((cookie = virtqueue_get_buf(vq, &len)) != NULL) {
        struct virtsnd_msg   *msg        = cookie;
        const size_t          status_len = sizeof(struct virtio_snd_pcm_status);
        const size_t          payload    = (len > status_len) ? len - status_len : 0;
        size_t                captured   = (payload < msg->length) ? payload : msg->length;
        ulsa_pcm_substream_t *substream  = NULL;
        ulsa_pcm_runtime_t   *runtime    = NULL;
        uint32_t              pos        = 0;
        uint32_t              fb         = 0;
        uint32_t              buf_frames = 0;
        size_t                frames     = 0;
        bool                  notify     = false;

        /* Inspect capture state while preserving buffer contents before recycling. */
        spin_lock(&snd->lock);
        if (msg->gen == dir->gen && dir->buffer_frames) {
            if (dir->inflight_frames >= msg->frames) {
                dir->inflight_frames -= msg->frames;
            } else {
                dir->inflight_frames = 0;
            }
            if (!dir->tearing_down && dir->dev_state == VIRTSND_DEV_STARTED && dir->substream && dir->substream->runtime && dir->substream->runtime->dma_area && dir->frame_bytes) {
                substream  = dir->substream;
                runtime    = substream->runtime;
                pos        = dir->hw_pos;
                fb         = dir->frame_bytes;
                buf_frames = dir->buffer_frames;
                frames     = captured / fb;
                if (frames > msg->frames) frames = msg->frames;
            }
        }
        spin_unlock(&snd->lock);

        if (substream && frames) {
            /* Safe: ops_lock blocks hw_free/close, and hw_ptr only moves after copy finishes. */
            virtsnd_copy_to_dma(runtime, pos, msg->mem + VIRTIO_SND_MSG_HEAD, frames, fb, buf_frames);
            notify = true;
        }

        spin_lock(&snd->lock);
        if (notify && msg->gen == dir->gen && !dir->tearing_down && dir->buffer_frames) {
            dir->hw_pos = (pos + (uint32_t)frames) % dir->buffer_frames;
        } else {
            notify = false;
        }
        virtsnd_pool_recycle(&dir->pool, msg);
        spin_unlock(&snd->lock);

        if (notify) ulsa_pcm_period_elapsed(substream);
    }
}

/* Read frames currently queued in dma_area and stream draining state under substream->lock. */
static bool virtsnd_snapshot_avail(ulsa_pcm_substream_t *substream, snd_pcm_sframes_t *hw_avail, bool *draining, ulsa_pcm_runtime_t **rt_out)
{
    uint64_t            rflags = spin_lock_irqsave(&substream->lock);
    ulsa_pcm_runtime_t *rt     = substream->runtime;
    bool                ok     = rt && rt->dma_area && ulsa_pcm_running(substream);

    if (ok) {
        *hw_avail = ulsa_pcm_hw_avail(substream);
        *draining = rt->status->state == SNDRV_PCM_STATE_DRAINING;
        if (rt_out) *rt_out = rt;
    }
    spin_unlock_irqrestore(&substream->lock, rflags);
    return ok;
}

/* Stage queued playback periods from dma_area and hand them to the TX queue. */
static void virtsnd_fill_tx(struct virtio_snd *snd)
{
    struct virtsnd_dir *dir       = &snd->dir[VIRTIO_SND_D_OUTPUT];
    int                 submitted = 0;

    for (;;) {
        ulsa_pcm_substream_t *substream;
        ulsa_pcm_runtime_t   *runtime = NULL;
        struct virtsnd_msg   *msg;
        snd_pcm_sframes_t     hw_avail = 0;
        size_t                unsubmitted;
        size_t                chunk;
        uint32_t              buf_frames;
        uint32_t              per_frames;
        uint32_t              fb;
        uint32_t              submit_pos;
        uint64_t              gen;
        bool                  draining = false;

        spin_lock(&snd->lock);
        substream         = dir->substream;
        buf_frames        = dir->buffer_frames;
        per_frames        = dir->period_frames;
        fb                = dir->frame_bytes;
        gen               = dir->gen;
        const bool active = substream && dir->dev_state == VIRTSND_DEV_STARTED && dir->want_running && !dir->tearing_down && dir->pool.free_count > 0;
        spin_unlock(&snd->lock);

        if (!active || !per_frames || !buf_frames || !fb) break;
        if (!virtsnd_snapshot_avail(substream, &hw_avail, &draining, &runtime)) break;

        spin_lock(&snd->lock);
        if (dir->gen != gen || dir->dev_state != VIRTSND_DEV_STARTED || !dir->want_running || dir->tearing_down || dir->pool.free_count == 0) {
            spin_unlock(&snd->lock);
            break;
        }
        if (hw_avail <= (snd_pcm_sframes_t)dir->inflight_frames) {
            spin_unlock(&snd->lock);
            break;
        }
        unsubmitted = (size_t)(hw_avail - (snd_pcm_sframes_t)dir->inflight_frames);
        if (unsubmitted >= per_frames) {
            chunk = per_frames;
        } else if (draining && unsubmitted > 0) {
            chunk = unsubmitted;
        } else {
            spin_unlock(&snd->lock);
            break;
        }

        msg = dir->pool.free[--dir->pool.free_count];
        dir->pool.inflight++;
        msg->in_flight  = true;
        msg->frames     = chunk;
        msg->length     = chunk * fb;
        msg->gen        = gen;
        msg->dir        = VIRTIO_SND_D_OUTPUT;
        submit_pos      = dir->submit_pos;
        dir->submit_pos = (submit_pos + (uint32_t)chunk) % buf_frames;
        dir->inflight_frames += chunk;
        spin_unlock(&snd->lock);

        virtsnd_copy_from_dma(msg->mem + VIRTIO_SND_MSG_HEAD, runtime, submit_pos, chunk, fb, buf_frames);

        if (virtsnd_submit_tx(snd, msg, msg->length, (uint32_t)dir->stream_id) != EOK) {
            spin_lock(&snd->lock);
            virtsnd_pool_recycle(&dir->pool, msg);
            if (dir->gen == gen) {
                dir->inflight_frames -= chunk;
                dir->submit_pos = submit_pos;
            }
            spin_unlock(&snd->lock);
            break;
        }
        submitted++;
    }
    if (submitted) virtqueue_kick(&snd->vq[VIRTIO_SND_VQ_TX]);
}

/* Post empty capture period buffers whenever the capture ring has free space. */
static void virtsnd_fill_rx(struct virtio_snd *snd)
{
    struct virtsnd_dir *dir       = &snd->dir[VIRTIO_SND_D_INPUT];
    int                 submitted = 0;

    for (;;) {
        ulsa_pcm_substream_t *substream;
        struct virtsnd_msg   *msg;
        snd_pcm_sframes_t     hw_avail = 0;
        size_t                free_frames;
        uint32_t              per_frames;
        uint32_t              fb;
        uint64_t              gen;
        bool                  draining = false;

        spin_lock(&snd->lock);
        substream         = dir->substream;
        per_frames        = dir->period_frames;
        fb                = dir->frame_bytes;
        gen               = dir->gen;
        const bool active = substream && dir->dev_state == VIRTSND_DEV_STARTED && dir->want_running && !dir->tearing_down && dir->pool.free_count > 0;
        spin_unlock(&snd->lock);

        if (!active || !per_frames || !fb) break;
        if (!virtsnd_snapshot_avail(substream, &hw_avail, &draining, NULL)) break;

        spin_lock(&snd->lock);
        if (dir->gen != gen || dir->dev_state != VIRTSND_DEV_STARTED || !dir->want_running || dir->tearing_down || dir->pool.free_count == 0) {
            spin_unlock(&snd->lock);
            break;
        }
        if (hw_avail <= (snd_pcm_sframes_t)dir->inflight_frames) {
            spin_unlock(&snd->lock);
            break;
        }
        free_frames = (size_t)(hw_avail - (snd_pcm_sframes_t)dir->inflight_frames);
        if (free_frames < per_frames) {
            spin_unlock(&snd->lock);
            break;
        }

        msg = dir->pool.free[--dir->pool.free_count];
        dir->pool.inflight++;
        msg->in_flight = true;
        msg->frames    = per_frames;
        msg->length    = (size_t)per_frames * fb;
        msg->gen       = gen;
        msg->dir       = VIRTIO_SND_D_INPUT;
        dir->inflight_frames += per_frames;
        spin_unlock(&snd->lock);

        if (virtsnd_submit_rx(snd, msg, msg->length, (uint32_t)dir->stream_id) != EOK) {
            spin_lock(&snd->lock);
            virtsnd_pool_recycle(&dir->pool, msg);
            if (dir->gen == gen) dir->inflight_frames -= per_frames;
            spin_unlock(&snd->lock);
            break;
        }
        submitted++;
    }
    if (submitted) virtqueue_kick(&snd->vq[VIRTIO_SND_VQ_RX]);
}

/* Reap completed I/O and control-idle work; caller holds ops_lock. */
static void virtsnd_wait_inflight(struct virtio_snd *snd, int d)
{
    const uint64_t deadline = sched_ticks() + ((uint64_t)VIRTIO_SND_DRAIN_TIMEOUT_MS * CONFIG_TIMER_HZ) / 1000U;

    for (;;) {
        virtsnd_reap_events(snd);
        virtsnd_reap_tx(snd);
        virtsnd_reap_rx(snd);

        spin_lock(&snd->lock);
        const int inflight = snd->dir[d].pool.inflight;
        spin_unlock(&snd->lock);

        if (inflight == 0) return;
        if (sched_ticks() >= deadline) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("virtio_snd: stream drain timed out with %d message(s) in flight.\n", inflight);
            return;
        }
        usleep(500);
    }
}

/* Stop and release one device stream synchronously and wait for its buffers; caller holds ops_lock. */
static void virtsnd_stop_dir(struct virtio_snd *snd, int d)
{
    struct virtsnd_dir    *dir = &snd->dir[d];
    enum virtsnd_dev_state state;
    uint32_t               stream_id;

    spin_lock(&snd->lock);
    dir->want_running = false;
    dir->tearing_down = true;
    state             = dir->dev_state;
    stream_id         = (uint32_t)dir->stream_id;
    spin_unlock(&snd->lock);

    if (dir->stream_id != VIRTIO_SND_NO_STREAM) {
        if (state == VIRTSND_DEV_STARTED) {
            (void)virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_STOP, stream_id);
            state = VIRTSND_DEV_STOPPED;
        }
        if (state == VIRTSND_DEV_PREPARED || state == VIRTSND_DEV_STOPPED) {
            (void)virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_RELEASE, stream_id);
            state = VIRTSND_DEV_RELEASED;
        }
    }

    virtsnd_wait_inflight(snd, d);

    spin_lock(&snd->lock);
    dir->dev_state       = state;
    dir->tearing_down    = false;
    dir->inflight_frames = 0;
    dir->gen++;
    spin_unlock(&snd->lock);
}

/* Start a prepared (or released) stream on the device; caller holds ops_lock. */
static void virtsnd_start_dir(struct virtio_snd *snd, int d)
{
    struct virtsnd_dir    *dir = &snd->dir[d];
    enum virtsnd_dev_state state;
    uint32_t               stream_id;
    bool                   can_start;
    int                    ret;

    spin_lock(&snd->lock);
    state     = dir->dev_state;
    stream_id = (uint32_t)dir->stream_id;
    can_start = dir->want_running && dir->params_valid && dir->substream != NULL && dir->stream_id != VIRTIO_SND_NO_STREAM;
    spin_unlock(&snd->lock);

    if (!can_start || state == VIRTSND_DEV_STARTED) return;

    if (state == VIRTSND_DEV_STOPPED) {
        ret = virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_RELEASE, stream_id);
        if (ret != EOK) return;
        state = VIRTSND_DEV_RELEASED;
    }
    if (state == VIRTSND_DEV_PARAMS_SET || state == VIRTSND_DEV_RELEASED) {
        ret = virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_PREPARE, stream_id);
        if (ret != EOK) return;
        state = VIRTSND_DEV_PREPARED;
    }
    if (state == VIRTSND_DEV_PREPARED) {
        ret = virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_START, stream_id);
        if (ret == EOK) state = VIRTSND_DEV_STARTED;
    }

    spin_lock(&snd->lock);
    dir->dev_state          = state;
    const bool still_wanted = dir->want_running;
    spin_unlock(&snd->lock);

    if (state == VIRTSND_DEV_STARTED && !still_wanted) virtsnd_stop_dir(snd, d);
}

/* Drain used buffers, execute pending START/STOP transitions, and refill queues. */
static void virtsnd_pump(struct virtio_snd *snd)
{
    if (!snd->card) return;

    mutex_lock(&snd->ops_lock);
    for (int d = 0; d < 2; d++) {
        spin_lock(&snd->lock);
        snd->dir[d].kick = false;
        spin_unlock(&snd->lock);
    }

    virtsnd_reap_events(snd);
    virtsnd_reap_tx(snd);
    virtsnd_reap_rx(snd);

    for (int d = 0; d < 2; d++) {
        struct virtsnd_dir *dir = &snd->dir[d];

        spin_lock(&snd->lock);
        const bool                   want = dir->want_running;
        const enum virtsnd_dev_state st   = dir->dev_state;
        spin_unlock(&snd->lock);

        if (!want && st == VIRTSND_DEV_STARTED) {
            virtsnd_stop_dir(snd, d);
        } else if (want && st != VIRTSND_DEV_STARTED) {
            virtsnd_start_dir(snd, d);
        }

        spin_lock(&snd->lock);
        const bool ready = dir->want_running && dir->dev_state == VIRTSND_DEV_STARTED;
        spin_unlock(&snd->lock);

        if (!ready) continue;
        if (d == VIRTIO_SND_D_OUTPUT) {
            virtsnd_fill_tx(snd);
        } else {
            virtsnd_fill_rx(snd);
        }
    }
    mutex_unlock(&snd->ops_lock);
}

/* Check whether any direction has work ready that would be missed if the worker slept. */
static bool virtsnd_has_pending_work(struct virtio_snd *snd)
{
    bool pending = false;

    spin_lock(&snd->lock);
    for (int d = 0; d < 2; d++) {
        const struct virtsnd_dir *dir = &snd->dir[d];
        if (dir->kick) pending = true;
        if (dir->want_running && dir->dev_state != VIRTSND_DEV_STARTED && dir->params_valid) pending = true;
        if (!dir->want_running && dir->dev_state == VIRTSND_DEV_STARTED) pending = true;
    }
    spin_unlock(&snd->lock);
    return pending;
}

/* Pump worker: synchronizes stream state transitions, reaps completions, and feeds queues. */
static int virtsnd_worker(void *arg)
{
    struct virtio_snd *snd = arg;

    while (!kthread_should_stop()) {
        virtsnd_pump(snd);

        spin_lock(&snd->lock);
        const bool running  = snd->dir[VIRTIO_SND_D_OUTPUT].dev_state == VIRTSND_DEV_STARTED || snd->dir[VIRTIO_SND_D_INPUT].dev_state == VIRTSND_DEV_STARTED;
        const int  inflight = snd->dir[VIRTIO_SND_D_OUTPUT].pool.inflight + snd->dir[VIRTIO_SND_D_INPUT].pool.inflight;
        spin_unlock(&snd->lock);

        const bool     idle_stream = running && inflight == 0;
        const uint64_t deadline    = (idle_stream || !snd->irq_enabled) ? sched_ticks() + VIRTIO_SND_IDLE_TICKS : sched_ticks() + VIRTIO_SND_SAFETY_TICKS;
        wait_queue_prepare(&snd->pump_wait);

        /* Recheck state transitions, explicit kicks, and used rings before sleeping. */
        if (kthread_should_stop() || virtsnd_has_pending_work(snd) || virtqueue_enable_cb(&snd->vq[VIRTIO_SND_VQ_TX]) || virtqueue_enable_cb(&snd->vq[VIRTIO_SND_VQ_RX])
            || virtqueue_enable_cb(&snd->vq[VIRTIO_SND_VQ_EVENT])) {
            wait_queue_cancel(&snd->pump_wait);
            continue;
        }
        (void)wait_queue_wait_timed(&snd->pump_wait, deadline);
    }
    return EOK;
}

/* ULSA callback: expose device capabilities and constraints for one substream. */
static int virtsnd_pcm_open(ulsa_pcm_substream_t *substream)
{
    struct virtio_snd           *snd     = substream->pcm->private_data;
    const int                    d       = virtsnd_dir_index(substream);
    struct virtsnd_dir          *dir     = &snd->dir[d];
    ulsa_pcm_runtime_t          *runtime = substream->runtime;
    const struct virtsnd_stream *vs;
    uint64_t                     formats;
    unsigned int                 rates;
    unsigned int                 rate_min = 0;
    unsigned int                 rate_max = 0;
    unsigned int                 ch_min;
    unsigned int                 ch_max;
    int                          ret;

    if (!snd->worker_started || dir->stream_id == VIRTIO_SND_NO_STREAM) return -ENODEV;

    vs      = &snd->streams[dir->stream_id];
    formats = virtsnd_build_formats(vs->formats);
    rates   = virtsnd_build_rates(vs->rates, &rate_min, &rate_max);
    if (!formats || !rates) return -EINVAL;

    ch_min = vs->channels_min ? vs->channels_min : 1U;
    ch_max = vs->channels_max >= ch_min ? vs->channels_max : ch_min;

    memset(&runtime->hw, 0, sizeof(runtime->hw));
    runtime->hw.info             = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BATCH | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_DRAIN_TRIGGER;
    runtime->hw.formats          = formats;
    runtime->hw.rates            = rates;
    runtime->hw.rate_min         = rate_min;
    runtime->hw.rate_max         = rate_max;
    runtime->hw.channels_min     = ch_min;
    runtime->hw.channels_max     = ch_max;
    runtime->hw.buffer_bytes_max = 256UL * 1024UL;
    runtime->hw.period_bytes_min = 256U;
    runtime->hw.period_bytes_max = 64UL * 1024UL;
    runtime->hw.periods_min      = 2U;
    runtime->hw.periods_max      = 64U;

    /* QEMU rejects SET_PARAMS unless buffer_bytes is an exact multiple of period_bytes. */
    ret = ulsa_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    if (ret < 0) return ret;

    mutex_lock(&snd->ops_lock);
    if (dir->substream) {
        mutex_unlock(&snd->ops_lock);
        return -EBUSY;
    }
    spin_lock(&snd->lock);
    dir->substream       = substream;
    dir->params_valid    = false;
    dir->want_running    = false;
    dir->hw_pos          = 0;
    dir->submit_pos      = 0;
    dir->inflight_frames = 0;
    spin_unlock(&snd->lock);
    mutex_unlock(&snd->ops_lock);
    return EOK;
}

/* ULSA callback: tear down and detach one substream on close. */
static int virtsnd_pcm_close(ulsa_pcm_substream_t *substream)
{
    struct virtio_snd  *snd = substream->pcm->private_data;
    const int           d   = virtsnd_dir_index(substream);
    struct virtsnd_dir *dir = &snd->dir[d];

    mutex_lock(&snd->ops_lock);
    virtsnd_stop_dir(snd, d);
    virtsnd_pool_release(&dir->pool);

    spin_lock(&snd->lock);
    dir->substream     = NULL;
    dir->params_valid  = false;
    dir->buffer_frames = 0;
    dir->period_frames = 0;
    dir->frame_bytes   = 0;
    spin_unlock(&snd->lock);
    mutex_unlock(&snd->ops_lock);
    return EOK;
}

/* ULSA callback: allocate the period message pool and send SET_PARAMS to the device. */
static int virtsnd_pcm_hw_params(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    struct virtio_snd               *snd       = substream->pcm->private_data;
    const int                        d         = virtsnd_dir_index(substream);
    struct virtsnd_dir              *dir       = &snd->dir[d];
    const snd_pcm_format_t           fmt       = ulsa_params_format(params);
    const unsigned int               rate      = ulsa_params_rate(params);
    const unsigned int               channels  = ulsa_params_channels(params);
    const size_t                     buf_bytes = ulsa_params_buffer_bytes(params);
    const size_t                     per_bytes = ulsa_params_period_bytes(params);
    const unsigned int               periods   = ulsa_params_periods(params);
    const int                        vformat   = virtsnd_to_vformat(fmt);
    const int                        vrate     = virtsnd_to_vrate(rate);
    const int                        phys_bits = ulsa_pcm_format_physical_width(fmt);
    struct virtio_snd_pcm_set_params req;
    int                              count;
    int                              ret;

    if (vformat < 0 || vrate < 0 || phys_bits <= 0) return -EINVAL;
    if (per_bytes == 0 || buf_bytes == 0 || (buf_bytes % per_bytes) != 0) return -EINVAL;

    count = (int)(periods < VIRTIO_SND_MSGS_MAX ? periods : VIRTIO_SND_MSGS_MAX);
    if (count < 2) count = 2;

    mutex_lock(&snd->ops_lock);
    virtsnd_stop_dir(snd, d);
    virtsnd_pool_release(&dir->pool);

    ret = virtsnd_pool_alloc(&dir->pool, count, (uint32_t)per_bytes, (uint32_t)d);
    if (ret != EOK) goto out;

    memset(&req, 0, sizeof(req));
    req.hdr.hdr.code  = VIRTIO_SND_R_PCM_SET_PARAMS;
    req.hdr.stream_id = (uint32_t)dir->stream_id;
    req.buffer_bytes  = (uint32_t)buf_bytes;
    req.period_bytes  = (uint32_t)per_bytes;
    req.features      = 0;
    req.channels      = (uint8_t)channels;
    req.format        = (uint8_t)vformat;
    req.rate          = (uint8_t)vrate;

    ret = virtsnd_ctl(snd, &req, sizeof(req), NULL, 0);
    if (ret != EOK) {
        virtsnd_pool_release(&dir->pool);
        goto out;
    }

    spin_lock(&snd->lock);
    dir->dev_state     = VIRTSND_DEV_PARAMS_SET;
    dir->params_valid  = true;
    dir->buffer_bytes  = (uint32_t)buf_bytes;
    dir->period_bytes  = (uint32_t)per_bytes;
    dir->frame_bytes   = (uint32_t)(phys_bits / 8) * channels;
    dir->buffer_frames = ulsa_params_buffer_size(params);
    dir->period_frames = ulsa_params_period_size(params);
    dir->hw_pos        = 0;
    dir->submit_pos    = 0;
    spin_unlock(&snd->lock);
out:
    mutex_unlock(&snd->ops_lock);
    return ret;
}

/* ULSA callback: release device stream and free driver-side message pool. */
static int virtsnd_pcm_hw_free(ulsa_pcm_substream_t *substream)
{
    struct virtio_snd  *snd = substream->pcm->private_data;
    const int           d   = virtsnd_dir_index(substream);
    struct virtsnd_dir *dir = &snd->dir[d];

    mutex_lock(&snd->ops_lock);
    virtsnd_stop_dir(snd, d);
    virtsnd_pool_release(&dir->pool);

    spin_lock(&snd->lock);
    dir->params_valid    = false;
    dir->hw_pos          = 0;
    dir->submit_pos      = 0;
    dir->inflight_frames = 0;
    spin_unlock(&snd->lock);
    mutex_unlock(&snd->ops_lock);
    return EOK;
}

/* ULSA callback: move device stream to PREPARED and zero hardware cursors. */
static int virtsnd_pcm_prepare(ulsa_pcm_substream_t *substream)
{
    struct virtio_snd  *snd = substream->pcm->private_data;
    const int           d   = virtsnd_dir_index(substream);
    struct virtsnd_dir *dir = &snd->dir[d];
    int                 ret;

    mutex_lock(&snd->ops_lock);
    virtsnd_stop_dir(snd, d);
    if (!dir->params_valid || dir->stream_id == VIRTIO_SND_NO_STREAM) {
        mutex_unlock(&snd->ops_lock);
        return -EINVAL;
    }

    ret = virtsnd_pcm_request(snd, VIRTIO_SND_R_PCM_PREPARE, (uint32_t)dir->stream_id);
    if (ret == EOK) {
        spin_lock(&snd->lock);
        dir->dev_state       = VIRTSND_DEV_PREPARED;
        dir->hw_pos          = 0;
        dir->submit_pos      = 0;
        dir->inflight_frames = 0;
        dir->want_running    = false;
        dir->tearing_down    = false;
        dir->gen++;
        spin_unlock(&snd->lock);
    }
    mutex_unlock(&snd->ops_lock);
    return ret;
}

/* ULSA callback: request async start/stop/drain on the pump worker (atomic context). */
static int virtsnd_pcm_trigger(ulsa_pcm_substream_t *substream, int cmd)
{
    struct virtio_snd  *snd = substream->pcm->private_data;
    const int           d   = virtsnd_dir_index(substream);
    struct virtsnd_dir *dir = &snd->dir[d];

    spin_lock(&snd->lock);
    switch (cmd) {
        case ULSA_PCM_TRIGGER_START :
            dir->want_running = true;
            dir->kick         = true;
            break;
        case ULSA_PCM_TRIGGER_STOP :
            dir->want_running = false;
            dir->kick         = true;
            break;
        case ULSA_PCM_TRIGGER_DRAIN :
            dir->kick = true;
            break;
        default :
            spin_unlock(&snd->lock);
            return -EINVAL;
    }
    spin_unlock(&snd->lock);

    (void)wait_queue_wake_all(&snd->pump_wait);
    return EOK;
}

/* ULSA callback: report the completed ring position in frames (atomic context). */
static snd_pcm_uframes_t virtsnd_pcm_pointer(ulsa_pcm_substream_t *substream)
{
    struct virtio_snd  *snd = substream->pcm->private_data;
    const int           d   = virtsnd_dir_index(substream);
    struct virtsnd_dir *dir = &snd->dir[d];
    snd_pcm_uframes_t   pos;

    spin_lock(&snd->lock);
    pos = dir->hw_pos;
    spin_unlock(&snd->lock);
    return pos;
}

/* ULSA callback: nudge the pump worker when user space updates appl_ptr. */
static int virtsnd_pcm_ack(ulsa_pcm_substream_t *substream)
{
    struct virtio_snd  *snd = substream->pcm->private_data;
    const int           d   = virtsnd_dir_index(substream);
    struct virtsnd_dir *dir = &snd->dir[d];
    bool                notify;

    spin_lock(&snd->lock);
    notify = dir->want_running;
    if (notify) dir->kick = true;
    spin_unlock(&snd->lock);

    if (notify) (void)wait_queue_wake_all(&snd->pump_wait);
    return EOK;
}

static const ulsa_pcm_ops_t virtsnd_pcm_ops = {
    .open      = virtsnd_pcm_open,
    .close     = virtsnd_pcm_close,
    .hw_params = virtsnd_pcm_hw_params,
    .hw_free   = virtsnd_pcm_hw_free,
    .prepare   = virtsnd_pcm_prepare,
    .trigger   = virtsnd_pcm_trigger,
    .pointer   = virtsnd_pcm_pointer,
    .ack       = virtsnd_pcm_ack,
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
        plogk("virtio_snd: interrupt setup failed, falling back to timed polling.\n");
        return;
    }

    __atomic_store_n(&virtsnd_irq_device, snd, __ATOMIC_RELEASE);
    snd->irq_enabled = true;
}

/* Disable the device interrupt and release the configured MSI/MSI-X vector. */
static void virtsnd_irq_fini(struct virtio_snd *snd)
{
    if (!snd || !snd->irq_enabled) return;
    __atomic_store_n(&virtsnd_irq_device, NULL, __ATOMIC_RELEASE);
    pci_free_irq(snd->vp.pci_dev, &snd->irq_state);
    snd->irq_enabled = false;
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

    snd->ctl_req  = virtsnd_alloc_page();
    snd->ctl_resp = virtsnd_alloc_page();
    if (!snd->ctl_req || !snd->ctl_resp) {
        ret = -ENOMEM;
        goto fail;
    }

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

    _Static_assert((size_t)VIRTIO_SND_STREAMS_MAX * sizeof(*info) <= PAGE_4K_SIZE, "PCM info page");
    if (count == 0 || count > VIRTIO_SND_STREAMS_MAX) return -EINVAL;

    snd->streams = calloc(VIRTIO_SND_STREAMS_MAX, sizeof(*snd->streams));
    if (!snd->streams) return -ENOMEM;

    info = (struct virtio_snd_pcm_info *)virtsnd_alloc_page();
    if (!info) {
        free(snd->streams);
        snd->streams = NULL;
        return -ENOMEM;
    }

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

            if (info[i].direction == VIRTIO_SND_D_OUTPUT && snd->dir[VIRTIO_SND_D_OUTPUT].stream_id == VIRTIO_SND_NO_STREAM) {
                snd->dir[VIRTIO_SND_D_OUTPUT].stream_id = (int)i;
            } else if (info[i].direction == VIRTIO_SND_D_INPUT && snd->dir[VIRTIO_SND_D_INPUT].stream_id == VIRTIO_SND_NO_STREAM) {
                snd->dir[VIRTIO_SND_D_INPUT].stream_id = (int)i;
            }
        }
    }

    virtsnd_free_page(info);
    return ret;
}

/* Give back everything held before the sound card was published. */
static void virtsnd_cleanup(struct virtio_snd *snd)
{
    virtsnd_irq_fini(snd);

    virtsnd_pool_release(&snd->dir[VIRTIO_SND_D_OUTPUT].pool);
    virtsnd_pool_release(&snd->dir[VIRTIO_SND_D_INPUT].pool);
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

/* Probe a virtio-snd device, register its ULSA card/PCM, and start its pump worker. */
void virtio_snd_init(void)
{
    struct virtio_snd *snd;
    ulsa_card_t       *card     = NULL;
    ulsa_pcm_t        *pcm      = NULL;
    uint64_t           features = 0;
    int                playback_count;
    int                capture_count;
    int                ret;

    snd = malloc(sizeof(*snd));
    if (!snd) return;
    memset(snd, 0, sizeof(*snd));

    snd->dir[VIRTIO_SND_D_OUTPUT].stream_id = VIRTIO_SND_NO_STREAM;
    snd->dir[VIRTIO_SND_D_INPUT].stream_id  = VIRTIO_SND_NO_STREAM;
    snd->lock.rflags                        = 0;
    mutex_init(&snd->ops_lock);
    mutex_init(&snd->ctl_lock);
    wait_queue_init(&snd->ctl_wait);
    wait_queue_init(&snd->pump_wait);

    ret = vp_find_device(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_VIRTIO_SOUND, &snd->vp);
    if (ret != EOK) ret = vp_find_device(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_VIRTIO_SOUND_LEGACY, &snd->vp);
    if (ret != EOK) {
        free(snd);
        return;
    }

    pci_enable_device(snd->vp.pci_dev, PCI_CMD_MEM | PCI_CMD_BUSMASTER);
    vp_setup_device(&snd->vp);

    ret = vp_negotiate_features(&snd->vp, (1ULL << VIRTIO_F_VERSION_1), &features);
    if (ret != EOK || !(features & (1ULL << VIRTIO_F_VERSION_1))) {
        plogk("virtio_snd: device does not offer VIRTIO_F_VERSION_1 (features 0x%016llx)\n", features);
        goto fail_device;
    }

    vp_set_status(&snd->vp, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    compiler_barrier();
    if (!(vp_get_status(&snd->vp) & VIRTIO_STATUS_FEATURES_OK)) {
        plogk("virtio_snd: device rejected feature negotiation (status 0x%02x)\n", vp_get_status(&snd->vp));
        goto fail_device;
    }

    ret = virtsnd_vq_init(snd);
    if (ret != EOK) goto fail_device;

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
    playback_count = (snd->dir[VIRTIO_SND_D_OUTPUT].stream_id != VIRTIO_SND_NO_STREAM) ? 1 : 0;
    capture_count  = (snd->dir[VIRTIO_SND_D_INPUT].stream_id != VIRTIO_SND_NO_STREAM) ? 1 : 0;
    if (!playback_count && !capture_count) {
        plogk("virtio_snd: device exposes no playback or capture stream.\n");
        goto fail_queues;
    }

    virtsnd_irq_init(snd);
    plogk("virtio_snd: %u stream(s), playback id %d, capture id %d, %s interrupts.\n", snd->cfg.streams, snd->dir[VIRTIO_SND_D_OUTPUT].stream_id, snd->dir[VIRTIO_SND_D_INPUT].stream_id,
          snd->irq_enabled ? "MSI-X" : "timed");

    ret = ulsa_card_new(-1, "virtiosnd", &card);
    if (ret != EOK) {
        plogk("virtio_snd: ulsa_card_new failed: %d\n", ret);
        goto fail_queues;
    }
    strncpy(card->driver, "virtio-snd", sizeof(card->driver) - 1);
    strncpy(card->shortname, "VirtIO SoundCard", sizeof(card->shortname) - 1);
    strncpy(card->longname, "VirtIO SoundCard", sizeof(card->longname) - 1);
    card->private_data = snd;

    ret = ulsa_pcm_new(card, "VirtIO PCM", 0, playback_count, capture_count, &pcm);
    if (ret != EOK) {
        plogk("virtio_snd: ulsa_pcm_new failed: %d\n", ret);
        ulsa_card_free(card);
        goto fail_queues;
    }
    pcm->private_data = snd;
    if (playback_count) ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &virtsnd_pcm_ops);
    if (capture_count) ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &virtsnd_pcm_ops);
    ulsa_pcm_set_managed_buffer_all(pcm, 0, 256UL * 1024UL);

    snd->card = card;
    ret       = ulsa_card_register(card);
    if (ret != EOK) {
        plogk("virtio_snd: ulsa_card_register failed: %d\n", ret);
        snd->card = NULL;
        ulsa_card_free(card);
        goto fail_queues;
    }

    ret = kernel_worker_register("virtio-snd-pump", virtsnd_worker, snd, &snd->worker);
    if (ret == EOK) {
        snd->worker_started = true;
    } else {
        plogk("virtio_snd: pump worker not registered (%d), PCM data path disabled.\n", ret);
    }
    return;
fail_queues:
    virtsnd_cleanup(snd);
    free(snd);
    return;
fail_device:
    vp_release_device(&snd->vp);
    free(snd);
}

#endif
