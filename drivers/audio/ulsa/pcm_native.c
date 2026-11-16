/*
 *
 *      pcm_native.c
 *      ULSA PCM character device: open/release, state machine and ioctls
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <kernel/debug/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <mem/page.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/fcntl.h>
#include <syscall/poll.h>

#if CONFIG_AUDIO

/*
 * State machine (Linux sound/core/pcm_native.c):
 *
 *   OPEN --hw_params--> SETUP --prepare--> PREPARED --start--> RUNNING
 *   RUNNING --drop--> SETUP, --drain--> DRAINING --(empty)--> SETUP,
 *   RUNNING --pause--> PAUSED, RUNNING --(xrun)--> XRUN --prepare--> PREPARED
 *
 * Transitions run under the stream lock and call ops->trigger with it held,
 * exactly as Linux does for atomic PCMs; drivers whose trigger needs to sleep
 * defer the work to a thread.  hw_params, hw_free and prepare may sleep and
 * are serialised by substream->buffer_mutex instead.
 */

/* Set the stream state; stream lock held. */
static void set_state(ulsa_pcm_runtime_t *runtime, snd_pcm_state_t state)
{
    runtime->status->state = state;
}

/* Latch the trigger timestamp. */
static void trigger_tstamp(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_gettime(substream->runtime, &substream->runtime->trigger_tstamp);
}

/* ---------------------------------------------------------- actions */

/* Start a PREPARED stream; stream lock held. */
int ulsa_pcm_start(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    int                 err;

    if (runtime->status->state != SNDRV_PCM_STATE_PREPARED) return -EBADFD;
    if (runtime->buffer_accessing < 0) return -EBADFD;
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && ulsa_pcm_hw_avail(substream) <= 0) return -EPIPE;

    err = substream->ops->trigger(substream, ULSA_PCM_TRIGGER_START);
    if (err < 0) {
        if (err == -EPIPE) set_state(runtime, SNDRV_PCM_STATE_XRUN);
        return err;
    }
    trigger_tstamp(substream);
    runtime->hw_ptr_ns        = timer_monotonic_ns();
    runtime->hw_ptr_buffer_ns = runtime->rate ? (uint64_t)runtime->buffer_size * 1000000000ULL / runtime->rate : 0;
    set_state(runtime, SNDRV_PCM_STATE_RUNNING);
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && runtime->silence_size > 0) ulsa_pcm_playback_silence(substream, (snd_pcm_uframes_t)-1);
    ulsa_timer_pcm_notify(substream, SNDRV_TIMER_EVENT_MSTART);
    return EOK;
}

/* Stop the stream and enter `state`; stream lock held. */
int ulsa_pcm_stop(ulsa_pcm_substream_t *substream, snd_pcm_state_t state)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;

    if (runtime->status->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (ulsa_pcm_running(substream)) {
        (void)substream->ops->trigger(substream, ULSA_PCM_TRIGGER_STOP);
        runtime->stop_operating = true;
    }
    if (runtime->status->state != state) {
        trigger_tstamp(substream);
        set_state(runtime, state);
        ulsa_timer_pcm_notify(substream, SNDRV_TIMER_EVENT_MSTOP);
    }
    ulsa_pcm_wake_sleepers(substream);
    return EOK;
}

/* End of a drain: stop in SETUP; stream lock held. */
int ulsa_pcm_drain_done(ulsa_pcm_substream_t *substream)
{
    return ulsa_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
}

/* Force an xrun from a driver. */
int ulsa_pcm_stop_xrun(ulsa_pcm_substream_t *substream)
{
    uint64_t flags = spin_lock_irqsave(&substream->lock);

    if (substream->runtime && ulsa_pcm_running(substream)) ulsa_pcm_xrun(substream);
    spin_unlock_irqrestore(&substream->lock, flags);
    return EOK;
}

/* Push or release the pause; stream lock held. */
static int pcm_pause(ulsa_pcm_substream_t *substream, bool push)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    int                 err;

    if (!(runtime->info & SNDRV_PCM_INFO_PAUSE)) return -ENOSYS;
    if (push) {
        if (runtime->status->state != SNDRV_PCM_STATE_RUNNING) return -EBADFD;
    } else if (runtime->status->state != SNDRV_PCM_STATE_PAUSED) {
        return -EBADFD;
    }
    /* The pointer check measures wall-clock progress; a pause would trip it, so restart its clock. */
    runtime->hw_ptr_ns = timer_monotonic_ns();
    err                = substream->ops->trigger(substream, push ? ULSA_PCM_TRIGGER_PAUSE_PUSH : ULSA_PCM_TRIGGER_PAUSE_RELEASE);
    if (err < 0) return err;
    trigger_tstamp(substream);
    if (push) {
        set_state(runtime, SNDRV_PCM_STATE_PAUSED);
        ulsa_timer_pcm_notify(substream, SNDRV_TIMER_EVENT_MPAUSE);
        ulsa_pcm_wake_sleepers(substream);
    } else {
        set_state(runtime, SNDRV_PCM_STATE_RUNNING);
        ulsa_timer_pcm_notify(substream, SNDRV_TIMER_EVENT_MCONTINUE);
    }
    return EOK;
}

/* Rebase the pointers on the current hardware position; stream lock held. */
static void pcm_do_reset(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;

    if (ulsa_pcm_running(substream) && ulsa_pcm_update_hw_ptr(substream) >= 0) {
        runtime->status->hw_ptr %= runtime->buffer_size;
    } else {
        runtime->status->hw_ptr = 0;
        runtime->hw_ptr_wrap    = 0;
    }
    runtime->hw_ptr_base      = 0;
    runtime->hw_ptr_interrupt = runtime->status->hw_ptr - runtime->status->hw_ptr % runtime->period_size;
    runtime->silence_start    = runtime->status->hw_ptr;
    runtime->silence_filled   = 0;
}

/* SNDRV_PCM_IOCTL_RESET */
static int pcm_reset(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    uint64_t            flags   = spin_lock_irqsave(&substream->lock);
    int                 err     = EOK;

    switch (runtime->status->state) {
        case SNDRV_PCM_STATE_RUNNING :
        case SNDRV_PCM_STATE_PREPARED :
        case SNDRV_PCM_STATE_PAUSED :
        case SNDRV_PCM_STATE_SUSPENDED :
            pcm_do_reset(substream);
            runtime->control->appl_ptr = runtime->status->hw_ptr;
            if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && runtime->silence_size > 0) ulsa_pcm_playback_silence(substream, (snd_pcm_uframes_t)-1);
            break;
        default :
            err = -EBADFD;
            break;
    }
    spin_unlock_irqrestore(&substream->lock, flags);
    return err;
}

/* SNDRV_PCM_IOCTL_PREPARE */
static int pcm_prepare(ulsa_pcm_substream_t *substream, int f_flags)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    uint64_t            flags;
    int                 err;

    mutex_lock(&substream->buffer_mutex);
    flags = spin_lock_irqsave(&substream->lock);
    switch (runtime->status->state) {
        case SNDRV_PCM_STATE_PAUSED :
            (void)pcm_pause(substream, false);
            (void)ulsa_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
            break;
        case SNDRV_PCM_STATE_SUSPENDED :
            (void)ulsa_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
            break;
        default :
            break;
    }
    switch (runtime->status->state) {
        case SNDRV_PCM_STATE_OPEN :
        case SNDRV_PCM_STATE_DISCONNECTED :
            err = -EBADFD;
            goto unlock;
        case SNDRV_PCM_STATE_RUNNING :
            err = -EBUSY;
            goto unlock;
        case SNDRV_PCM_STATE_DRAINING :
            if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
                err = -EBUSY;
                goto unlock;
            }
            break;
        default :
            break;
    }
    substream->f_flags      = f_flags;
    runtime->stop_operating = false;
    spin_unlock_irqrestore(&substream->lock, flags);

    err = substream->ops->prepare ? substream->ops->prepare(substream) : EOK;
    if (err < 0) {
        mutex_unlock(&substream->buffer_mutex);
        return err;
    }

    flags = spin_lock_irqsave(&substream->lock);
    pcm_do_reset(substream);
    runtime->control->appl_ptr = runtime->status->hw_ptr;
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && runtime->silence_size > 0) ulsa_pcm_playback_silence(substream, (snd_pcm_uframes_t)-1);
    set_state(runtime, SNDRV_PCM_STATE_PREPARED);
    err = EOK;
unlock:
    spin_unlock_irqrestore(&substream->lock, flags);
    mutex_unlock(&substream->buffer_mutex);
    return err;
}

/* SNDRV_PCM_IOCTL_DROP */
static int pcm_drop(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    uint64_t            flags   = spin_lock_irqsave(&substream->lock);
    int                 err;

    if (runtime->status->state == SNDRV_PCM_STATE_OPEN || runtime->status->state == SNDRV_PCM_STATE_DISCONNECTED) {
        spin_unlock_irqrestore(&substream->lock, flags);
        return -EBADFD;
    }
    if (runtime->status->state == SNDRV_PCM_STATE_PAUSED) (void)pcm_pause(substream, false);
    err = ulsa_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
    spin_unlock_irqrestore(&substream->lock, flags);
    return err;
}

/* SNDRV_PCM_IOCTL_DRAIN: let queued playback data play out (or end a capture). */
static int pcm_drain(ulsa_pcm_substream_t *substream, bool nonblock)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    uint64_t            flags;
    int                 err = EOK;

    flags = spin_lock_irqsave(&substream->lock);
    switch (runtime->status->state) {
        case SNDRV_PCM_STATE_OPEN :
        case SNDRV_PCM_STATE_DISCONNECTED :
        case SNDRV_PCM_STATE_SUSPENDED :
            spin_unlock_irqrestore(&substream->lock, flags);
            return -EBADFD;
        default :
            break;
    }
    if (runtime->status->state == SNDRV_PCM_STATE_PAUSED) (void)pcm_pause(substream, false);
    if (ulsa_pcm_running(substream)) {
        err = ulsa_pcm_update_hw_ptr(substream);
        if (err < 0 && runtime->status->state != SNDRV_PCM_STATE_SETUP) {
            spin_unlock_irqrestore(&substream->lock, flags);
            return err;
        }
        err = EOK;
    }

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        switch (runtime->status->state) {
            case SNDRV_PCM_STATE_PREPARED :
                if (ulsa_pcm_playback_avail(runtime) < runtime->buffer_size) {
                    err = substream->ops->trigger(substream, ULSA_PCM_TRIGGER_START);
                    if (err < 0) goto unlock;
                    trigger_tstamp(substream);
                    runtime->hw_ptr_ns        = timer_monotonic_ns();
                    runtime->hw_ptr_buffer_ns = runtime->rate ? (uint64_t)runtime->buffer_size * 1000000000ULL / runtime->rate : 0;
                    set_state(runtime, SNDRV_PCM_STATE_DRAINING);
                    ulsa_timer_pcm_notify(substream, SNDRV_TIMER_EVENT_MSTART);
                } else {
                    set_state(runtime, SNDRV_PCM_STATE_SETUP);
                }
                break;
            case SNDRV_PCM_STATE_RUNNING :
                set_state(runtime, SNDRV_PCM_STATE_DRAINING);
                break;
            case SNDRV_PCM_STATE_XRUN :
                set_state(runtime, SNDRV_PCM_STATE_SETUP);
                break;
            default :
                break;
        }
    } else if (runtime->status->state == SNDRV_PCM_STATE_RUNNING) {
        snd_pcm_state_t next = ulsa_pcm_capture_avail(runtime) > 0 ? SNDRV_PCM_STATE_DRAINING : SNDRV_PCM_STATE_SETUP;
        (void)ulsa_pcm_stop(substream, next);
    }
    if (runtime->status->state == SNDRV_PCM_STATE_DRAINING && (runtime->hw.info & SNDRV_PCM_INFO_DRAIN_TRIGGER)) {
        err = substream->ops->trigger(substream, ULSA_PCM_TRIGGER_DRAIN);
        if (err < 0) goto unlock;
    }
    if (nonblock) {
        err = runtime->status->state == SNDRV_PCM_STATE_DRAINING && substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? -EAGAIN : EOK;
        goto unlock;
    }

    while (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && runtime->status->state == SNDRV_PCM_STATE_DRAINING) {
        uint64_t timeout_ms = 1000;
        uint64_t deadline;
        int      tout;

        if (signal_has_pending_current()) {
            err = -ERESTARTSYS;
            break;
        }
        if (runtime->rate) {
            uint64_t t = (uint64_t)runtime->buffer_size * 2000 / runtime->rate;
            if (t > timeout_ms) timeout_ms = t;
        }
        deadline = sched_ticks() + timeout_ms * CONFIG_TIMER_HZ / 1000 + 1;
        wait_queue_prepare(&runtime->sleep);
        spin_unlock_irqrestore(&substream->lock, flags);
        if (runtime->no_period_wakeup) {
            wait_queue_sleep();
            tout = 0;
        } else {
            tout = wait_queue_wait_timed(&runtime->sleep, deadline);
        }
        flags = spin_lock_irqsave(&substream->lock);
        if (tout == -ETIMEDOUT && runtime->status->state == SNDRV_PCM_STATE_DRAINING) {
            (void)ulsa_pcm_update_hw_ptr(substream);
            if (runtime->status->state != SNDRV_PCM_STATE_DRAINING) break;
            plogk("ulsa: playback drain timeout on card%d pcm%d (DMA or IRQ trouble?)\n", substream->pcm->card->number, substream->pcm->device);
            (void)ulsa_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
            err = -EIO;
            break;
        }
    }
unlock:
    spin_unlock_irqrestore(&substream->lock, flags);
    return err;
}

/* SNDRV_PCM_IOCTL_XRUN */
static int pcm_xrun_ioctl(ulsa_pcm_substream_t *substream)
{
    uint64_t flags = spin_lock_irqsave(&substream->lock);
    int      err   = EOK;

    switch (substream->runtime->status->state) {
        case SNDRV_PCM_STATE_XRUN :
            break;
        case SNDRV_PCM_STATE_RUNNING :
            ulsa_pcm_xrun(substream);
            break;
        default :
            err = -EBADFD;
            break;
    }
    spin_unlock_irqrestore(&substream->lock, flags);
    return err;
}

/* -------------------------------------------------------- hw params */

/* hw_params/hw_free/refine may only run before the stream starts. */
static int hw_params_state_check(ulsa_pcm_substream_t *substream, bool check_mmap)
{
    switch (substream->runtime->status->state) {
        case SNDRV_PCM_STATE_OPEN :
        case SNDRV_PCM_STATE_SETUP :
        case SNDRV_PCM_STATE_PREPARED :
            if (check_mmap && substream->runtime->mmap_count) return -EBADFD;
            return EOK;
        default :
            return -EBADFD;
    }
}

/* Release the hardware resources of hw_params. */
static int do_hw_free(ulsa_pcm_substream_t *substream)
{
    int err = substream->ops->hw_free ? substream->ops->hw_free(substream) : EOK;
    if (substream->managed_buffer) ulsa_pcm_managed_free(substream);
    return err;
}

/* SNDRV_PCM_IOCTL_HW_PARAMS */
static int pcm_hw_params(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    unsigned int        bits;
    snd_pcm_uframes_t   frames;
    uint64_t            flags;
    int                 err;

    mutex_lock(&substream->buffer_mutex);
    flags = spin_lock_irqsave(&substream->lock);
    err   = hw_params_state_check(substream, true);
    if (!err && runtime->buffer_accessing) err = -EBUSY;
    if (!err) runtime->buffer_accessing = -1;
    spin_unlock_irqrestore(&substream->lock, flags);
    if (err) goto unlock;

    params->rmask = ~0U;
    err           = ulsa_pcm_hw_refine(substream, params);
    if (err < 0) goto error;
    err = ulsa_pcm_hw_params_choose(substream, params);
    if (err < 0) goto error;
    err = ulsa_pcm_fixup_unreferenced_params(substream, params);
    if (err < 0) goto error;

    if (substream->managed_buffer) {
        err = ulsa_pcm_managed_alloc(substream, ulsa_params_buffer_bytes(params));
        if (err < 0) goto error;
    }
    if (substream->ops->hw_params) {
        err = substream->ops->hw_params(substream, params);
        if (err < 0) goto error;
    }

    flags                     = spin_lock_irqsave(&substream->lock);
    runtime->access           = ulsa_params_access(params);
    runtime->format           = ulsa_params_format(params);
    runtime->subformat        = ulsa_params_subformat(params);
    runtime->channels         = ulsa_params_channels(params);
    runtime->rate             = ulsa_params_rate(params);
    runtime->period_size      = ulsa_params_period_size(params);
    runtime->periods          = ulsa_params_periods(params);
    runtime->buffer_size      = ulsa_params_buffer_size(params);
    runtime->info             = params->info;
    runtime->rate_num         = params->rate_num;
    runtime->rate_den         = params->rate_den;
    runtime->no_period_wakeup = (params->info & SNDRV_PCM_INFO_NO_PERIOD_WAKEUP) && (params->flags & SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP);

    bits                 = (unsigned int)ulsa_pcm_format_physical_width(runtime->format);
    runtime->sample_bits = bits;
    bits *= runtime->channels;
    runtime->frame_bits = bits;
    frames              = 1;
    while (bits % 8) {
        bits *= 2;
        frames *= 2;
    }
    runtime->byte_align = bits / 8;
    runtime->min_align  = frames;

    /* Default software parameters, as Linux sets them. */
    runtime->tstamp_mode        = SNDRV_PCM_TSTAMP_NONE;
    runtime->period_step        = 1;
    runtime->control->avail_min = runtime->period_size;
    runtime->start_threshold    = 1;
    runtime->stop_threshold     = runtime->buffer_size;
    runtime->silence_threshold  = 0;
    runtime->silence_size       = 0;
    runtime->boundary           = runtime->buffer_size;
    while (runtime->boundary * 2 <= 0x7fffffffffffffffUL - runtime->buffer_size) runtime->boundary *= 2;

    /* Never leak stale kernel memory through a fresh ring buffer. */
    if (runtime->dma_area) memset(runtime->dma_area, 0, runtime->dma_alloc_pages ? runtime->dma_alloc_pages * PAGE_4K_SIZE : runtime->dma_bytes);

    set_state(runtime, SNDRV_PCM_STATE_SETUP);
    runtime->buffer_accessing = 0;
    spin_unlock_irqrestore(&substream->lock, flags);
    ulsa_timer_pcm_resolution_change(substream);
    mutex_unlock(&substream->buffer_mutex);
    return EOK;

error:
    /* The hardware may be half-configured now; force the application to start over. */
    flags = spin_lock_irqsave(&substream->lock);
    set_state(runtime, SNDRV_PCM_STATE_OPEN);
    runtime->buffer_accessing = 0;
    spin_unlock_irqrestore(&substream->lock, flags);
    (void)do_hw_free(substream);
unlock:
    mutex_unlock(&substream->buffer_mutex);
    return err;
}

/* SNDRV_PCM_IOCTL_HW_REFINE */
static int pcm_hw_refine(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    int err;

    mutex_lock(&substream->buffer_mutex);
    err = hw_params_state_check(substream, false);
    if (!err) err = ulsa_pcm_hw_refine(substream, params);
    mutex_unlock(&substream->buffer_mutex);
    if (err < 0) return err;
    return ulsa_pcm_fixup_unreferenced_params(substream, params);
}

/* SNDRV_PCM_IOCTL_HW_FREE */
static int pcm_hw_free(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    uint64_t            flags;
    int                 err = EOK;

    mutex_lock(&substream->buffer_mutex);
    flags = spin_lock_irqsave(&substream->lock);
    switch (runtime->status->state) {
        case SNDRV_PCM_STATE_SETUP :
        case SNDRV_PCM_STATE_PREPARED :
            if (runtime->mmap_count || runtime->buffer_accessing) err = -EBADFD;
            break;
        default :
            err = -EBADFD;
            break;
    }
    if (!err) runtime->buffer_accessing = -1;
    spin_unlock_irqrestore(&substream->lock, flags);
    if (!err) {
        err   = do_hw_free(substream);
        flags = spin_lock_irqsave(&substream->lock);
        set_state(runtime, SNDRV_PCM_STATE_OPEN);
        runtime->buffer_accessing = 0;
        spin_unlock_irqrestore(&substream->lock, flags);
    }
    mutex_unlock(&substream->buffer_mutex);
    return err;
}

/* SNDRV_PCM_IOCTL_SW_PARAMS */
static int pcm_sw_params(ulsa_pcm_substream_t *substream, struct snd_pcm_sw_params *params)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    uint64_t            flags;
    int                 err = EOK;

    if (runtime->status->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (params->tstamp_mode < 0 || params->tstamp_mode > SNDRV_PCM_TSTAMP_LAST) return -EINVAL;
    if (params->proto >= SNDRV_PROTOCOL_VERSION(2, 0, 12) && params->tstamp_type > SNDRV_PCM_TSTAMP_TYPE_LAST) return -EINVAL;
    if (params->avail_min == 0) return -EINVAL;
    if (params->silence_size >= runtime->boundary) {
        if (params->silence_threshold != 0) return -EINVAL;
    } else {
        if (params->silence_size > params->silence_threshold) return -EINVAL;
        if (params->silence_threshold > runtime->buffer_size) return -EINVAL;
    }

    flags                = spin_lock_irqsave(&substream->lock);
    runtime->tstamp_mode = params->tstamp_mode;
    if (params->proto >= SNDRV_PROTOCOL_VERSION(2, 0, 12)) runtime->tstamp_type = params->tstamp_type;
    runtime->period_step        = params->period_step;
    runtime->control->avail_min = params->avail_min;
    runtime->start_threshold    = params->start_threshold;
    runtime->stop_threshold     = params->stop_threshold;
    runtime->silence_threshold  = params->silence_threshold;
    runtime->silence_size       = params->silence_size;
    params->boundary            = runtime->boundary;
    if (ulsa_pcm_running(substream)) {
        if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && runtime->silence_size > 0) ulsa_pcm_playback_silence(substream, (snd_pcm_uframes_t)-1);
        err = ulsa_pcm_update_state(substream);
    }
    spin_unlock_irqrestore(&substream->lock, flags);
    return err;
}

/* ------------------------------------------------- status and sync */

/* Frames between the application and the DAC/ADC. */
static snd_pcm_sframes_t calc_delay(ulsa_pcm_substream_t *substream)
{
    snd_pcm_sframes_t delay;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        delay = ulsa_pcm_hw_avail(substream);
    } else {
        delay = (snd_pcm_sframes_t)ulsa_pcm_capture_avail(substream->runtime);
    }
    return delay + substream->runtime->delay;
}

/* SNDRV_PCM_IOCTL_STATUS / STATUS_EXT */
static int pcm_status(ulsa_pcm_substream_t *substream, struct snd_pcm_status *status)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    uint64_t            flags   = spin_lock_irqsave(&substream->lock);

    status->state           = runtime->status->state;
    status->suspended_state = runtime->status->suspended_state;
    if (status->state == SNDRV_PCM_STATE_OPEN) goto out;
    status->trigger_tstamp = runtime->trigger_tstamp;
    if (ulsa_pcm_running(substream)) {
        if (ulsa_pcm_update_hw_ptr(substream) < 0) {
            status->state           = runtime->status->state;
            status->suspended_state = runtime->status->suspended_state;
            status->trigger_tstamp  = runtime->trigger_tstamp;
        }
        if (runtime->tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE) {
            status->tstamp        = runtime->status->tstamp;
            status->driver_tstamp = runtime->driver_tstamp;
            status->audio_tstamp  = runtime->status->audio_tstamp;
        }
    } else if (runtime->tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE) {
        ulsa_pcm_gettime(runtime, &status->tstamp);
    }
    status->appl_ptr   = runtime->control->appl_ptr;
    status->hw_ptr     = runtime->status->hw_ptr;
    status->avail      = ulsa_pcm_avail(substream);
    status->delay      = ulsa_pcm_running(substream) ? calc_delay(substream) : 0;
    status->avail_max  = runtime->avail_max;
    status->overrange  = runtime->overrange;
    runtime->avail_max = 0;
    runtime->overrange = 0;
out:
    spin_unlock_irqrestore(&substream->lock, flags);
    return EOK;
}

/* Bring hw_ptr up to date for HWSYNC/DELAY/REWIND/FORWARD; stream lock held. */
static int do_hwsync(ulsa_pcm_substream_t *substream)
{
    switch (substream->runtime->status->state) {
        case SNDRV_PCM_STATE_DRAINING :
            if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) return EOK;
            return ulsa_pcm_update_hw_ptr(substream);
        case SNDRV_PCM_STATE_RUNNING :
            return ulsa_pcm_update_hw_ptr(substream);
        case SNDRV_PCM_STATE_PREPARED :
        case SNDRV_PCM_STATE_PAUSED :
            return EOK;
        case SNDRV_PCM_STATE_SUSPENDED :
            return -ESTRPIPE;
        case SNDRV_PCM_STATE_XRUN :
            return -EPIPE;
        default :
            return -EBADFD;
    }
}

/* SNDRV_PCM_IOCTL_DELAY / HWSYNC */
static int pcm_delay(ulsa_pcm_substream_t *substream, snd_pcm_sframes_t *delay)
{
    uint64_t flags = spin_lock_irqsave(&substream->lock);
    int      err   = do_hwsync(substream);

    if (delay && !err) *delay = calc_delay(substream);
    spin_unlock_irqrestore(&substream->lock, flags);
    return err;
}

/* SNDRV_PCM_IOCTL_REWIND / FORWARD: move appl_ptr; returns frames moved. */
static snd_pcm_sframes_t pcm_move_appl(ulsa_pcm_substream_t *substream, snd_pcm_uframes_t frames, bool forward)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    snd_pcm_sframes_t   ret, avail;
    uint64_t            flags;

    if (!frames) return 0;
    flags = spin_lock_irqsave(&substream->lock);
    ret   = do_hwsync(substream);
    if (!ret) {
        avail = forward ? (snd_pcm_sframes_t)ulsa_pcm_avail(substream) : ulsa_pcm_hw_avail(substream);
        if (avail <= 0) {
            ret = 0;
        } else {
            snd_pcm_sframes_t appl;
            int               err;

            if (frames > (snd_pcm_uframes_t)avail) frames = (snd_pcm_uframes_t)avail;
            if (forward) {
                appl = (snd_pcm_sframes_t)(runtime->control->appl_ptr + frames);
                if (appl >= (snd_pcm_sframes_t)runtime->boundary) appl -= (snd_pcm_sframes_t)runtime->boundary;
            } else {
                appl = (snd_pcm_sframes_t)runtime->control->appl_ptr - (snd_pcm_sframes_t)frames;
                if (appl < 0) appl += (snd_pcm_sframes_t)runtime->boundary;
            }
            err = ulsa_pcm_apply_appl_ptr(substream, (snd_pcm_uframes_t)appl);
            /* A failed rewind reports 0 frames rather than an error, which PulseAudio relies on. */
            if (err < 0) {
                ret = forward ? err : 0;
            } else {
                ret = (snd_pcm_sframes_t)frames;
            }
        }
    }
    spin_unlock_irqrestore(&substream->lock, flags);
    return ret;
}

/*
 * SNDRV_PCM_IOCTL_SYNC_PTR: the ioctl twin of the mmap'ed status/control
 * records.  Unless the APPL / AVAIL_MIN flags ask to read them back, the
 * caller's appl_ptr and avail_min are applied first; then the current status
 * and control records are returned.
 */
static int pcm_sync_ptr(ulsa_pcm_substream_t *substream, struct snd_pcm_sync_ptr *usp)
{
    ulsa_pcm_runtime_t         *runtime = substream->runtime;
    struct snd_pcm_sync_ptr     sp;
    struct snd_pcm_mmap_control control;
    uint64_t                    flags;
    int                         err;

    if (copy_from_user(&sp, usp, sizeof(sp))) return -EFAULT;
    if (sp.flags & SNDRV_PCM_SYNC_PTR_HWSYNC) {
        err = pcm_delay(substream, NULL);
        if (err < 0) return err;
    }
    control = sp.c.control;
    flags   = spin_lock_irqsave(&substream->lock);
    if (!(sp.flags & SNDRV_PCM_SYNC_PTR_APPL)) {
        err = ulsa_pcm_apply_appl_ptr(substream, control.appl_ptr);
        if (err < 0) {
            spin_unlock_irqrestore(&substream->lock, flags);
            return err;
        }
    } else {
        control.appl_ptr = runtime->control->appl_ptr;
    }
    if (!(sp.flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN)) {
        runtime->control->avail_min = control.avail_min;
    } else {
        control.avail_min = runtime->control->avail_min;
    }
    memset(&sp.s, 0, sizeof(sp.s));
    sp.s.status.state           = runtime->status->state;
    sp.s.status.hw_ptr          = runtime->status->hw_ptr;
    sp.s.status.tstamp          = runtime->status->tstamp;
    sp.s.status.suspended_state = runtime->status->suspended_state;
    sp.s.status.audio_tstamp    = runtime->status->audio_tstamp;
    spin_unlock_irqrestore(&substream->lock, flags);
    sp.c.control = control;
    return copy_to_user(usp, &sp, sizeof(sp)) ? -EFAULT : EOK;
}

/* SNDRV_PCM_IOCTL_CHANNEL_INFO: where a channel's samples live in the mmap'ed ring. */
static int pcm_channel_info(ulsa_pcm_substream_t *substream, struct snd_pcm_channel_info *info)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    unsigned int        channel = info->channel;
    int                 width;

    if (runtime->status->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (channel >= runtime->channels) return -EINVAL;
    memset(info, 0, sizeof(*info));
    info->channel = channel;
    if (!(runtime->info & SNDRV_PCM_INFO_MMAP)) {
        info->offset = -1;
        return EOK;
    }
    width = ulsa_pcm_format_physical_width(runtime->format);
    if (width < 0) return width;
    info->offset = (long)SNDRV_PCM_MMAP_OFFSET_DATA;
    switch (runtime->access) {
        case SNDRV_PCM_ACCESS_MMAP_INTERLEAVED :
        case SNDRV_PCM_ACCESS_RW_INTERLEAVED :
            info->first = channel * (unsigned int)width;
            info->step  = runtime->channels * (unsigned int)width;
            break;
        case SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED :
        case SNDRV_PCM_ACCESS_RW_NONINTERLEAVED :
            info->first = (unsigned int)(channel * (runtime->dma_bytes / runtime->channels) * 8);
            info->step  = (unsigned int)width;
            break;
        default :
            return -EINVAL;
    }
    return EOK;
}

/* Fill snd_pcm_info for a substream. */
int ulsa_pcm_info(ulsa_pcm_substream_t *substream, struct snd_pcm_info *info)
{
    ulsa_pcm_t     *pcm  = substream->pcm;
    ulsa_pcm_str_t *pstr = substream->pstr;

    memset(info, 0, sizeof(*info));
    info->card             = pcm->card->number;
    info->device           = (unsigned int)pcm->device;
    info->stream           = substream->stream;
    info->subdevice        = (unsigned int)substream->number;
    info->dev_class        = pcm->dev_class;
    info->dev_subclass     = pcm->dev_subclass;
    info->subdevices_count = pstr->substream_count;
    info->subdevices_avail = pstr->substream_count - pstr->substream_opened;
    strncpy((char *)info->id, pcm->id, sizeof(info->id) - 1);
    strncpy((char *)info->name, pcm->name, sizeof(info->name) - 1);
    strncpy((char *)info->subname, substream->name, sizeof(info->subname) - 1);
    return EOK;
}

/* ------------------------------------------------------ open/close */

/* Allocate the runtime, apply the driver's open and its constraints. */
static int pcm_attach_runtime(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = calloc(1, sizeof(*runtime));
    int                 err;

    if (!runtime) return -ENOMEM;
    err = ulsa_pcm_runtime_alloc_records(runtime);
    if (err < 0) {
        free(runtime);
        return err;
    }
    wait_queue_init(&runtime->sleep);
    wait_queue_init(&runtime->tsleep);
    vfs_poll_source_init(&runtime->poll_source);
    runtime->status->state = SNDRV_PCM_STATE_OPEN;
    runtime->tstamp_type   = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC;
    substream->runtime     = runtime;

    err = ulsa_pcm_hw_constraints_init(substream);
    if (err < 0) goto fail;
    err = substream->ops->open ? substream->ops->open(substream) : -ENODEV;
    if (err < 0) goto fail;
    err = ulsa_pcm_hw_constraints_complete(substream);
    if (err < 0) {
        if (substream->ops->close) (void)substream->ops->close(substream);
        goto fail;
    }
    (void)ulsa_timer_pcm_new(substream);
    return EOK;

fail:
    ulsa_pcm_hw_constraints_free(runtime);
    ulsa_pcm_runtime_free_records(runtime);
    substream->runtime = NULL;
    free(runtime);
    return err;
}

/* Detach and free the runtime of a closed substream. */
static void pcm_detach_runtime(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;

    if (!runtime) return;
    ulsa_timer_pcm_free(substream);
    if (runtime->private_free) runtime->private_free(runtime);
    vfs_poll_source_close(&runtime->poll_source, POLLHUP);
    ulsa_pcm_hw_constraints_free(runtime);
    ulsa_pcm_runtime_free_records(runtime);
    substream->runtime = NULL;
    free(runtime);
}

/* Pick and open a free substream of pcm/stream; -EAGAIN when all are busy. */
static int pcm_try_open(ulsa_pcm_t *pcm, int stream, int f_flags, ulsa_pcm_substream_t **substream_ret)
{
    ulsa_pcm_str_t       *pstr = &pcm->streams[stream];
    ulsa_pcm_substream_t *s;
    int                   err;

    if (!pstr->substream_count) return -ENODEV;
    for (s = pstr->substream; s; s = s->next)
        if (!s->opened) break;
    if (!s) return -EAGAIN;

    s->f_flags = f_flags;
    err        = pcm_attach_runtime(s);
    if (err < 0) return err;
    s->opened = true;
    pstr->substream_opened++;
    *substream_ret = s;
    return EOK;
}

/* Open a substream, sleeping (unless O_NONBLOCK) while all of them are busy. */
int ulsa_pcm_open_substream(ulsa_pcm_t *pcm, int stream, int f_flags, ulsa_pcm_substream_t **substream_ret)
{
    int err;

    mutex_lock(&pcm->open_mutex);
    for (;;) {
        err = pcm_try_open(pcm, stream, f_flags, substream_ret);
        if (err != -EAGAIN) break;
        if (f_flags & O_NONBLOCK) {
            err = -EBUSY;
            break;
        }
        if (signal_has_pending_current()) {
            err = -ERESTARTSYS;
            break;
        }
        wait_queue_prepare(&pcm->open_wait);
        mutex_unlock(&pcm->open_mutex);
        wait_queue_sleep();
        mutex_lock(&pcm->open_mutex);
    }
    mutex_unlock(&pcm->open_mutex);
    return err;
}

/* Stop, free and close a substream at last release. */
void ulsa_pcm_release_substream(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_t *pcm = substream->pcm;
    uint64_t    flags;

    if (!substream->runtime) return;
    flags = spin_lock_irqsave(&substream->lock);
    if (substream->runtime->status->state != SNDRV_PCM_STATE_OPEN) {
        if (substream->runtime->status->state == SNDRV_PCM_STATE_PAUSED) (void)pcm_pause(substream, false);
        (void)ulsa_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
    }
    spin_unlock_irqrestore(&substream->lock, flags);

    mutex_lock(&substream->buffer_mutex);
    if (substream->runtime->status->state != SNDRV_PCM_STATE_OPEN) (void)do_hw_free(substream);
    if (substream->ops->close) (void)substream->ops->close(substream);
    mutex_unlock(&substream->buffer_mutex);

    mutex_lock(&pcm->open_mutex);
    pcm_detach_runtime(substream);
    substream->opened = false;
    substream->pstr->substream_opened--;
    mutex_unlock(&pcm->open_mutex);
    wait_queue_wake_all(&pcm->open_wait);
}

/* ---------------------------------------------------- file callbacks */

/* tmpfs open: bind the file to a free substream of the node's PCM stream. */
static int pcm_dev_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    tmpfs_file_t         *f     = node ? node->handle : NULL;
    ulsa_minor_t         *minor = f ? f->device.ctx : NULL;
    ulsa_pcm_file_t      *pcm_file;
    ulsa_pcm_substream_t *substream;
    int                   err;

    if (!minor || !minor->pcm || !private_data) return -ENODEV;
    pcm_file = calloc(1, sizeof(*pcm_file));
    if (!pcm_file) return -ENOMEM;
    err = ulsa_pcm_open_substream(minor->pcm, minor->stream, (int)flags, &substream);
    if (err < 0) {
        free(pcm_file);
        return err;
    }
    pcm_file->substream = substream;
    *private_data       = pcm_file;
    return EOK;
}

/* tmpfs release: close the substream. */
static void pcm_dev_release(vfs_node_t node, void *private_data)
{
    ulsa_pcm_file_t *pcm_file = private_data;

    (void)node;
    if (!pcm_file) return;
    if (pcm_file->substream) ulsa_pcm_release_substream(pcm_file->substream);
    free(pcm_file);
}

/* Kernel-buffer transfers are never used for PCM nodes. */
static int64_t pcm_dev_read_kernel(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    return -EINVAL;
}

/* Kernel-buffer transfers are never used for PCM nodes. */
static int64_t pcm_dev_write_kernel(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    return -EINVAL;
}

/* read(2)/write(2): whole frames of interleaved data. */
static int64_t pcm_dev_xfer_user(void *private_data, uint64_t flags, void *addr, size_t size, int stream)
{
    ulsa_pcm_file_t      *pcm_file  = private_data;
    ulsa_pcm_substream_t *substream = pcm_file ? pcm_file->substream : NULL;
    ulsa_pcm_runtime_t   *runtime;
    snd_pcm_sframes_t     result;
    void                 *bufs[1] = {addr};

    if (!substream || !substream->runtime) return -ENXIO;
    if (substream->stream != stream) return -EINVAL;
    runtime = substream->runtime;
    if (runtime->status->state == SNDRV_PCM_STATE_OPEN || runtime->status->state == SNDRV_PCM_STATE_DISCONNECTED) return -EBADFD;
    if (!runtime->byte_align || size % runtime->byte_align) return -EINVAL;
    result = ulsa_pcm_lib_xfer(substream, bufs, true, (snd_pcm_uframes_t)ulsa_bytes_to_frames(runtime, size), (flags & O_NONBLOCK) != 0);
    if (result > 0) return (int64_t)ulsa_frames_to_bytes(runtime, result);
    return result;
}

/* read(2) on a capture node. */
static int64_t pcm_dev_read_user(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc)
{
    (void)ctx;
    (void)offset;
    (void)proc;
    return pcm_dev_xfer_user(private_data, flags, addr, size, SNDRV_PCM_STREAM_CAPTURE);
}

/* write(2) on a playback node. */
static int64_t pcm_dev_write_user(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, struct process *proc)
{
    (void)ctx;
    (void)offset;
    (void)proc;
    return pcm_dev_xfer_user(private_data, flags, (void *)addr, size, SNDRV_PCM_STREAM_PLAYBACK);
}

/* poll(2): ready once avail_min frames can be transferred. */
static int pcm_dev_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    ulsa_pcm_file_t      *pcm_file  = private_data;
    ulsa_pcm_substream_t *substream = pcm_file ? pcm_file->substream : NULL;
    ulsa_pcm_runtime_t   *runtime;
    unsigned int          ok, mask = 0;
    snd_pcm_uframes_t     avail;
    uint64_t              lflags;

    (void)ctx;
    (void)flags;
    if (!substream || !substream->runtime) return (int)(POLLERR & events);
    runtime = substream->runtime;
    ok      = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? (POLLOUT | POLLWRNORM) : (POLLIN | POLLRDNORM);

    lflags = spin_lock_irqsave(&substream->lock);
    if (ulsa_pcm_running(substream)) (void)ulsa_pcm_update_hw_ptr(substream);
    avail = ulsa_pcm_avail(substream);
    switch (runtime->status->state) {
        case SNDRV_PCM_STATE_RUNNING :
        case SNDRV_PCM_STATE_PREPARED :
        case SNDRV_PCM_STATE_PAUSED :
            if (avail >= runtime->control->avail_min) mask = ok;
            break;
        case SNDRV_PCM_STATE_DRAINING :
            if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
                mask = ok;
                if (!avail) mask |= POLLERR;
            }
            break;
        default :
            mask = ok | POLLERR;
            break;
    }
    spin_unlock_irqrestore(&substream->lock, lflags);
    return (int)(mask & (events | POLLERR));
}

/* Readiness source for poll/epoll. */
static vfs_poll_source_t *pcm_dev_poll_source(void *ctx, void *private_data)
{
    ulsa_pcm_file_t *pcm_file = private_data;

    (void)ctx;
    if (!pcm_file || !pcm_file->substream || !pcm_file->substream->runtime) return NULL;
    return &pcm_file->substream->runtime->poll_source;
}

/* mmap(2) of the ring buffer or the status/control records. */
static void *pcm_dev_mmap(void *ctx, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    ulsa_pcm_file_t *pcm_file = private_data;

    (void)ctx;
    if (!pcm_file || !pcm_file->substream || !pcm_file->substream->runtime) return NULL;
    return ulsa_pcm_mmap(pcm_file->substream, offset, size, flags, vma);
}

/* READI/WRITEI and READN/WRITEN */
static int pcm_xfer_ioctl(ulsa_pcm_substream_t *substream, uint64_t flags, size_t req, void *arg)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    bool                write   = req == SNDRV_PCM_IOCTL_WRITEI_FRAMES || req == SNDRV_PCM_IOCTL_WRITEN_FRAMES;
    bool                inter   = req == SNDRV_PCM_IOCTL_WRITEI_FRAMES || req == SNDRV_PCM_IOCTL_READI_FRAMES;
    snd_pcm_sframes_t   result;
    snd_pcm_sframes_t   zero = 0;
    void              **bufs;

    if (runtime->status->state == SNDRV_PCM_STATE_OPEN) return -EBADFD;
    if (write != (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)) return -EINVAL;
    if (copy_to_user(arg, &zero, sizeof(zero))) return -EFAULT;

    if (inter) {
        struct snd_xferi xferi;
        void            *one[1];

        if (copy_from_user(&xferi, arg, sizeof(xferi))) return -EFAULT;
        one[0] = xferi.buf;
        result = ulsa_pcm_lib_xfer(substream, one, true, xferi.frames, (flags & O_NONBLOCK) != 0);
    } else {
        struct snd_xfern xfern;

        if (runtime->channels > 128) return -EINVAL;
        if (copy_from_user(&xfern, arg, sizeof(xfern))) return -EFAULT;
        bufs = malloc(runtime->channels * sizeof(void *));
        if (!bufs) return -ENOMEM;
        if (copy_from_user(bufs, xfern.bufs, runtime->channels * sizeof(void *))) {
            free(bufs);
            return -EFAULT;
        }
        result = ulsa_pcm_lib_xfer(substream, bufs, false, xfern.frames, (flags & O_NONBLOCK) != 0);
        free(bufs);
    }
    if (copy_to_user(arg, &result, sizeof(result))) return -EFAULT;
    return result < 0 ? (int)result : EOK;
}

/* PCM ioctl dispatcher. */
static int pcm_dev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    ulsa_pcm_file_t      *pcm_file  = private_data;
    ulsa_pcm_substream_t *substream = pcm_file ? pcm_file->substream : NULL;
    ulsa_pcm_runtime_t   *runtime;

    (void)ctx;
    if (!substream || !substream->runtime) return -ENXIO;
    runtime = substream->runtime;
    if (((req >> 8) & 0xff) != 'A') return -ENOTTY;
    if (runtime->status->state == SNDRV_PCM_STATE_DISCONNECTED) return -EBADFD;

    switch (req) {
        case SNDRV_PCM_IOCTL_PVERSION : {
            int version = SNDRV_PCM_VERSION;
            return copy_to_user(arg, &version, sizeof(version)) ? -EFAULT : EOK;
        }
        case SNDRV_PCM_IOCTL_INFO : {
            struct snd_pcm_info info;
            (void)ulsa_pcm_info(substream, &info);
            return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : EOK;
        }
        case SNDRV_PCM_IOCTL_TSTAMP :
            return EOK;
        case SNDRV_PCM_IOCTL_TTSTAMP : {
            int type;
            if (copy_from_user(&type, arg, sizeof(type))) return -EFAULT;
            if (type < 0 || type > SNDRV_PCM_TSTAMP_TYPE_LAST) return -EINVAL;
            runtime->tstamp_type = (unsigned int)type;
            return EOK;
        }
        case SNDRV_PCM_IOCTL_USER_PVERSION :
            return copy_from_user(&pcm_file->user_pversion, arg, sizeof(pcm_file->user_pversion)) ? -EFAULT : EOK;
        case SNDRV_PCM_IOCTL_HW_REFINE :
        case SNDRV_PCM_IOCTL_HW_PARAMS : {
            struct snd_pcm_hw_params *params = malloc(sizeof(*params));
            int                       err;

            if (!params) return -ENOMEM;
            if (copy_from_user(params, arg, sizeof(*params))) {
                free(params);
                return -EFAULT;
            }
            err = req == SNDRV_PCM_IOCTL_HW_PARAMS ? pcm_hw_params(substream, params) : pcm_hw_refine(substream, params);
            if (err >= 0 && copy_to_user(arg, params, sizeof(*params))) err = -EFAULT;
            free(params);
            return err;
        }
        case SNDRV_PCM_IOCTL_HW_FREE :
            return pcm_hw_free(substream);
        case SNDRV_PCM_IOCTL_SW_PARAMS : {
            struct snd_pcm_sw_params params;
            int                      err;

            if (copy_from_user(&params, arg, sizeof(params))) return -EFAULT;
            err = pcm_sw_params(substream, &params);
            if (err < 0) return err;
            return copy_to_user(arg, &params, sizeof(params)) ? -EFAULT : EOK;
        }
        case SNDRV_PCM_IOCTL_STATUS :
        case SNDRV_PCM_IOCTL_STATUS_EXT : {
            struct snd_pcm_status status;

            memset(&status, 0, sizeof(status));
            if (req == SNDRV_PCM_IOCTL_STATUS_EXT && copy_from_user(&status.audio_tstamp_data, (uint8_t *)arg + offsetof(struct snd_pcm_status, audio_tstamp_data), sizeof(status.audio_tstamp_data)))
                return -EFAULT;
            (void)pcm_status(substream, &status);
            return copy_to_user(arg, &status, sizeof(status)) ? -EFAULT : EOK;
        }
        case SNDRV_PCM_IOCTL_CHANNEL_INFO : {
            struct snd_pcm_channel_info info;
            int                         err;

            if (copy_from_user(&info, arg, sizeof(info))) return -EFAULT;
            err = pcm_channel_info(substream, &info);
            if (err < 0) return err;
            return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : EOK;
        }
        case SNDRV_PCM_IOCTL_PREPARE :
            return pcm_prepare(substream, (int)flags);
        case SNDRV_PCM_IOCTL_RESET :
            return pcm_reset(substream);
        case SNDRV_PCM_IOCTL_START : {
            uint64_t lflags = spin_lock_irqsave(&substream->lock);
            int      err    = ulsa_pcm_start(substream);
            spin_unlock_irqrestore(&substream->lock, lflags);
            return err;
        }
        case SNDRV_PCM_IOCTL_DROP :
            return pcm_drop(substream);
        case SNDRV_PCM_IOCTL_DRAIN :
            return pcm_drain(substream, (flags & O_NONBLOCK) != 0);
        case SNDRV_PCM_IOCTL_PAUSE : {
            /* The pause flag is passed by value, not through a pointer. */
            uint64_t lflags = spin_lock_irqsave(&substream->lock);
            int      err    = pcm_pause(substream, (uintptr_t)arg != 0);
            spin_unlock_irqrestore(&substream->lock, lflags);
            return err;
        }
        case SNDRV_PCM_IOCTL_RESUME :
            return -ENOSYS;
        case SNDRV_PCM_IOCTL_XRUN :
            return pcm_xrun_ioctl(substream);
        case SNDRV_PCM_IOCTL_HWSYNC :
            return pcm_delay(substream, NULL);
        case SNDRV_PCM_IOCTL_DELAY : {
            snd_pcm_sframes_t delay = 0;
            int               err   = pcm_delay(substream, &delay);
            if (err) return err;
            return copy_to_user(arg, &delay, sizeof(delay)) ? -EFAULT : EOK;
        }
        case SNDRV_PCM_IOCTL_SYNC_PTR :
            return pcm_sync_ptr(substream, arg);
        case SNDRV_PCM_IOCTL_REWIND :
        case SNDRV_PCM_IOCTL_FORWARD : {
            snd_pcm_uframes_t frames;
            snd_pcm_sframes_t result;
            snd_pcm_sframes_t zero = 0;

            if (copy_from_user(&frames, arg, sizeof(frames))) return -EFAULT;
            if (copy_to_user(arg, &zero, sizeof(zero))) return -EFAULT;
            result = pcm_move_appl(substream, frames, req == SNDRV_PCM_IOCTL_FORWARD);
            if (copy_to_user(arg, &result, sizeof(result))) return -EFAULT;
            return result < 0 ? (int)result : EOK;
        }
        case SNDRV_PCM_IOCTL_WRITEI_FRAMES :
        case SNDRV_PCM_IOCTL_READI_FRAMES :
        case SNDRV_PCM_IOCTL_WRITEN_FRAMES :
        case SNDRV_PCM_IOCTL_READN_FRAMES :
            return pcm_xfer_ioctl(substream, flags, req, arg);
        case SNDRV_PCM_IOCTL_LINK :
        case SNDRV_PCM_IOCTL_UNLINK :
            /* Stream linking (synchronised start of several PCMs) is not implemented. */
            return -ENOSYS;
        default :
            return -ENOTTY;
    }
}

const tmpfs_device_ops_t ulsa_pcm_device_ops = {
    .open             = pcm_dev_open,
    .release          = pcm_dev_release,
    .mmap             = pcm_dev_mmap,
    .file_read        = pcm_dev_read_kernel,
    .file_write       = pcm_dev_write_kernel,
    .file_read_user   = pcm_dev_read_user,
    .file_write_user  = pcm_dev_write_user,
    .file_poll        = pcm_dev_poll,
    .file_poll_source = pcm_dev_poll_source,
    .file_ioctl       = pcm_dev_ioctl,
};

#endif
