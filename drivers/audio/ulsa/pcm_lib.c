/*
 *
 *      pcm_lib.c
 *      ULSA PCM pointer bookkeeping and read/write transfers
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <kernel/debug/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/poll.h>

#if CONFIG_AUDIO

/*
 * Pointer model (as in Linux): hw_ptr and appl_ptr count frames modulo
 * runtime->boundary, a large multiple of buffer_size.  The ring offset of a
 * pointer is ptr % buffer_size.  For playback the application is ahead
 * (appl_ptr - hw_ptr frames are queued); for capture the hardware is ahead.
 * All of this is protected by the substream's stream lock, which interrupt
 * handlers take through ulsa_pcm_period_elapsed().
 */

/* Take the stream lock with interrupts disabled. */
uint64_t ulsa_pcm_stream_lock_irqsave(ulsa_pcm_substream_t *substream)
{
    return spin_lock_irqsave(&substream->lock);
}

/* Release the stream lock. */
void ulsa_pcm_stream_unlock_irqrestore(ulsa_pcm_substream_t *substream, uint64_t flags)
{
    spin_unlock_irqrestore(&substream->lock, flags);
}

/* True while the stream moves data (RUNNING, or a playback DRAINING). */
bool ulsa_pcm_running(const ulsa_pcm_substream_t *substream)
{
    snd_pcm_state_t state = substream->runtime->status->state;
    return state == SNDRV_PCM_STATE_RUNNING || (state == SNDRV_PCM_STATE_DRAINING && substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
}

/* Frames the application may write. */
snd_pcm_uframes_t ulsa_pcm_playback_avail(const ulsa_pcm_runtime_t *runtime)
{
    snd_pcm_sframes_t avail = (snd_pcm_sframes_t)(runtime->status->hw_ptr + runtime->buffer_size - runtime->control->appl_ptr);

    if (avail < 0) {
        avail += (snd_pcm_sframes_t)runtime->boundary;
    } else if ((snd_pcm_uframes_t)avail >= runtime->boundary) {
        avail -= (snd_pcm_sframes_t)runtime->boundary;
    }
    return (snd_pcm_uframes_t)avail;
}

/* Frames the application may read. */
snd_pcm_uframes_t ulsa_pcm_capture_avail(const ulsa_pcm_runtime_t *runtime)
{
    snd_pcm_sframes_t avail = (snd_pcm_sframes_t)(runtime->status->hw_ptr - runtime->control->appl_ptr);

    if (avail < 0) avail += (snd_pcm_sframes_t)runtime->boundary;
    return (snd_pcm_uframes_t)avail;
}

/* Frames available to the application in its direction. */
snd_pcm_uframes_t ulsa_pcm_avail(const ulsa_pcm_substream_t *substream)
{
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) return ulsa_pcm_playback_avail(substream->runtime);
    return ulsa_pcm_capture_avail(substream->runtime);
}

/* Frames owned by the hardware side (queued for playback, or free for capture). */
snd_pcm_sframes_t ulsa_pcm_hw_avail(const ulsa_pcm_substream_t *substream)
{
    return (snd_pcm_sframes_t)substream->runtime->buffer_size - (snd_pcm_sframes_t)ulsa_pcm_avail(substream);
}

/* Current time on the clock selected with TTSTAMP. */
void ulsa_pcm_gettime(const ulsa_pcm_runtime_t *runtime, struct snd_timespec *ts)
{
    uint64_t ns;

    if (runtime->tstamp_type == SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY) {
        int64_t rt = timer_realtime_ns();
        ns         = rt > 0 ? (uint64_t)rt : 0;
    } else {
        ns = timer_monotonic_ns();
    }
    ts->tv_sec  = (int64_t)(ns / 1000000000ULL);
    ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
}

/* Wake blocked transfers, drain waiters and poll()/epoll() watchers. */
void ulsa_pcm_wake_sleepers(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;

    wait_queue_wake_all(&runtime->sleep);
    wait_queue_wake_all(&runtime->tsleep);
    vfs_poll_source_notify(&runtime->poll_source, POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM | POLLERR);
}

/* Fill `frames` frames of silence starting at ring offset `off`. */
static void fill_silence_frames(ulsa_pcm_substream_t *substream, snd_pcm_uframes_t off, snd_pcm_uframes_t frames)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;

    if (!runtime->dma_area) return;
    if (runtime->access == SNDRV_PCM_ACCESS_RW_INTERLEAVED || runtime->access == SNDRV_PCM_ACCESS_MMAP_INTERLEAVED) {
        (void)ulsa_pcm_format_set_silence(runtime->format, runtime->dma_area + ulsa_frames_to_bytes(runtime, (snd_pcm_sframes_t)off), frames * runtime->channels);
    } else {
        size_t channel_bytes = runtime->dma_bytes / runtime->channels;
        for (unsigned int c = 0; c < runtime->channels; c++) {
            uint8_t *base = runtime->dma_area + c * channel_bytes + off * runtime->sample_bits / 8;
            (void)ulsa_pcm_format_set_silence(runtime->format, base, frames);
        }
    }
}

/* Advance the silence bookkeeping from ptr to new_ptr. */
static void update_silence_vars(ulsa_pcm_runtime_t *runtime, snd_pcm_uframes_t ptr, snd_pcm_uframes_t new_ptr)
{
    snd_pcm_sframes_t delta = (snd_pcm_sframes_t)(new_ptr - ptr);

    if (delta == 0) return;
    if (delta < 0) delta += (snd_pcm_sframes_t)runtime->boundary;
    if ((snd_pcm_uframes_t)delta < runtime->silence_filled) {
        runtime->silence_filled -= (snd_pcm_uframes_t)delta;
    } else {
        runtime->silence_filled = 0;
    }
    runtime->silence_start = new_ptr;
}

/*
 * Playback silence: either keep `silence_threshold` frames of silence ahead of
 * the application (threshold mode), or, when silence_size >= boundary, wipe
 * every period the hardware has played so an underrun replays silence instead
 * of stale audio.  new_hw_ptr == (snd_pcm_uframes_t)-1 means "initialise".
 */
void ulsa_pcm_playback_silence(ulsa_pcm_substream_t *substream, snd_pcm_uframes_t new_hw_ptr)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    snd_pcm_uframes_t   frames, ofs, transfer;

    if (runtime->silence_size < runtime->boundary) {
        snd_pcm_sframes_t noise_dist;
        snd_pcm_uframes_t appl_ptr = runtime->control->appl_ptr;

        update_silence_vars(runtime, runtime->silence_start, appl_ptr);
        if (new_hw_ptr == (snd_pcm_uframes_t)-1) new_hw_ptr = runtime->status->hw_ptr;
        noise_dist = (snd_pcm_sframes_t)(appl_ptr - new_hw_ptr);
        if (noise_dist < 0) noise_dist += (snd_pcm_sframes_t)runtime->boundary;
        noise_dist += (snd_pcm_sframes_t)runtime->silence_filled;
        if (noise_dist >= (snd_pcm_sframes_t)runtime->silence_threshold) return;
        frames = runtime->silence_threshold - (snd_pcm_uframes_t)noise_dist;
        if (frames > runtime->silence_size) frames = runtime->silence_size;
    } else {
        snd_pcm_uframes_t hw_ptr = runtime->status->hw_ptr;

        if (new_hw_ptr == (snd_pcm_uframes_t)-1) {
            snd_pcm_sframes_t avail = (snd_pcm_sframes_t)(runtime->control->appl_ptr - hw_ptr);
            if (avail < 0) avail += (snd_pcm_sframes_t)runtime->boundary;
            runtime->silence_filled = (snd_pcm_uframes_t)avail > runtime->buffer_size ? 0 : (snd_pcm_uframes_t)avail;
            runtime->silence_start  = hw_ptr;
        } else {
            update_silence_vars(runtime, hw_ptr, new_hw_ptr);
        }
        frames = runtime->buffer_size - runtime->silence_filled;
    }
    if (frames > runtime->buffer_size || frames == 0) return;

    ofs = (runtime->silence_start + runtime->silence_filled) % runtime->buffer_size;
    do {
        transfer = ofs + frames > runtime->buffer_size ? runtime->buffer_size - ofs : frames;
        fill_silence_frames(substream, ofs, transfer);
        runtime->silence_filled += transfer;
        frames -= transfer;
        ofs = 0;
    } while (frames > 0);
}

/* Put a running stream into XRUN. */
void ulsa_pcm_xrun(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;

    if (runtime->tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE) {
        struct snd_timespec ts;
        ulsa_pcm_gettime(runtime, &ts);
        runtime->status->tstamp = ts;
    }
    (void)ulsa_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
}

/*
 * Re-evaluate the state after the pointers moved: finish a drain once the
 * buffer is empty, declare an xrun at stop_threshold, and wake whoever waits
 * for avail_min frames.
 */
int ulsa_pcm_update_state(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    snd_pcm_uframes_t   avail   = ulsa_pcm_avail(substream);

    if (avail > runtime->avail_max) runtime->avail_max = avail;
    if (runtime->status->state == SNDRV_PCM_STATE_DRAINING) {
        if (avail >= runtime->buffer_size) {
            (void)ulsa_pcm_drain_done(substream);
            return -EPIPE;
        }
    } else if (avail >= runtime->stop_threshold) {
        ulsa_pcm_xrun(substream);
        return -EPIPE;
    }
    if (runtime->twake) {
        if (avail >= runtime->twake) wait_queue_wake_all(&runtime->tsleep);
    } else if (avail >= runtime->control->avail_min) {
        wait_queue_wake_all(&runtime->sleep);
    }
    if (avail >= runtime->control->avail_min) vfs_poll_source_notify(&runtime->poll_source, POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
    return EOK;
}

/*
 * Read the hardware position and fold it into hw_ptr.  The position only
 * tells where in the ring the hardware is, so wrap-arounds have to be inferred:
 * a position behind the previous one means the ring wrapped; and when called
 * from the period interrupt, a position that did not advance by at least one
 * period after more than half a buffer of wall-clock time means a whole
 * buffer went by.
 */
static int update_hw_ptr0(ulsa_pcm_substream_t *substream, bool in_interrupt)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    snd_pcm_uframes_t   pos, old_hw_ptr, new_hw_ptr, hw_base;
    snd_pcm_sframes_t   delta;
    uint64_t            now           = timer_monotonic_ns();
    bool                crossed_bound = false;

    old_hw_ptr = runtime->status->hw_ptr;
    pos        = substream->ops->pointer(substream);
    if (pos == ULSA_PCM_POS_XRUN) {
        ulsa_pcm_xrun(substream);
        return -EPIPE;
    }
    if (pos >= runtime->buffer_size) pos = 0;
    pos -= pos % runtime->min_align;
    hw_base    = runtime->hw_ptr_base;
    new_hw_ptr = hw_base + pos;

    if (in_interrupt) {
        snd_pcm_uframes_t expected = runtime->hw_ptr_interrupt + runtime->period_size;
        if (expected > new_hw_ptr && now - runtime->hw_ptr_ns > runtime->hw_ptr_buffer_ns / 2 + 1000000ULL) {
            hw_base += runtime->buffer_size;
            if (hw_base >= runtime->boundary) {
                hw_base       = 0;
                crossed_bound = true;
            }
            new_hw_ptr = hw_base + pos;
            goto delta;
        }
    }
    if (new_hw_ptr < old_hw_ptr) {
        hw_base += runtime->buffer_size;
        if (hw_base >= runtime->boundary) {
            hw_base       = 0;
            crossed_bound = true;
        }
        new_hw_ptr = hw_base + pos;
    }
delta:
    delta = (snd_pcm_sframes_t)(new_hw_ptr - old_hw_ptr);
    if (delta < 0) delta += (snd_pcm_sframes_t)runtime->boundary;

    /* A jump beyond a buffer and a period cannot be explained by the hardware; ignore it. */
    if (!runtime->no_period_wakeup && (snd_pcm_uframes_t)delta >= runtime->buffer_size + runtime->period_size) return EOK;

    if (runtime->status->hw_ptr == new_hw_ptr) {
        runtime->hw_ptr_ns = now;
        return EOK;
    }
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && runtime->silence_size > 0) ulsa_pcm_playback_silence(substream, new_hw_ptr);

    if (in_interrupt) {
        delta = (snd_pcm_sframes_t)(new_hw_ptr - runtime->hw_ptr_interrupt);
        if (delta < 0) delta += (snd_pcm_sframes_t)runtime->boundary;
        delta -= (snd_pcm_sframes_t)((snd_pcm_uframes_t)delta % runtime->period_size);
        runtime->hw_ptr_interrupt += (snd_pcm_uframes_t)delta;
        if (runtime->hw_ptr_interrupt >= runtime->boundary) runtime->hw_ptr_interrupt -= runtime->boundary;
    }
    runtime->hw_ptr_base    = hw_base;
    runtime->status->hw_ptr = new_hw_ptr;
    runtime->hw_ptr_ns      = now;
    if (crossed_bound) runtime->hw_ptr_wrap += runtime->boundary;

    if (runtime->tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE) {
        struct snd_timespec ts;
        uint64_t            frames = runtime->hw_ptr_wrap + runtime->status->hw_ptr;
        uint64_t            ns     = runtime->rate ? frames * 1000000000ULL / runtime->rate : 0;

        ulsa_pcm_gettime(runtime, &ts);
        runtime->status->tstamp               = ts;
        runtime->status->audio_tstamp.tv_sec  = (int64_t)(ns / 1000000000ULL);
        runtime->status->audio_tstamp.tv_nsec = (int64_t)(ns % 1000000000ULL);
        runtime->driver_tstamp                = ts;
    }
    return ulsa_pcm_update_state(substream);
}

/* Update hw_ptr outside the interrupt path; stream lock held. */
int ulsa_pcm_update_hw_ptr(ulsa_pcm_substream_t *substream)
{
    return update_hw_ptr0(substream, false);
}

/* Period interrupt with the stream lock already held. */
void ulsa_pcm_period_elapsed_under_stream_lock(ulsa_pcm_substream_t *substream)
{
    if (!substream || !substream->runtime) return;
    if (!ulsa_pcm_running(substream)) return;
    if (update_hw_ptr0(substream, true) < 0) return;
    if (substream->timer_running) ulsa_timer_pcm_interrupt(substream);
}

/* Period interrupt entry point for drivers. */
void ulsa_pcm_period_elapsed(ulsa_pcm_substream_t *substream)
{
    uint64_t flags;

    if (!substream) return;
    flags = spin_lock_irqsave(&substream->lock);
    ulsa_pcm_period_elapsed_under_stream_lock(substream);
    spin_unlock_irqrestore(&substream->lock, flags);
}

/* Move appl_ptr and tell the driver (ops->ack); stream lock held. */
int ulsa_pcm_apply_appl_ptr(ulsa_pcm_substream_t *substream, snd_pcm_uframes_t appl_ptr)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    snd_pcm_uframes_t   old     = runtime->control->appl_ptr;

    if (old == appl_ptr) return EOK;
    if (appl_ptr >= runtime->boundary) return -EINVAL;
    if (runtime->info & SNDRV_PCM_INFO_NO_REWINDS) {
        snd_pcm_sframes_t diff = (snd_pcm_sframes_t)(appl_ptr - old);
        if (diff >= 0) {
            if ((snd_pcm_uframes_t)diff > runtime->buffer_size) return -EINVAL;
        } else if (runtime->boundary + (snd_pcm_uframes_t)diff > runtime->buffer_size) {
            return -EINVAL;
        }
    }
    runtime->control->appl_ptr = appl_ptr;
    if (substream->ops->ack) {
        int ret = substream->ops->ack(substream);
        if (ret < 0) {
            runtime->control->appl_ptr = old;
            if (ret == -EPIPE) ulsa_pcm_xrun(substream);
            return ret;
        }
    }
    return EOK;
}

/* Whether the current state allows data transfers. */
static int pcm_accessible_state(const ulsa_pcm_substream_t *substream)
{
    switch (substream->runtime->status->state) {
        case SNDRV_PCM_STATE_PREPARED :
        case SNDRV_PCM_STATE_RUNNING :
        case SNDRV_PCM_STATE_PAUSED :
            return EOK;
        case SNDRV_PCM_STATE_DRAINING :
            return substream->stream == SNDRV_PCM_STREAM_CAPTURE ? EOK : -EBADFD;
        case SNDRV_PCM_STATE_XRUN :
            return -EPIPE;
        case SNDRV_PCM_STATE_SUSPENDED :
            return -ESTRPIPE;
        default :
            return -EBADFD;
    }
}

/*
 * Sleep until at least twake frames are available, the state changes, a
 * signal arrives, or the hardware stalls for longer than the buffer (plus a
 * margin) should take.  Called and returns with the stream lock held.
 */
static int wait_for_avail(ulsa_pcm_substream_t *substream, snd_pcm_uframes_t *availp, uint64_t *flags)
{
    ulsa_pcm_runtime_t *runtime     = substream->runtime;
    bool                is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
    uint64_t            wait_ms     = 1000;
    snd_pcm_uframes_t   avail       = 0;
    int                 err         = 0;

    if (runtime->rate) {
        uint64_t t = (uint64_t)runtime->buffer_size * 2000 / runtime->rate;
        if (t > wait_ms) wait_ms = t;
    }

    for (;;) {
        uint64_t deadline;
        int      tout;

        if (signal_has_pending_current()) {
            err = -ERESTARTSYS;
            break;
        }
        avail = ulsa_pcm_avail(substream);
        if (avail >= runtime->twake) break;

        deadline = sched_ticks() + wait_ms * CONFIG_TIMER_HZ / 1000 + 1;
        wait_queue_prepare(&runtime->tsleep);
        spin_unlock_irqrestore(&substream->lock, *flags);
        if (runtime->no_period_wakeup) {
            wait_queue_sleep();
            tout = 0;
        } else {
            tout = wait_queue_wait_timed(&runtime->tsleep, deadline);
        }
        *flags = spin_lock_irqsave(&substream->lock);

        switch (runtime->status->state) {
            case SNDRV_PCM_STATE_SUSPENDED :
                err = -ESTRPIPE;
                goto out;
            case SNDRV_PCM_STATE_XRUN :
                err = -EPIPE;
                goto out;
            case SNDRV_PCM_STATE_DRAINING :
                if (is_playback) {
                    err = -EPIPE;
                } else {
                    avail = ulsa_pcm_avail(substream);
                }
                goto out;
            case SNDRV_PCM_STATE_OPEN :
            case SNDRV_PCM_STATE_SETUP :
            case SNDRV_PCM_STATE_DISCONNECTED :
                err = -EBADFD;
                goto out;
            case SNDRV_PCM_STATE_PAUSED :
                continue;
            default :
                break;
        }
        if (signal_has_pending_current()) {
            err = -ERESTARTSYS;
            break;
        }
        if (tout == -ETIMEDOUT && runtime->status->state == SNDRV_PCM_STATE_RUNNING) {
            /* Re-check: the hardware may have advanced without interrupting us. */
            (void)ulsa_pcm_update_hw_ptr(substream);
            if (ulsa_pcm_avail(substream) >= runtime->twake) continue;
            plogk("ulsa: %s timeout on card%d pcm%d (DMA or IRQ trouble?)\n", is_playback ? "playback write" : "capture read", substream->pcm->card->number, substream->pcm->device);
            err = -EIO;
            break;
        }
    }
out:
    *availp = avail;
    return err;
}

/* Copy one contiguous chunk between user memory and the ring. */
static int transfer_chunk(ulsa_pcm_substream_t *substream, void *const *ubufs, bool interleaved, snd_pcm_uframes_t hwoff, snd_pcm_uframes_t user_off, snd_pcm_uframes_t frames)
{
    ulsa_pcm_runtime_t *runtime     = substream->runtime;
    bool                is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;

    if (interleaved) {
        uint8_t *dma   = runtime->dma_area + ulsa_frames_to_bytes(runtime, (snd_pcm_sframes_t)hwoff);
        size_t   bytes = ulsa_frames_to_bytes(runtime, (snd_pcm_sframes_t)frames);
        uint8_t *user  = (uint8_t *)ubufs[0];

        if (!user) {
            if (!is_playback) return -EINVAL;
            fill_silence_frames(substream, hwoff, frames);
            return EOK;
        }
        user += ulsa_frames_to_bytes(runtime, (snd_pcm_sframes_t)user_off);
        if (is_playback) return copy_from_user(dma, user, bytes) ? -EFAULT : EOK;
        return copy_to_user(user, dma, bytes) ? -EFAULT : EOK;
    }

    {
        size_t channel_bytes = runtime->dma_bytes / runtime->channels;
        size_t sample_bytes  = runtime->sample_bits / 8;

        for (unsigned int c = 0; c < runtime->channels; c++) {
            uint8_t *dma  = runtime->dma_area + c * channel_bytes + hwoff * sample_bytes;
            uint8_t *user = (uint8_t *)ubufs[c];

            if (!user) {
                if (!is_playback) continue;
                (void)ulsa_pcm_format_set_silence(runtime->format, dma, frames);
                continue;
            }
            user += user_off * sample_bytes;
            if (is_playback) {
                if (copy_from_user(dma, user, frames * sample_bytes)) return -EFAULT;
            } else if (copy_to_user(user, dma, frames * sample_bytes)) {
                return -EFAULT;
            }
        }
    }
    return EOK;
}

/*
 * The common read/write loop behind read(), write(), READI/WRITEI and
 * READN/WRITEN.  ubufs holds one user pointer (interleaved) or one per
 * channel.  Returns the frames transferred, or an error if none were.
 */
snd_pcm_sframes_t ulsa_pcm_lib_xfer(ulsa_pcm_substream_t *substream, void *const *ubufs, bool interleaved, snd_pcm_uframes_t size, bool nonblock)
{
    ulsa_pcm_runtime_t *runtime     = substream->runtime;
    bool                is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
    snd_pcm_uframes_t   xfer = 0, offset = 0, avail;
    uint64_t            flags;
    int                 err;

    if (runtime->status->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (!runtime->dma_area) return -EINVAL;
    if (interleaved) {
        if (runtime->access != SNDRV_PCM_ACCESS_RW_INTERLEAVED && runtime->channels > 1) return -EINVAL;
    } else if (runtime->access != SNDRV_PCM_ACCESS_RW_NONINTERLEAVED) {
        return -EINVAL;
    }
    if (size == 0) return 0;

    flags = spin_lock_irqsave(&substream->lock);
    err   = pcm_accessible_state(substream);
    if (err < 0) goto out;

    runtime->twake = runtime->control->avail_min ? runtime->control->avail_min : 1;
    if (runtime->status->state == SNDRV_PCM_STATE_RUNNING) (void)ulsa_pcm_update_hw_ptr(substream);

    /* A capture stream starts as soon as the reader asks for at least start_threshold frames. */
    if (!is_playback && runtime->status->state == SNDRV_PCM_STATE_PREPARED && size >= runtime->start_threshold) {
        err = ulsa_pcm_start(substream);
        if (err < 0) goto out;
    }

    avail = ulsa_pcm_avail(substream);
    while (size > 0) {
        snd_pcm_uframes_t frames, appl_ptr, appl_ofs, cont;

        if (!avail) {
            if (!is_playback && runtime->status->state == SNDRV_PCM_STATE_DRAINING) {
                (void)ulsa_pcm_drain_done(substream);
                goto out;
            }
            if (nonblock) {
                err = -EAGAIN;
                goto out;
            }
            snd_pcm_uframes_t avail_min = runtime->control->avail_min ? runtime->control->avail_min : 1;
            runtime->twake              = size < avail_min ? size : avail_min;
            err                         = wait_for_avail(substream, &avail, &flags);
            if (err < 0) goto out;
            if (!avail) continue;
        }
        frames   = size > avail ? avail : size;
        appl_ptr = runtime->control->appl_ptr;
        appl_ofs = appl_ptr % runtime->buffer_size;
        cont     = runtime->buffer_size - appl_ofs;
        if (frames > cont) frames = cont;

        if (runtime->buffer_accessing < 0) {
            err = -EBUSY;
            goto out;
        }
        runtime->buffer_accessing++;
        spin_unlock_irqrestore(&substream->lock, flags);
        err   = transfer_chunk(substream, ubufs, interleaved, appl_ofs, offset, frames);
        flags = spin_lock_irqsave(&substream->lock);
        runtime->buffer_accessing--;
        if (err < 0) goto out;

        err = pcm_accessible_state(substream);
        if (err < 0) goto out;
        appl_ptr += frames;
        if (appl_ptr >= runtime->boundary) appl_ptr -= runtime->boundary;
        err = ulsa_pcm_apply_appl_ptr(substream, appl_ptr);
        if (err < 0) goto out;

        offset += frames;
        size -= frames;
        xfer += frames;
        avail -= frames;
        if (is_playback && runtime->status->state == SNDRV_PCM_STATE_PREPARED && ulsa_pcm_hw_avail(substream) >= (snd_pcm_sframes_t)runtime->start_threshold) {
            err = ulsa_pcm_start(substream);
            if (err < 0) goto out;
        }
    }
out:
    runtime->twake = 0;
    if (xfer > 0 && err >= 0) (void)ulsa_pcm_update_state(substream);
    spin_unlock_irqrestore(&substream->lock, flags);
    return xfer > 0 ? (snd_pcm_sframes_t)xfer : err;
}

#endif
