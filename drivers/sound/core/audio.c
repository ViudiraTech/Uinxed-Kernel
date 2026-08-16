/*
 *
 *      audio.c
 *      Generic audio subsystem
 *
 *      2026/7/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/sound/core/audio.h>
#include <fs/core/vfs.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/uaccess.h>

#define NODES_PER_CARD 4
#define MAX_NODES      ((size_t)AUDIO_MAX_CARDS * NODES_PER_CARD)

static audio_card_t        audio_cards[AUDIO_MAX_CARDS];
static audio_device_node_t audio_nodes[MAX_NODES];
static char                audio_node_names[MAX_NODES][24];
static size_t              audio_cards_count;
static size_t              audio_nodes_count;

/* Return the tmpfs node name suffix for a node type. */
static const char *audio_node_suffix(audio_node_type_t type)
{
    switch (type) {
        case audio_node_control :
            return "controlC%u";
        case audio_node_pcm_playback :
            return "pcmC%uD0p";
        case audio_node_pcm_capture :
            return "pcmC%uD0c";
        case audio_node_mixer :
            return "mixerC%uD0";
    }
    return "unknownC%u";
}

/* Allocate the next device-node slot and bind the card's tmpfs ops. */
static int audio_add_node(audio_card_t *card, audio_node_type_t type)
{
    audio_device_node_t *node;

    if (audio_nodes_count >= MAX_NODES) return -ENOSPC;

    node = &audio_nodes[audio_nodes_count];
    (void)snprintf(audio_node_names[audio_nodes_count], sizeof(audio_node_names[audio_nodes_count]), audio_node_suffix(type), card->id);

    node->card                 = card;
    node->type                 = type;
    node->name                 = audio_node_names[audio_nodes_count];
    node->tmpfs_ops.open       = audio_file_open;
    node->tmpfs_ops.release    = audio_file_release;
    node->tmpfs_ops.file_read  = audio_file_read;
    node->tmpfs_ops.file_write = audio_file_write;
    node->tmpfs_ops.file_poll  = audio_file_poll;
    node->tmpfs_ops.file_ioctl = audio_file_ioctl;
    node->tmpfs_ops.read       = audio_device_read;
    node->tmpfs_ops.write      = audio_device_write;
    node->tmpfs_ops.poll       = audio_device_poll;
    node->tmpfs_ops.ioctl      = audio_device_ioctl;
    node->tmpfs_ops.ctx        = node;

    /* Publish the /dev/snd node dynamically. */
    char     dev_path[64];
    uint64_t devt = MKDEV(SND_DEV_MAJOR, (uint32_t)audio_nodes_count);
    (void)snprintf(dev_path, sizeof(dev_path), "/dev/snd/%s", node->name);
    int status = devtmpfs_register_char_device(dev_path, devt, devt, file_audio | file_stream, &node->tmpfs_ops);
    if (status != EOK) {
        plogk("audio: Failed to register %s: %d\n", dev_path, status);
        return status;
    }

    audio_nodes_count++;
    return EOK;
}

/* Fill the userspace card-info structure from the card's state. */
static void audio_fill_info(audio_card_t *card, audio_card_info_t *info)
{
    info->card        = card->id;
    info->sample_rate = card->format.sample_rate;
    info->bits        = card->format.bits;
    info->channels    = card->format.channels;
    strncpy(info->name, card->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
}

/* Register an audio card and create its device nodes. */
int audio_register_card(const char *name, const audio_pcm_format_t *format, const audio_card_ops_t *ops, void *driver_data)
{
    audio_card_t *card;
    int           status;
    size_t        node_base;

    if (!name || !format || !ops) return -EINVAL;
    if (audio_cards_count >= AUDIO_MAX_CARDS) return -ENOSPC;

    card = &audio_cards[audio_cards_count];
    memset(card, 0, sizeof(*card));

    card->id           = audio_cards_count;
    card->format       = *format;
    card->ops          = ops;
    card->driver_data  = driver_data;
    card->volume.left  = 0xCC;
    card->volume.right = 0xCC;
    strncpy(card->name, name, sizeof(card->name) - 1);

    node_base = audio_nodes_count;
    status    = audio_add_node(card, audio_node_control);
    if (status != EOK) goto rollback;
    status = audio_add_node(card, audio_node_pcm_playback);
    if (status != EOK) goto rollback;
    status = audio_add_node(card, audio_node_pcm_capture);
    if (status != EOK) goto rollback;
    status = audio_add_node(card, audio_node_mixer);
    if (status != EOK) goto rollback;

    audio_cards_count++;
    plogk("audio: Registered card%u: %s\n", card->id, card->name);
    return (int)card->id;
rollback:
    audio_nodes_count = node_base;
    memset(card, 0, sizeof(*card));
    return status;
}

/* Return the card at the given index, or NULL. */
audio_card_t *audio_get_card(uint32_t card)
{
    if (card >= audio_cards_count) return 0;
    return &audio_cards[card];
}

/* Return the number of registered cards. */
size_t audio_card_count(void)
{
    return audio_cards_count;
}

/* Return the number of device nodes. */
size_t audio_device_node_count(void)
{
    return audio_nodes_count;
}

/* Return the device node at the given index, or NULL. */
audio_device_node_t *audio_get_device_node(size_t index)
{
    if (index >= audio_nodes_count) return 0;
    return &audio_nodes[index];
}

/* PCM Ring Buffer */
/* Allocate and initialize the ring buffer for the given frame count. */
int pcm_ring_buffer_init(audio_pcm_file_t *pf, size_t size_frames)
{
    size_t fb    = (size_t)(pf->fmt.bits / 8) * pf->fmt.channels;
    size_t bytes = size_frames * fb;
    if (bytes == 0 || bytes > ((size_t)2 * 1024 * 1024)) return -EINVAL;

    pf->ring_buf = malloc(bytes);
    if (!pf->ring_buf) return -ENOMEM;

    pf->ring_buf_size = bytes;
    pf->boundary      = size_frames;
    pf->appl_ptr      = 0;
    pf->hw_ptr        = 0;
    pf->xrun          = 0;
    pf->state         = SNDRV_PCM_STATE_SETUP;

    if (pf->period_size == 0) {
        pf->period_size = size_frames / 4;
        if (pf->period_size < 32) pf->period_size = 32;
    }
    if (pf->avail_min == 0) pf->avail_min = pf->period_size;
    if (pf->start_threshold == 0) pf->start_threshold = pf->period_size * 2;
    if (pf->period_bytes == 0) pf->period_bytes = pf->period_size * fb;

    return EOK;
}

/* Free the ring buffer backing memory. */
void pcm_ring_buffer_destroy(audio_pcm_file_t *pf)
{
    if (pf->ring_buf) free(pf->ring_buf);
    pf->ring_buf      = 0;
    pf->ring_buf_size = 0;
}

/* Return the byte count of one interleaved frame. */
static size_t frame_bytes(const audio_pcm_format_t *fmt)
{
    return (size_t)(fmt->bits / 8) * fmt->channels;
}

/* Number of recorded frames available for the application. */
snd_pcm_sframes_t pcm_ring_buffer_avail(audio_pcm_file_t *pf)
{
    snd_pcm_uframes_t hw  = pf->hw_ptr;
    snd_pcm_uframes_t app = pf->appl_ptr;
    if (hw < app) hw += pf->boundary;
    return (snd_pcm_sframes_t)(hw - app);
}

/* Free frame capacity left in the ring. */
snd_pcm_sframes_t pcm_ring_buffer_space(audio_pcm_file_t *pf)
{
    return (snd_pcm_sframes_t)(pf->boundary - pcm_ring_buffer_avail(pf) - 1);
}

/* Advance the hardware pointer by the given frame count. */
void pcm_ring_buffer_advance_hw(audio_pcm_file_t *pf, snd_pcm_uframes_t frames)
{
    pf->hw_ptr = (pf->hw_ptr + frames) % pf->boundary;
}

/* Copy frames into the ring from the application pointer, wrapping as needed. */
size_t pcm_ring_buffer_write_frames(audio_pcm_file_t *pf, const void *data, size_t frames)
{
    size_t fb      = frame_bytes(&pf->fmt);
    size_t space   = (size_t)pcm_ring_buffer_space(pf);
    size_t to_copy = (frames < space) ? frames : space;

    if (to_copy == 0) return 0;

    size_t byte_off = (size_t)(pf->appl_ptr % pf->boundary) * fb;
    size_t byte_cnt = to_copy * fb;

    if (byte_off + byte_cnt <= pf->ring_buf_size) {
        memcpy(pf->ring_buf + byte_off, data, byte_cnt);
    } else {
        size_t first = pf->ring_buf_size - byte_off;
        memcpy(pf->ring_buf + byte_off, data, first);
        memcpy(pf->ring_buf, (const uint8_t *)data + first, byte_cnt - first);
    }

    pf->appl_ptr = (pf->appl_ptr + to_copy) % pf->boundary;
    return to_copy;
}

/* Copy recorded frames out of the ring from the hardware pointer. */
size_t pcm_ring_buffer_read_frames(audio_pcm_file_t *pf, void *data, size_t frames)
{
    size_t fb      = frame_bytes(&pf->fmt);
    size_t avail   = (size_t)pcm_ring_buffer_avail(pf);
    size_t to_copy = (frames < avail) ? frames : avail;

    if (to_copy == 0) return 0;

    size_t byte_off = (size_t)(pf->hw_ptr % pf->boundary) * fb;
    size_t byte_cnt = to_copy * fb;

    if (byte_off + byte_cnt <= pf->ring_buf_size) {
        memcpy(data, pf->ring_buf + byte_off, byte_cnt);
    } else {
        size_t first = pf->ring_buf_size - byte_off;
        memcpy(data, pf->ring_buf + byte_off, first);
        memcpy((uint8_t *)data + first, pf->ring_buf, byte_cnt - first);
    }

    pf->hw_ptr = (pf->hw_ptr + to_copy) % pf->boundary;
    return to_copy;
}

/* Add src into dst with s16 clamping. */
size_t audio_mix_interleaved_s16(int16_t *dst, const int16_t *src, size_t frames, unsigned int channels)
{
    size_t total = frames * channels;
    for (size_t i = 0; i < total; i++) {
        int32_t s = (int32_t)dst[i] + (int32_t)src[i];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        dst[i] = (int16_t)s;
    }
    return frames;
}

/* Per-open instance helpers */
/* Allocate and initialize the per-open PCM file state. */
static audio_pcm_file_t *audio_pcm_create(audio_device_node_t *node)
{
    audio_pcm_file_t *pf;

    pf = malloc(sizeof(*pf));
    if (!pf) return 0;
    memset(pf, 0, sizeof(*pf));

    pf->card        = node->card;
    pf->type        = node->type;
    pf->fmt         = node->card->format;
    pf->state       = SNDRV_PCM_STATE_OPEN;
    pf->nonblock    = 0;
    pf->lock.lock   = 0;
    pf->lock.rflags = 0;
    wait_queue_init(&pf->read_wait);
    wait_queue_init(&pf->write_wait);

    return pf;
}

/* Tear down a PCM file and release its ring buffer. */
static void audio_pcm_destroy(audio_pcm_file_t *pf)
{
    if (!pf) return;
    pf->lock.lock = 0;
    pcm_ring_buffer_destroy(pf);
    pf->state = SNDRV_PCM_STATE_OPEN;
    wait_queue_wake_all(&pf->read_wait);
    wait_queue_wake_all(&pf->write_wait);
    free(pf);
}

/* file_open / file_release */
/* Create a per-open PCM file state for a device node. */
int audio_file_open(vfs_node_t vnode, uint64_t flags, void **private_data)
{
    (void)flags;
    tmpfs_file_t        *f          = (tmpfs_file_t *)vnode->handle;
    audio_device_node_t *audio_node = (audio_device_node_t *)(f ? f->device.ctx : NULL);
    audio_pcm_file_t    *pf;

    if (!audio_node || !audio_node->card) return -ENODEV;

    pf = audio_pcm_create(audio_node);
    if (!pf) return -ENOMEM;

    *private_data = pf;
    return EOK;
}

/* Destroy the per-open PCM file state. */
void audio_file_release(vfs_node_t node, void *private_data)
{
    (void)node;
    audio_pcm_destroy((audio_pcm_file_t *)private_data);
}

/* Per-open PCM read/write (file_*) */
/* Read from the control/mixer node or drain the capture ring. */
int64_t audio_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;
    audio_pcm_file_t *pf = private_data;
    audio_card_t     *card;
    size_t            ret = 0;

    (void)offset;
    if (!pf || !addr || !size) return 0;
    card = pf->card;

    if (pf->type == audio_node_control) {
        audio_card_info_t info;
        audio_fill_info(card, &info);
        size_t cpy = (size < sizeof(info)) ? size : sizeof(info);
        memcpy(addr, &info, cpy);
        return (int64_t)cpy;
    }

    if (pf->type == audio_node_mixer) {
        audio_volume_t vol;
        vol        = card->volume;
        size_t cpy = (size < sizeof(vol)) ? size : sizeof(vol);
        memcpy(addr, &vol, cpy);
        return (int64_t)cpy;
    }

    if (pf->type != audio_node_pcm_capture) return 0;
    if (pf->state < SNDRV_PCM_STATE_PREPARED) return 0;

    size_t fb     = frame_bytes(&pf->fmt);
    size_t frames = size / fb;

    if (frames == 0) return 0;

    spin_lock(&pf->lock);
    while (frames > 0) {
        snd_pcm_sframes_t avail = pcm_ring_buffer_avail(pf);
        if (avail <= 0) {
            if (pf->nonblock) {
                ret = ret ? ret : (size_t)(-EAGAIN);
                break;
            }
            if (pf->state == SNDRV_PCM_STATE_DRAINING || pf->state == SNDRV_PCM_STATE_XRUN) break;
            wait_queue_prepare(&pf->read_wait);
            spin_unlock(&pf->lock);
            wait_queue_sleep();
            spin_lock(&pf->lock);
            continue;
        }

        size_t chunk = (frames < (size_t)avail) ? frames : (size_t)avail;
        size_t done  = pcm_ring_buffer_read_frames(pf, (uint8_t *)addr + ret * fb, chunk);
        if (done == 0) break;

        ret += done;
        frames -= done;
    }
    spin_unlock(&pf->lock);
    return (int64_t)(ret * fb);
}

/* Write PCM data into the playback ring. */
int64_t audio_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;
    audio_pcm_file_t *pf = private_data;
    audio_card_t     *card;
    size_t            ret = 0;

    (void)offset;
    if (!pf || !addr || !size) return 0;
    card = pf->card;

    if (pf->type == audio_node_control || pf->type == audio_node_mixer) return 0;

    if (pf->type != audio_node_pcm_playback) return 0;

    if (pf->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;

    size_t fb     = frame_bytes(&pf->fmt);
    size_t frames = size / fb;

    if (frames == 0) return 0;

    if (pf->state == SNDRV_PCM_STATE_SETUP) {
        if (!pf->ring_buf) {
            pf->period_size     = 1024;
            pf->start_threshold = 2048;
            pf->avail_min       = 1024;
            if (pcm_ring_buffer_init(pf, pf->start_threshold * 4) != EOK) return -ENOMEM;
            pf->period_bytes = pf->period_size * fb;
        }
        pf->state = SNDRV_PCM_STATE_PREPARED;
    }

    spin_lock(&pf->lock);
    while (frames > 0) {
        snd_pcm_sframes_t space = pcm_ring_buffer_space(pf);
        if (space <= 0) {
            if (pf->nonblock) {
                ret = ret ? ret : (size_t)(-EAGAIN);
                break;
            }
            wait_queue_prepare(&pf->write_wait);
            spin_unlock(&pf->lock);
            wait_queue_sleep();
            spin_lock(&pf->lock);
            continue;
        }

        size_t chunk = (frames < (size_t)space) ? frames : (size_t)space;
        size_t done  = pcm_ring_buffer_write_frames(pf, (const uint8_t *)addr + ret * fb, chunk);
        if (done == 0) break;

        ret += done;
        frames -= done;

        if (card->ops->start && pf->state == SNDRV_PCM_STATE_PREPARED && (snd_pcm_sframes_t)pcm_ring_buffer_avail(pf) >= (snd_pcm_sframes_t)pf->start_threshold) {
            card->ops->start(card);
            pf->state = SNDRV_PCM_STATE_RUNNING;
        }

        if (pf->period_event) pf->period_event = 0;
    }
    spin_unlock(&pf->lock);
    wait_queue_wake_all(&pf->read_wait);
    return (int64_t)(ret * fb);
}

/* Poll the PCM file for ready events. */
int audio_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;
    audio_pcm_file_t *pf      = private_data;
    int               revents = 0;

    if (!pf) return 0;

    switch (pf->type) {
        case audio_node_pcm_playback : {
            snd_pcm_sframes_t space = pcm_ring_buffer_space(pf);
            if (space > 0) revents |= POLLOUT;
            break;
        }
        case audio_node_pcm_capture : {
            snd_pcm_sframes_t avail = pcm_ring_buffer_avail(pf);
            if (avail > 0) revents |= POLLIN;
            break;
        }
        case audio_node_control :
        case audio_node_mixer :
            revents |= POLLIN | POLLOUT;
            break;
    }

    return revents & (int)events;
}

/* ALSA-compatible ioctl handler */
/* SNDRV_PCM_IOCTL_HW_PARAMS: validate and apply the hardware format. */
static int audio_hw_params_ioctl(audio_pcm_file_t *pf, struct snd_pcm_hw_params *uarg)
{
    struct snd_pcm_hw_params params;
    audio_pcm_format_t       fmt = pf->fmt;

    if (!uarg) return -EFAULT;
    if (copy_from_user(&params, uarg, sizeof(params))) return -EFAULT;

    switch (params.format) {
        case SNDRV_PCM_FORMAT_U8 :
            fmt.bits = 8;
            break;
        case SNDRV_PCM_FORMAT_S16_LE :
            fmt.bits = 16;
            break;
        default :
            return -EINVAL;
    }
    fmt.channels = (uint8_t)(params.channels & 0xff);
    if (fmt.channels < 1 || fmt.channels > 8) return -EINVAL;

    fmt.sample_rate = params.rate;
    if (fmt.sample_rate < 8000 || fmt.sample_rate > 192000) return -EINVAL;

    if (pf->card->ops->set_format) {
        int r = pf->card->ops->set_format(pf->card, &fmt);
        if (r != EOK) return r;
    }

    pf->fmt = fmt;

    if (pf->ring_buf) pcm_ring_buffer_destroy(pf);

    size_t fb         = frame_bytes(&fmt);
    size_t buf_frames = params.buffer_size;
    if (buf_frames == 0) buf_frames = (size_t)params.period_size * params.periods;
    if (buf_frames == 0) buf_frames = 4096;

    pf->period_size     = params.period_size;
    pf->avail_min       = params.period_size;
    pf->start_threshold = pf->period_size * 2;
    pf->period_bytes    = pf->period_size * fb;

    int r = pcm_ring_buffer_init(pf, buf_frames);
    if (r != EOK) return r;

    if (pf->card->ops->set_params) pf->card->ops->set_params(pf->card, &fmt, buf_frames * fb, pf->period_bytes);

    params.buffer_size  = (unsigned int)pf->boundary;
    params.period_size  = (unsigned int)pf->period_size;
    params.channels     = fmt.channels;
    params.rate         = fmt.sample_rate;
    params.format       = (fmt.bits == 16) ? SNDRV_PCM_FORMAT_S16_LE : SNDRV_PCM_FORMAT_U8;
    params.access       = SNDRV_PCM_ACCESS_RW_INTERLEAVED;
    params.sample_bits  = fmt.bits;
    params.frame_bits   = (unsigned int)(fb * 8);
    params.period_bytes = (unsigned int)pf->period_bytes;
    params.periods      = (unsigned int)(buf_frames / pf->period_size);

    if (pf->type == audio_node_pcm_capture) {
        pf->state = SNDRV_PCM_STATE_PREPARED;
    } else {
        pf->state = SNDRV_PCM_STATE_SETUP;
    }

    return copy_to_user(uarg, &params, sizeof(params)) ? -EFAULT : EOK;
}

/* SNDRV_PCM_IOCTL_SW_PARAMS: apply the software (threshold) parameters. */
static int audio_sw_params_ioctl(audio_pcm_file_t *pf, void *uarg)
{
    struct snd_pcm_sw_params sp;
    if (!uarg) return -EFAULT;
    if (copy_from_user(&sp, uarg, sizeof(sp))) return -EFAULT;

    pf->avail_min       = sp.avail_min;
    pf->start_threshold = sp.start_threshold;
    if (pf->start_threshold > pf->boundary) pf->start_threshold = pf->boundary / 2;

    return EOK;
}

/* SNDRV_PCM_IOCTL_STATUS: report the current stream state and pointers. */
static int audio_status_ioctl(audio_pcm_file_t *pf, void *uarg)
{
    struct snd_pcm_status st;
    memset(&st, 0, sizeof(st));

    st.state     = (snd_pcm_state_t)pf->state;
    st.appl_ptr  = pf->appl_ptr;
    st.hw_ptr    = pf->hw_ptr;
    st.avail     = (snd_pcm_uframes_t)pcm_ring_buffer_avail(pf);
    st.delay     = 0;
    st.avail_max = st.avail;

    return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : EOK;
}

/* SNDRV_PCM_IOCTL_SYNC_PTR: return the status snapshot, if requested. */
static int audio_sync_ptr_ioctl(audio_pcm_file_t *pf, void *uarg)
{
    struct snd_pcm_sync_ptr sp;
    if (!uarg) return -EFAULT;
    if (copy_from_user(&sp, uarg, sizeof(sp))) return -EFAULT;

    if (sp.status) {
        struct snd_pcm_status st;
        memset(&st, 0, sizeof(st));
        st.state    = (snd_pcm_state_t)pf->state;
        st.appl_ptr = pf->appl_ptr;
        st.hw_ptr   = pf->hw_ptr;
        st.avail    = (snd_pcm_uframes_t)pcm_ring_buffer_avail(pf);
        copy_to_user((void *)sp.status, &st, sizeof(st));
    }

    return EOK;
}

/* file_ioctl (ALSA + legacy) */
int audio_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    (void)flags;
    (void)ctx;
    audio_pcm_file_t *pf = private_data;
    audio_card_t     *card;
    int               status;

    if (!pf || !pf->card) return -ENODEV;
    card = pf->card;

    switch (req) {
        case SNDRV_PCM_IOCTL_PVERSION : {
            unsigned int ver = (2 << 16) | (0 << 8) | 14;
            if (copy_to_user(arg, &ver, sizeof(ver))) return -EFAULT;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_INFO : {
            struct snd_pcm_info info;
            memset(&info, 0, sizeof(info));
            info.card   = card->id;
            info.device = 0;
            info.stream = (pf->type == audio_node_pcm_capture) ? 1 : 0;
            strncpy((char *)info.name, card->name, sizeof(info.name) - 1);
            if (copy_to_user(arg, &info, sizeof(info))) return -EFAULT;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_HW_PARAMS :
            return audio_hw_params_ioctl(pf, arg);
        case SNDRV_PCM_IOCTL_HW_FREE :
            pcm_ring_buffer_destroy(pf);
            pf->state = SNDRV_PCM_STATE_OPEN;
            return EOK;
        case SNDRV_PCM_IOCTL_SW_PARAMS :
            return audio_sw_params_ioctl(pf, arg);
        case SNDRV_PCM_IOCTL_STATUS :
            return audio_status_ioctl(pf, arg);
        case SNDRV_PCM_IOCTL_PREPARE :
            pf->state    = SNDRV_PCM_STATE_PREPARED;
            pf->appl_ptr = 0;
            pf->hw_ptr   = 0;
            pf->xrun     = 0;
            return EOK;
        case SNDRV_PCM_IOCTL_START :
            if (pf->state == SNDRV_PCM_STATE_PREPARED) {
                if (card->ops->start) card->ops->start(card);
                pf->state = SNDRV_PCM_STATE_RUNNING;
            }
            return EOK;
        case SNDRV_PCM_IOCTL_DROP :
            if (card->ops->stop) card->ops->stop(card);
            pf->state    = SNDRV_PCM_STATE_SETUP;
            pf->appl_ptr = 0;
            pf->hw_ptr   = 0;
            wait_queue_wake_all(&pf->write_wait);
            return EOK;
        case SNDRV_PCM_IOCTL_DRAIN :
            if (pf->state == SNDRV_PCM_STATE_RUNNING) {
                pf->state = SNDRV_PCM_STATE_DRAINING;
                if (card->ops->drain) card->ops->drain(card);
            }
            return EOK;
        case SNDRV_PCM_IOCTL_PAUSE : {
            int pause;
            if (copy_from_user(&pause, arg, sizeof(pause))) return -EFAULT;
            if (pause) {
                if (card->ops->stop) card->ops->stop(card);
                pf->state = SNDRV_PCM_STATE_PAUSED;
            } else {
                if (card->ops->start) card->ops->start(card);
                pf->state = SNDRV_PCM_STATE_RUNNING;
            }
            return EOK;
        }
        case SNDRV_PCM_IOCTL_HWSYNC :
        case SNDRV_PCM_IOCTL_RESET :
            return EOK;
        case SNDRV_PCM_IOCTL_SYNC_PTR :
            return audio_sync_ptr_ioctl(pf, arg);
        case SNDRV_PCM_IOCTL_DELAY : {
            snd_pcm_sframes_t delay = 0;
            if (copy_to_user(arg, &delay, sizeof(delay))) return -EFAULT;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_REWIND : {
            snd_pcm_uframes_t frames;
            if (copy_from_user(&frames, arg, sizeof(frames))) return -EFAULT;
            pf->appl_ptr = (pf->appl_ptr >= frames) ? pf->appl_ptr - frames : 0;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_FORWARD : {
            snd_pcm_uframes_t frames;
            if (copy_from_user(&frames, arg, sizeof(frames))) return -EFAULT;
            pf->appl_ptr = (pf->appl_ptr + frames) % pf->boundary;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_WRITEI_FRAMES : {
            struct snd_xferi xf;
            if (copy_from_user(&xf, arg, sizeof(xf))) return -EFAULT;
            xf.result = (snd_pcm_sframes_t)(audio_file_write(ctx, pf, flags, xf.buf, 0, xf.frames * frame_bytes(&pf->fmt)) / frame_bytes(&pf->fmt));
            if (copy_to_user(arg, &xf, sizeof(xf))) return -EFAULT;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_READI_FRAMES : {
            struct snd_xferi xf;
            if (copy_from_user(&xf, arg, sizeof(xf))) return -EFAULT;
            xf.result = (snd_pcm_sframes_t)(audio_file_read(ctx, pf, flags, xf.buf, 0, xf.frames * frame_bytes(&pf->fmt)) / frame_bytes(&pf->fmt));
            if (copy_to_user(arg, &xf, sizeof(xf))) return -EFAULT;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_LINK :
        case SNDRV_PCM_IOCTL_UNLINK :
            return EOK;
        default :
            break;
    }

    /* Legacy ioctls */
    switch (req) {
        case AUDIO_IOCTL_GET_INFO : {
            audio_card_info_t info;
            audio_fill_info(card, &info);
            if (copy_to_user(arg, &info, sizeof(info))) return -EFAULT;
            return EOK;
        }
        case AUDIO_IOCTL_SET_FORMAT : {
            audio_pcm_format_t fmt;
            if (copy_from_user(&fmt, arg, sizeof(fmt))) return -EFAULT;
            if (card->ops->set_format) {
                status = card->ops->set_format(card, &fmt);
                if (status != EOK) return status;
            }
            pf->fmt = fmt;
            if (pf->ring_buf) pcm_ring_buffer_destroy(pf);
            pf->period_size     = 1024;
            pf->start_threshold = 2048;
            pf->avail_min       = 1024;
            pf->period_bytes    = pf->period_size * frame_bytes(&fmt);
            status              = pcm_ring_buffer_init(pf, 16384);
            if (card->ops->set_params) card->ops->set_params(card, &fmt, 16384 * frame_bytes(&fmt), pf->period_bytes);
            return status;
        }
        case AUDIO_IOCTL_STOP :
            if (card->ops->stop) card->ops->stop(card);
            pf->state = SNDRV_PCM_STATE_SETUP;
            return EOK;
        case AUDIO_IOCTL_START :
            if (pf->type != audio_node_pcm_playback) return -EINVAL;
            if (pf->state == SNDRV_PCM_STATE_PREPARED || pf->state == SNDRV_PCM_STATE_SETUP) {
                if (card->ops->start) card->ops->start(card);
                pf->state = SNDRV_PCM_STATE_RUNNING;
            }
            return EOK;
        case AUDIO_IOCTL_DRAIN :
            if (pf->state == SNDRV_PCM_STATE_RUNNING) {
                pf->state = SNDRV_PCM_STATE_DRAINING;
                if (card->ops->drain) card->ops->drain(card);
            }
            return EOK;
        case AUDIO_IOCTL_SET_VOLUME : {
            audio_volume_t vol;
            if (copy_from_user(&vol, arg, sizeof(vol))) return -EFAULT;
            card->volume = vol;
            if (card->ops->set_volume) return card->ops->set_volume(card, &vol);
            return EOK;
        }
        case AUDIO_IOCTL_GET_VOLUME : {
            audio_volume_t vol = card->volume;
            if (card->ops->get_volume) card->ops->get_volume(card, &vol);
            if (copy_to_user(arg, &vol, sizeof(vol))) return -EFAULT;
            return EOK;
        }
        case AUDIO_IOCTL_GET_POS : {
            snd_pcm_uframes_t pos = pf->hw_ptr;
            if (card->ops->get_position) card->ops->get_position(card, &pos);
            if (copy_to_user(arg, &pos, sizeof(pos))) return -EFAULT;
            return EOK;
        }
        case AUDIO_IOCTL_SET_PARAMS : {
            struct {
                    audio_pcm_format_t fmt;
                    size_t             buf_bytes;
                    size_t             per_bytes;
            } p;
            if (copy_from_user(&p, arg, sizeof(p))) return -EFAULT;
            if (card->ops->set_format) {
                status = card->ops->set_format(card, &p.fmt);
                if (status != EOK) return status;
            }
            pf->fmt = p.fmt;
            if (pf->ring_buf) pcm_ring_buffer_destroy(pf);
            pf->period_bytes    = p.per_bytes;
            pf->period_size     = p.per_bytes / frame_bytes(&p.fmt);
            pf->start_threshold = pf->period_size * 2;
            pf->avail_min       = pf->period_size;
            status              = pcm_ring_buffer_init(pf, p.buf_bytes / frame_bytes(&p.fmt));
            if (card->ops->set_params) card->ops->set_params(card, &p.fmt, p.buf_bytes, p.per_bytes);
            return status;
        }
        default :
            return -EINVAL;
    }
}

/* Legacy device callbacks (backward compat) */

/* Read device-node data through a transient PCM file. */
size_t audio_device_read(void *ctx, void *addr, size_t offset, size_t size)
{
    audio_device_node_t *node = ctx;
    audio_pcm_file_t    *pf;
    size_t               ret;

    if (!node || !node->card) return 0;

    if (node->type == audio_node_control) {
        audio_card_info_t info;
        audio_fill_info(node->card, &info);
        if (offset >= sizeof(info)) return 0;
        if (offset + size > sizeof(info)) size = sizeof(info) - offset;
        memcpy(addr, ((uint8_t *)&info) + offset, size);
        return size;
    }

    if (node->type == audio_node_mixer) {
        audio_volume_t vol = node->card->volume;
        if (offset >= sizeof(vol)) return 0;
        if (offset + size > sizeof(vol)) size = sizeof(vol) - offset;
        memcpy(addr, ((uint8_t *)&vol) + offset, size);
        return size;
    }

    pf = audio_pcm_create(node);
    if (!pf) return 0;
    pf->state = SNDRV_PCM_STATE_PREPARED;
    if (!pf->ring_buf) {
        pf->period_size     = 1024;
        pf->avail_min       = 1024;
        pf->start_threshold = 2048;
        pf->period_bytes    = pf->period_size * frame_bytes(&pf->fmt);
        pcm_ring_buffer_init(pf, 16384);
    }
    ret = audio_file_read(node, pf, 0, addr, offset, size);
    audio_pcm_destroy(pf);
    return ret;
}

/* Legacy device write: stream frames through a transient PCM file. */
size_t audio_device_write(void *ctx, const void *addr, size_t offset, size_t size)
{
    audio_device_node_t *node = ctx;
    audio_pcm_file_t    *pf;
    size_t               ret;

    if (!node || !node->card) return 0;
    if (node->type != audio_node_pcm_playback) return 0;

    pf = audio_pcm_create(node);
    if (!pf) return 0;
    pf->state = SNDRV_PCM_STATE_PREPARED;
    if (!pf->ring_buf) {
        pf->period_size     = 1024;
        pf->avail_min       = 1024;
        pf->start_threshold = 2048;
        pf->period_bytes    = pf->period_size * frame_bytes(&pf->fmt);
        pcm_ring_buffer_init(pf, 16384);
    }
    ret = audio_file_write(node, pf, 0, addr, offset, size);
    audio_pcm_destroy(pf);
    return ret;
}

/* Legacy device poll: playback is writable, other nodes are always ready. */
int audio_device_poll(void *ctx, size_t events)
{
    audio_device_node_t *node = ctx;
    if (!node) return 0;
    if (node->type == audio_node_pcm_playback) return (int)(events & POLLOUT);
    return (int)(events & (POLLIN | POLLOUT));
}

/* Legacy device ioctl: dispatch through a transient PCM file. */
int audio_device_ioctl(void *ctx, size_t req, void *arg)
{
    audio_device_node_t *node = ctx;
    audio_pcm_file_t    *pf;
    int                  ret;

    if (!node || !node->card) return -ENODEV;

    pf = audio_pcm_create(node);
    if (!pf) return -ENOMEM;

    ret = audio_file_ioctl(node, pf, 0, req, arg);
    audio_pcm_destroy(pf);
    return ret;
}
