/*
 *
 *      sb16.c
 *      Sound Blaster 16 (ISA DSP 4.x) driver
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/firmware/dma.h>
#include <arch/misc/common.h>
#include <arch/tables/idt.h>
#include <drivers/audio/soundblaster/sb16.h>
#include <drivers/audio/ulsa/ulsa.h>
#include <drivers/firmware/apic.h>
#include <kernel/debug/printk.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

#if CONFIG_AUDIO && CONFIG_AUDIO_SB16

#    define SB16_RATE_MIN 5000U
#    define SB16_RATE_MAX 44100U

typedef struct sb16_stream {
        ulsa_pcm_substream_t *substream;
        uint64_t              alloc_phys;
        uint32_t              buf_phys;
        uint8_t              *buf_virt;
        uint32_t              dma_bytes;
        uint8_t               dma_ch;
        bool                  is_16;
        bool                  opened;
        bool                  prepared;
        bool                  running;
} sb16_stream_t;

typedef struct sb16_card {
        uint16_t      base;
        uint8_t       irq;
        uint8_t       dma8;
        uint8_t       dma16;
        uint8_t       ver_major;
        uint8_t       ver_minor;
        bool          irq_registered;
        bool          dma8_busy;
        bool          dma16_busy;
        unsigned int  locked_rate;
        spinlock_t    reg_lock;
        sb16_stream_t streams[2];
        ulsa_card_t  *card;
} sb16_card_t;

typedef struct sb16_mixer_spec {
        const char  *name;
        uint8_t      reg_l;
        uint8_t      reg_r;
        uint8_t      shift_l;
        uint8_t      shift_r;
        uint8_t      max_val;
        bool         is_stereo;
        bool         is_boolean;
        bool         has_tlv;
        int          tlv_min_db;
        unsigned int tlv_step_db;
        unsigned int tlv[4];
} sb16_mixer_spec_t;

static const uint16_t sb16_probe_ports[] = {SB16_BASE_220, SB16_BASE_240, SB16_BASE_260, SB16_BASE_280};
static sb16_card_t   *sb16_active_dev;

/* Write a value to an SB16 CT1745 mixer register while holding reg_lock. */
static void sb16_mixer_write_locked(const sb16_card_t *sb, uint8_t reg, uint8_t val)
{
    outb((uint16_t)(sb->base + SB16_MIXER_ADDR), reg);
    outb((uint16_t)(sb->base + SB16_MIXER_DATA), val);
}

/* Read an SB16 CT1745 mixer register while holding reg_lock. */
static uint8_t sb16_mixer_read_locked(const sb16_card_t *sb, uint8_t reg)
{
    outb((uint16_t)(sb->base + SB16_MIXER_ADDR), reg);
    return inb((uint16_t)(sb->base + SB16_MIXER_DATA));
}

/* Wait for the DSP write port to accept a byte and write cmd. */
static int sb16_dsp_write(const sb16_card_t *sb, uint8_t val)
{
    for (int i = 0; i < 10000; i++) {
        if (!(inb((uint16_t)(sb->base + SB16_DSP_WRITE)) & 0x80U)) {
            outb((uint16_t)(sb->base + SB16_DSP_WRITE), val);
            return EOK;
        }
    }
    return -ETIMEDOUT;
}

/* Wait for a response byte from the DSP read port. */
static int sb16_dsp_read(const sb16_card_t *sb, uint8_t *val)
{
    for (int i = 0; i < 10000; i++) {
        if (inb((uint16_t)(sb->base + SB16_DSP_DATA_AVAIL)) & 0x80U) {
            *val = inb((uint16_t)(sb->base + SB16_DSP_READ));
            return EOK;
        }
    }
    return -ETIMEDOUT;
}

/* Reset the SB16 DSP and verify it responds with 0xAA. */
static int sb16_dsp_reset(const sb16_card_t *sb)
{
    outb((uint16_t)(sb->base + SB16_DSP_RESET), 1);
    usleep(10);
    outb((uint16_t)(sb->base + SB16_DSP_RESET), 0);

    for (int i = 0; i < 100; i++) {
        if (inb((uint16_t)(sb->base + SB16_DSP_DATA_AVAIL)) & 0x80U) {
            if (inb((uint16_t)(sb->base + SB16_DSP_READ)) == 0xaaU) return EOK;
        }
        usleep(10);
    }
    return -ENODEV;
}

/* Allocate a 64 KiB DMA buffer strictly below 16 MiB that does not cross a 64 KiB boundary. */
static int sb16_alloc_dma_buf(sb16_stream_t *st)
{
    uint64_t raw = alloc_frames_below(SB16_DMA_ALLOC_PAGES, 0x1000000ULL);
    if (!raw) return -ENOMEM;

    uint64_t aligned = (raw + SB16_DMA_BUFFER_SIZE - 1ULL) & ~(SB16_DMA_BUFFER_SIZE - 1ULL);
    if (aligned + SB16_DMA_BUFFER_SIZE > 0x1000000ULL) {
        free_frames(raw, SB16_DMA_ALLOC_PAGES);
        return -ENOMEM;
    }

    st->alloc_phys = raw;
    st->buf_phys   = (uint32_t)aligned;
    st->buf_virt   = phys_to_virt(aligned);
    memset(st->buf_virt, 0, SB16_DMA_BUFFER_SIZE);
    return EOK;
}

/* Handle an SB16 DMA completion interrupt and notify elapsed ULSA substreams outside reg_lock. */
static void sb16_handle_irq(void)
{
    sb16_card_t *sb = sb16_active_dev;
    if (!sb) return;

    ulsa_pcm_substream_t *notify8  = NULL;
    ulsa_pcm_substream_t *notify16 = NULL;

    uint64_t rflags = spin_lock_irqsave(&sb->reg_lock);
    uint8_t  status = sb16_mixer_read_locked(sb, SB_DSP4_IRQSTATUS);

    if (status & 0x01U) {
        (void)inb((uint16_t)(sb->base + SB16_DSP_ACK_8));
        for (size_t dir = 0; dir < 2; dir++) {
            const sb16_stream_t *st = &sb->streams[dir];
            if (st->running && !st->is_16 && st->substream) {
                notify8 = st->substream;
                break;
            }
        }
    }

    if (status & 0x02U) {
        (void)inb((uint16_t)(sb->base + SB16_DSP_ACK_16));
        for (size_t dir = 0; dir < 2; dir++) {
            const sb16_stream_t *st = &sb->streams[dir];
            if (st->running && st->is_16 && st->substream) {
                notify16 = st->substream;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&sb->reg_lock, rflags);

    if (notify8) ulsa_pcm_period_elapsed(notify8);
    if (notify16) ulsa_pcm_period_elapsed(notify16);
}

/* SB16 interrupt service routine entry wrapper. */
INTERRUPT_BEGIN static void sb16_interrupt_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    sb16_handle_irq();
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Open one SB16 playback or capture substream with DSP 4.x hardware constraints. */
static int sb16_pcm_open(ulsa_pcm_substream_t *substream)
{
    sb16_card_t        *sb = substream->pcm->private_data;
    sb16_stream_t      *st = &sb->streams[substream->stream];
    ulsa_pcm_runtime_t *rt = substream->runtime;

    rt->hw = (ulsa_pcm_hardware_t) {
        .info             = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED,
        .formats          = ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S16_LE) | ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_U16_LE) | ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_U8) | ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S8),
        .rates            = ULSA_PCM_RATE_CONTINUOUS | ULSA_PCM_RATE_8000 | ULSA_PCM_RATE_11025 | ULSA_PCM_RATE_16000 | ULSA_PCM_RATE_22050 | ULSA_PCM_RATE_32000 | ULSA_PCM_RATE_44100,
        .rate_min         = SB16_RATE_MIN,
        .rate_max         = SB16_RATE_MAX,
        .channels_min     = 1,
        .channels_max     = 2,
        .buffer_bytes_max = SB16_DMA_BUFFER_SIZE,
        .period_bytes_min = 64,
        .period_bytes_max = 32UL * 1024UL,
        .periods_min      = 2,
        .periods_max      = 1024,
    };

    int err = ulsa_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
    if (err < 0) return err;

    st->opened       = true;
    st->prepared     = false;
    st->running      = false;
    st->substream    = substream;
    rt->private_data = st;
    return EOK;
}

/* Release DMA channel ownership held by a substream if any. */
static void sb16_release_dma_channel(sb16_card_t *sb, sb16_stream_t *st)
{
    uint64_t rflags = spin_lock_irqsave(&sb->reg_lock);
    if (st->dma_ch == sb->dma16)
        sb->dma16_busy = false;
    else if (st->dma_ch == sb->dma8)
        sb->dma8_busy = false;
    st->dma_ch = 0xffU;
    spin_unlock_irqrestore(&sb->reg_lock, rflags);
}

/* Close an SB16 substream and release its resources. */
static int sb16_pcm_close(ulsa_pcm_substream_t *substream)
{
    sb16_card_t   *sb = substream->pcm->private_data;
    sb16_stream_t *st = substream->runtime->private_data;

    if (st) {
        st->opened    = false;
        st->prepared  = false;
        st->running   = false;
        st->substream = NULL;
        sb16_release_dma_channel(sb, st);
    }
    return EOK;
}

/* Bind the preallocated ISA DMA buffer and reserve the 8-bit or 16-bit DMA channel. */
static int sb16_pcm_hw_params(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    sb16_card_t        *sb    = substream->pcm->private_data;
    ulsa_pcm_runtime_t *rt    = substream->runtime;
    sb16_stream_t      *st    = rt->private_data;
    size_t              bytes = ulsa_params_buffer_bytes(params);
    snd_pcm_format_t    fmt   = ulsa_params_format(params);

    if (bytes == 0 || bytes > SB16_DMA_BUFFER_SIZE) return -EINVAL;

    bool is_16;
    if (fmt == SNDRV_PCM_FORMAT_S16_LE || fmt == SNDRV_PCM_FORMAT_U16_LE)
        is_16 = true;
    else if (fmt == SNDRV_PCM_FORMAT_U8 || fmt == SNDRV_PCM_FORMAT_S8)
        is_16 = false;
    else
        return -EINVAL;

    sb16_release_dma_channel(sb, st);

    uint64_t rflags = spin_lock_irqsave(&sb->reg_lock);
    bool    *busy   = is_16 ? &sb->dma16_busy : &sb->dma8_busy;
    if (*busy) {
        spin_unlock_irqrestore(&sb->reg_lock, rflags);
        return -EBUSY;
    }
    *busy      = true;
    st->is_16  = is_16;
    st->dma_ch = is_16 ? sb->dma16 : sb->dma8;
    spin_unlock_irqrestore(&sb->reg_lock, rflags);

    st->dma_bytes = (uint32_t)bytes;
    rt->dma_area  = st->buf_virt;
    rt->dma_addr  = st->buf_phys;
    rt->dma_bytes = bytes;
    return EOK;
}

/* Halt ISA DMA and clear unmanaged runtime DMA pointers on teardown. */
static int sb16_pcm_hw_free(ulsa_pcm_substream_t *substream)
{
    sb16_card_t        *sb = substream->pcm->private_data;
    ulsa_pcm_runtime_t *rt = substream->runtime;
    sb16_stream_t      *st = rt->private_data;

    if (st) {
        if (st->dma_ch != 0xffU) dma_disable(st->dma_ch);
        st->prepared  = false;
        st->running   = false;
        st->dma_bytes = 0;
        sb16_release_dma_channel(sb, st);
    }
    rt->dma_area  = NULL;
    rt->dma_addr  = 0;
    rt->dma_bytes = 0;
    return EOK;
}

/* Program the DSP sample rate, auto-init 8237 DMA controller, and paused DSP block command. */
static int sb16_pcm_prepare(ulsa_pcm_substream_t *substream)
{
    sb16_card_t              *sb           = substream->pcm->private_data;
    const ulsa_pcm_runtime_t *rt           = substream->runtime;
    sb16_stream_t            *st           = rt->private_data;
    int                       dir          = substream->stream;
    const sb16_stream_t      *other        = &sb->streams[dir == SNDRV_PCM_STREAM_PLAYBACK ? SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK];
    size_t                    buf_bytes    = ulsa_pcm_lib_buffer_bytes(substream);
    size_t                    period_bytes = ulsa_pcm_lib_period_bytes(substream);

    if (buf_bytes == 0 || buf_bytes > SB16_DMA_BUFFER_SIZE || period_bytes == 0 || st->dma_ch == 0xffU) return -EINVAL;
    if (other->running && sb->locked_rate != rt->rate) return -EBUSY;

    uint32_t count = st->is_16 ? (uint32_t)(period_bytes >> 1) : (uint32_t)period_bytes;
    if (count == 0) return -EINVAL;
    count--;

    uint8_t dma_mode = dir == SNDRV_PCM_STREAM_PLAYBACK ? SB16_DMA_MODE_PLAYBACK : SB16_DMA_MODE_CAPTURE;
    int     err      = dma_start(dma_mode, st->dma_ch, (uint32_t *)(uintptr_t)st->buf_phys, (uint32_t)buf_bytes);
    if (err < 0) return err;

    uint8_t dsp_mode;
    if (ulsa_pcm_format_signed(rt->format) == 0) {
        dsp_mode = rt->channels > 1 ? SB_DSP4_MODE_UNS_STEREO : SB_DSP4_MODE_UNS_MONO;
    } else {
        dsp_mode = rt->channels > 1 ? SB_DSP4_MODE_SIGN_STEREO : SB_DSP4_MODE_SIGN_MONO;
    }

    uint8_t cmd_start;
    if (dir == SNDRV_PCM_STREAM_PLAYBACK)
        cmd_start = st->is_16 ? SB_DSP4_OUT16_AI : SB_DSP4_OUT8_AI;
    else
        cmd_start = st->is_16 ? SB_DSP4_IN16_AI : SB_DSP4_IN8_AI;
    uint8_t cmd_off = st->is_16 ? SB_DSP_DMA16_OFF : SB_DSP_DMA8_OFF;

    uint64_t rflags = spin_lock_irqsave(&sb->reg_lock);
    if (!other->running) {
        sb->locked_rate = rt->rate;
        (void)sb16_dsp_write(sb, SB_DSP_SAMPLE_RATE_IN);
        (void)sb16_dsp_write(sb, (uint8_t)(rt->rate >> 8));
        (void)sb16_dsp_write(sb, (uint8_t)(rt->rate & 0xffU));
        (void)sb16_dsp_write(sb, SB_DSP_SAMPLE_RATE_OUT);
        (void)sb16_dsp_write(sb, (uint8_t)(rt->rate >> 8));
        (void)sb16_dsp_write(sb, (uint8_t)(rt->rate & 0xffU));
    }

    (void)sb16_dsp_write(sb, cmd_start);
    (void)sb16_dsp_write(sb, dsp_mode);
    (void)sb16_dsp_write(sb, (uint8_t)(count & 0xffU));
    (void)sb16_dsp_write(sb, (uint8_t)((count >> 8) & 0xffU));
    (void)sb16_dsp_write(sb, cmd_off);
    spin_unlock_irqrestore(&sb->reg_lock, rflags);

    st->dma_bytes = (uint32_t)buf_bytes;
    st->prepared  = true;
    return EOK;
}

/* Resume or halt SB16 DSP DMA in atomic context (substream->lock held). */
static int sb16_pcm_trigger(ulsa_pcm_substream_t *substream, int cmd)
{
    sb16_card_t         *sb    = substream->pcm->private_data;
    int                  dir   = substream->stream;
    sb16_stream_t       *st    = &sb->streams[dir];
    const sb16_stream_t *other = &sb->streams[dir == SNDRV_PCM_STREAM_PLAYBACK ? SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK];
    bool                 run;

    switch (cmd) {
        case ULSA_PCM_TRIGGER_START :
        case ULSA_PCM_TRIGGER_RESUME :
        case ULSA_PCM_TRIGGER_PAUSE_RELEASE :
            run = true;
            break;
        case ULSA_PCM_TRIGGER_STOP :
        case ULSA_PCM_TRIGGER_SUSPEND :
        case ULSA_PCM_TRIGGER_PAUSE_PUSH :
            run = false;
            break;
        default :
            return -EINVAL;
    }

    spin_lock(&sb->reg_lock);
    if (run) {
        st->running = true;
        (void)sb16_dsp_write(sb, st->is_16 ? SB_DSP_DMA16_ON : SB_DSP_DMA8_ON);
    } else {
        st->running = false;
        (void)sb16_dsp_write(sb, st->is_16 ? SB_DSP_DMA16_OFF : SB_DSP_DMA8_OFF);
        if (other->running && other->is_16 != st->is_16) (void)sb16_dsp_write(sb, other->is_16 ? SB_DSP_DMA16_ON : SB_DSP_DMA8_ON);
    }
    spin_unlock(&sb->reg_lock);
    return EOK;
}

/* Read current 8237 DMA position inside the SB16 ring buffer in frames. */
static snd_pcm_uframes_t sb16_pcm_pointer(ulsa_pcm_substream_t *substream)
{
    const sb16_stream_t *st = substream->runtime->private_data;
    if (!st->running || st->dma_ch == 0xffU || st->dma_bytes == 0) return 0;

    uint32_t bytes = dma_pointer(st->dma_ch, st->dma_bytes);
    return (snd_pcm_uframes_t)ulsa_bytes_to_frames(substream->runtime, bytes);
}

static const ulsa_pcm_ops_t sb16_pcm_ops = {
    .open      = sb16_pcm_open,
    .close     = sb16_pcm_close,
    .hw_params = sb16_pcm_hw_params,
    .hw_free   = sb16_pcm_hw_free,
    .prepare   = sb16_pcm_prepare,
    .trigger   = sb16_pcm_trigger,
    .pointer   = sb16_pcm_pointer,
};

/* ULSA kcontrol info callback for SB16 mixer volume and switch elements. */
static int sb16_mixer_info(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_info *uinfo)
{
    const sb16_mixer_spec_t *spec = (const sb16_mixer_spec_t *)kctl->private_value;
    uinfo->type                   = spec->is_boolean ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count                  = spec->is_stereo ? 2U : 1U;
    uinfo->value.integer.min      = 0;
    uinfo->value.integer.max      = spec->max_val;
    return EOK;
}

/* ULSA kcontrol get callback reading masked values from CT1745 mixer registers. */
static int sb16_mixer_get(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    sb16_card_t             *sb   = kctl->private_data;
    const sb16_mixer_spec_t *spec = (const sb16_mixer_spec_t *)kctl->private_value;

    uint64_t rflags                  = spin_lock_irqsave(&sb->reg_lock);
    uint8_t  vl                      = sb16_mixer_read_locked(sb, spec->reg_l);
    ucontrol->value.integer.value[0] = (long)((vl >> spec->shift_l) & spec->max_val);
    if (spec->is_stereo) {
        uint8_t vr                       = spec->reg_r == spec->reg_l ? vl : sb16_mixer_read_locked(sb, spec->reg_r);
        ucontrol->value.integer.value[1] = (long)((vr >> spec->shift_r) & spec->max_val);
    }
    spin_unlock_irqrestore(&sb->reg_lock, rflags);
    return EOK;
}

/* ULSA kcontrol put callback writing updated CT1745 mixer register bits. */
static int sb16_mixer_put(ulsa_kcontrol_t *kctl, struct snd_ctl_elem_value *ucontrol)
{
    sb16_card_t             *sb   = kctl->private_data;
    const sb16_mixer_spec_t *spec = (const sb16_mixer_spec_t *)kctl->private_value;

    long lv = ucontrol->value.integer.value[0];
    if (lv < 0 || lv > spec->max_val) return -EINVAL;

    long rv = lv;
    if (spec->is_stereo) {
        rv = ucontrol->value.integer.value[1];
        if (rv < 0 || rv > spec->max_val) return -EINVAL;
    }

    uint64_t rflags  = spin_lock_irqsave(&sb->reg_lock);
    uint8_t  ol      = sb16_mixer_read_locked(sb, spec->reg_l);
    uint8_t  mask_l  = (uint8_t)(spec->max_val << spec->shift_l);
    uint8_t  nl      = (uint8_t)((ol & ~mask_l) | ((uint8_t)lv << spec->shift_l));
    int      changed = 0;

    if (spec->is_stereo && spec->reg_r == spec->reg_l) {
        uint8_t mask_r = (uint8_t)(spec->max_val << spec->shift_r);
        nl             = (uint8_t)((nl & ~mask_r) | ((uint8_t)rv << spec->shift_r));
        if (nl != ol) {
            sb16_mixer_write_locked(sb, spec->reg_l, nl);
            changed = 1;
        }
    } else {
        if (nl != ol) {
            sb16_mixer_write_locked(sb, spec->reg_l, nl);
            changed = 1;
        }
        if (spec->is_stereo) {
            uint8_t or_val = sb16_mixer_read_locked(sb, spec->reg_r);
            uint8_t mask_r = (uint8_t)(spec->max_val << spec->shift_r);
            uint8_t nr     = (uint8_t)((or_val & ~mask_r) | ((uint8_t)rv << spec->shift_r));
            if (nr != or_val) {
                sb16_mixer_write_locked(sb, spec->reg_r, nr);
                changed = 1;
            }
        }
    }
    spin_unlock_irqrestore(&sb->reg_lock, rflags);
    return changed;
}

static sb16_mixer_spec_t sb16_mixer_specs[] = {
    {.name        = "Master Playback Volume",
     .reg_l       = SB_DSP4_MASTER_DEV,
     .reg_r       = SB_DSP4_MASTER_DEV + 1U,
     .shift_l     = 3,
     .shift_r     = 3,
     .max_val     = 31,
     .is_stereo   = true,
     .has_tlv     = true,
     .tlv_min_db  = -6200,
     .tlv_step_db = 200},
    {.name        = "PCM Playback Volume",
     .reg_l       = SB_DSP4_PCM_DEV,
     .reg_r       = SB_DSP4_PCM_DEV + 1U,
     .shift_l     = 3,
     .shift_r     = 3,
     .max_val     = 31,
     .is_stereo   = true,
     .has_tlv     = true,
     .tlv_min_db  = -6200,
     .tlv_step_db = 200},
    {.name        = "Synth Playback Volume",
     .reg_l       = SB_DSP4_SYNTH_DEV,
     .reg_r       = SB_DSP4_SYNTH_DEV + 1U,
     .shift_l     = 3,
     .shift_r     = 3,
     .max_val     = 31,
     .is_stereo   = true,
     .has_tlv     = true,
     .tlv_min_db  = -6200,
     .tlv_step_db = 200},
    {.name = "CD Playback Switch", .reg_l = SB_DSP4_OUTPUT_SW, .reg_r = SB_DSP4_OUTPUT_SW, .shift_l = 2, .shift_r = 1, .max_val = 1, .is_stereo = true, .is_boolean = true},
    {.name        = "CD Playback Volume",
     .reg_l       = SB_DSP4_CD_DEV,
     .reg_r       = SB_DSP4_CD_DEV + 1U,
     .shift_l     = 3,
     .shift_r     = 3,
     .max_val     = 31,
     .is_stereo   = true,
     .has_tlv     = true,
     .tlv_min_db  = -6200,
     .tlv_step_db = 200},
    {.name = "Mic Playback Switch", .reg_l = SB_DSP4_OUTPUT_SW, .reg_r = SB_DSP4_OUTPUT_SW, .shift_l = 0, .shift_r = 0, .max_val = 1, .is_stereo = false, .is_boolean = true},
    {.name        = "Mic Playback Volume",
     .reg_l       = SB_DSP4_MIC_DEV,
     .reg_r       = SB_DSP4_MIC_DEV,
     .shift_l     = 3,
     .shift_r     = 3,
     .max_val     = 31,
     .is_stereo   = false,
     .has_tlv     = true,
     .tlv_min_db  = -6200,
     .tlv_step_db = 200},
    {.name        = "Beep Volume",
     .reg_l       = SB_DSP4_SPEAKER_DEV,
     .reg_r       = SB_DSP4_SPEAKER_DEV,
     .shift_l     = 6,
     .shift_r     = 6,
     .max_val     = 3,
     .is_stereo   = false,
     .has_tlv     = true,
     .tlv_min_db  = -1800,
     .tlv_step_db = 600},
    {.name        = "Capture Volume",
     .reg_l       = SB_DSP4_IGAIN_DEV,
     .reg_r       = SB_DSP4_IGAIN_DEV + 1U,
     .shift_l     = 6,
     .shift_r     = 6,
     .max_val     = 3,
     .is_stereo   = true,
     .has_tlv     = true,
     .tlv_min_db  = 0,
     .tlv_step_db = 600},
    {.name        = "Playback Volume",
     .reg_l       = SB_DSP4_OGAIN_DEV,
     .reg_r       = SB_DSP4_OGAIN_DEV + 1U,
     .shift_l     = 6,
     .shift_r     = 6,
     .max_val     = 3,
     .is_stereo   = true,
     .has_tlv     = true,
     .tlv_min_db  = 0,
     .tlv_step_db = 600},
    {.name = "Line Playback Switch", .reg_l = SB_DSP4_OUTPUT_SW, .reg_r = SB_DSP4_OUTPUT_SW, .shift_l = 4, .shift_r = 3, .max_val = 1, .is_stereo = true, .is_boolean = true},
    {.name        = "Line Playback Volume",
     .reg_l       = SB_DSP4_LINE_DEV,
     .reg_r       = SB_DSP4_LINE_DEV + 1U,
     .shift_l     = 3,
     .shift_r     = 3,
     .max_val     = 31,
     .is_stereo   = true,
     .has_tlv     = true,
     .tlv_min_db  = -6200,
     .tlv_step_db = 200},
    {.name = "Mic Auto Gain", .reg_l = SB_DSP4_MIC_AGC, .reg_r = SB_DSP4_MIC_AGC, .shift_l = 0, .shift_r = 0, .max_val = 1, .is_stereo = false, .is_boolean = true},
    {.name = "Tone Control - Bass", .reg_l = SB_DSP4_BASS_DEV, .reg_r = SB_DSP4_BASS_DEV + 1U, .shift_l = 4, .shift_r = 4, .max_val = 15, .is_stereo = true},
    {.name = "Tone Control - Treble", .reg_l = SB_DSP4_TREBLE_DEV, .reg_r = SB_DSP4_TREBLE_DEV + 1U, .shift_l = 4, .shift_r = 4, .max_val = 15, .is_stereo = true},
};

/* Initialize default CT1745 mixer levels and register ULSA mixer controls. */
static void sb16_mixer_init(sb16_card_t *sb)
{
    uint64_t rflags = spin_lock_irqsave(&sb->reg_lock);
    sb16_mixer_write_locked(sb, SB_DSP4_MASTER_DEV, 31U << 3);
    sb16_mixer_write_locked(sb, SB_DSP4_MASTER_DEV + 1U, 31U << 3);
    sb16_mixer_write_locked(sb, SB_DSP4_PCM_DEV, 31U << 3);
    sb16_mixer_write_locked(sb, SB_DSP4_PCM_DEV + 1U, 31U << 3);
    sb16_mixer_write_locked(sb, SB_DSP4_SYNTH_DEV, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_SYNTH_DEV + 1U, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_CD_DEV, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_CD_DEV + 1U, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_LINE_DEV, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_LINE_DEV + 1U, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_MIC_DEV, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_SPEAKER_DEV, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_OUTPUT_SW, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_INPUT_LEFT, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_INPUT_RIGHT, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_IGAIN_DEV, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_IGAIN_DEV + 1U, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_OGAIN_DEV, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_OGAIN_DEV + 1U, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_MIC_AGC, 0);
    sb16_mixer_write_locked(sb, SB_DSP4_TREBLE_DEV, 8U << 4);
    sb16_mixer_write_locked(sb, SB_DSP4_TREBLE_DEV + 1U, 8U << 4);
    sb16_mixer_write_locked(sb, SB_DSP4_BASS_DEV, 8U << 4);
    sb16_mixer_write_locked(sb, SB_DSP4_BASS_DEV + 1U, 8U << 4);
    spin_unlock_irqrestore(&sb->reg_lock, rflags);

    for (size_t i = 0; i < sizeof(sb16_mixer_specs) / sizeof(sb16_mixer_specs[0]); i++) {
        sb16_mixer_spec_t *spec = &sb16_mixer_specs[i];
        if (spec->has_tlv) {
            spec->tlv[0] = SNDRV_CTL_TLVT_DB_SCALE;
            spec->tlv[1] = 2U * (unsigned int)sizeof(unsigned int);
            spec->tlv[2] = (unsigned int)spec->tlv_min_db;
            spec->tlv[3] = spec->tlv_step_db & SNDRV_CTL_TLVD_DB_SCALE_MASK;
        }

        ulsa_kcontrol_new_t tmpl = {
            .iface         = SNDRV_CTL_ELEM_IFACE_MIXER,
            .name          = spec->name,
            .access        = SNDRV_CTL_ELEM_ACCESS_READWRITE | (spec->has_tlv ? SNDRV_CTL_ELEM_ACCESS_TLV_READ : 0U),
            .count         = 1,
            .info          = sb16_mixer_info,
            .get           = sb16_mixer_get,
            .put           = sb16_mixer_put,
            .tlv           = spec->has_tlv ? spec->tlv : NULL,
            .private_value = (unsigned long)(uintptr_t)spec,
        };
        ulsa_kcontrol_t *kctl = ulsa_ctl_new1(&tmpl, sb);
        if (kctl) (void)ulsa_ctl_add(sb->card, kctl);
    }
}

/* Decode SB16 IRQ and 8/16-bit DMA configuration from mixer registers 0x80 and 0x81. */
static void sb16_detect_irq_dma(sb16_card_t *sb)
{
    uint64_t rflags  = spin_lock_irqsave(&sb->reg_lock);
    uint8_t  irq_reg = sb16_mixer_read_locked(sb, SB_DSP4_IRQSETUP);
    uint8_t  dma_reg = sb16_mixer_read_locked(sb, SB_DSP4_DMASETUP);

    /* Ensure IRQ 5 (the IO-APIC routed SB16 vector) is selected on hardware. */
    if (irq_reg != 0x02U) sb16_mixer_write_locked(sb, SB_DSP4_IRQSETUP, 0x02U);
    sb->irq = 5;

    sb->dma8 = 1;
    if (!(dma_reg & 0x02U)) {
        if (dma_reg & 0x08U)
            sb->dma8 = 3;
        else if (dma_reg & 0x01U)
            sb->dma8 = 0;
    }

    sb->dma16 = 5;
    if (!(dma_reg & 0x20U)) {
        if (dma_reg & 0x40U)
            sb->dma16 = 6;
        else if (dma_reg & 0x80U)
            sb->dma16 = 7;
    }

    spin_unlock_irqrestore(&sb->reg_lock, rflags);
}

/* Free SB16 DMA buffers, IRQ registration, and device state on card teardown. */
static void sb16_card_free(ulsa_card_t *card)
{
    sb16_card_t *sb = card ? card->private_data : NULL;
    if (!sb) return;

    if (sb->irq_registered) {
        unregister_interrupt_handler((uint16_t)(IRQ_0 + sb->irq));
        sb->irq_registered = false;
    }
    if (sb16_active_dev == sb) sb16_active_dev = NULL;

    for (size_t dir = 0; dir < 2; dir++) {
        if (sb->streams[dir].alloc_phys) free_frames(sb->streams[dir].alloc_phys, SB16_DMA_ALLOC_PAGES);
    }
    free(sb);
}

/* Detect and initialize the Sound Blaster 16 ISA sound card. */
void sb16_init(void)
{
    sb16_card_t *sb = calloc(1, sizeof(sb16_card_t));
    if (!sb) return;

    bool found = false;
    for (size_t i = 0; i < sizeof(sb16_probe_ports) / sizeof(sb16_probe_ports[0]); i++) {
        sb->base = sb16_probe_ports[i];
        if (sb16_dsp_reset(sb) != EOK) continue;
        if (sb16_dsp_write(sb, SB_DSP_GET_VERSION) != EOK) continue;

        uint8_t maj = 0, min = 0;
        if (sb16_dsp_read(sb, &maj) != EOK || sb16_dsp_read(sb, &min) != EOK) continue;
        if (maj < 4) continue;

        sb->ver_major = maj;
        sb->ver_minor = min;
        found         = true;
        break;
    }

    if (!found) {
        free(sb);
        return;
    }

    sb->streams[0].dma_ch = 0xffU;
    sb->streams[1].dma_ch = 0xffU;
    if (sb16_alloc_dma_buf(&sb->streams[SNDRV_PCM_STREAM_PLAYBACK]) < 0 || sb16_alloc_dma_buf(&sb->streams[SNDRV_PCM_STREAM_CAPTURE]) < 0) {
        for (size_t dir = 0; dir < 2; dir++) {
            if (sb->streams[dir].alloc_phys) free_frames(sb->streams[dir].alloc_phys, SB16_DMA_ALLOC_PAGES);
        }
        free(sb);
        return;
    }

    sb16_detect_irq_dma(sb);
    (void)sb16_dsp_write(sb, SB_DSP_SPEAKER_ON);

    ulsa_card_t *card = NULL;
    if (ulsa_card_new(-1, "SB16", &card) < 0) {
        for (size_t dir = 0; dir < 2; dir++) free_frames(sb->streams[dir].alloc_phys, SB16_DMA_ALLOC_PAGES);
        free(sb);
        return;
    }

    sb->card           = card;
    card->private_data = sb;
    card->private_free = sb16_card_free;
    strncpy(card->driver, "SB16", sizeof(card->driver) - 1);
    strncpy(card->shortname, "Sound Blaster 16", sizeof(card->shortname) - 1);
    snprintf(card->longname, sizeof(card->longname), "Sound Blaster 16 at 0x%x, irq %u, dma %u&%u", sb->base, sb->irq, sb->dma8, sb->dma16);
    strncpy(card->mixername, "CTL1745", sizeof(card->mixername) - 1);

    sb16_active_dev = sb;
    register_interrupt_handler((uint16_t)(IRQ_0 + sb->irq), (void *)sb16_interrupt_handler, 0, 0x8e);
    sb->irq_registered = true;

    sb16_mixer_init(sb);

    ulsa_pcm_t *pcm = NULL;
    if (ulsa_pcm_new(card, "SB16 DSP", 0, 1, 1, &pcm) < 0) {
        ulsa_card_free(card);
        return;
    }
    pcm->private_data = sb;
    ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &sb16_pcm_ops);
    ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &sb16_pcm_ops);

    if (ulsa_card_register(card) < 0) {
        ulsa_card_free(card);
        return;
    }

    plogk("sb16: DSP %u.%u at 0x%x irq %u dma8 %u dma16 %u registered\n", sb->ver_major, sb->ver_minor, sb->base, sb->irq, sb->dma8, sb->dma16);
}

/* Play a short square-wave beep through the SB16 DSP at freq Hz for ms milliseconds. */
void sb16_beep(uint16_t freq, uint32_t ms)
{
    sb16_card_t *sb = sb16_active_dev;
    if (!sb || freq == 0 || ms == 0) return;

    const unsigned int rate   = 22050U;
    uint32_t           period = rate / freq;
    if (period < 2U) period = 2U;

    sb16_stream_t *st = &sb->streams[SNDRV_PCM_STREAM_PLAYBACK];
    if (!st->buf_virt || st->opened || sb->dma8_busy) return;

    if (ms > 3000U) ms = 3000U;
    uint32_t samples = (rate * ms) / 1000U;
    if (samples == 0) samples = 1;
    if (samples > SB16_DMA_BUFFER_SIZE) samples = SB16_DMA_BUFFER_SIZE;

    for (uint32_t i = 0; i < samples; i++) st->buf_virt[i] = (i % period) < (period / 2U) ? 0xc0U : 0x40U;

    if (dma_start(0x48U, sb->dma8, (uint32_t *)(uintptr_t)st->buf_phys, samples) < 0) return;

    uint64_t rflags = spin_lock_irqsave(&sb->reg_lock);
    (void)sb16_dsp_write(sb, SB_DSP_SAMPLE_RATE_OUT);
    (void)sb16_dsp_write(sb, (uint8_t)(rate >> 8));
    (void)sb16_dsp_write(sb, (uint8_t)(rate & 0xffU));
    (void)sb16_dsp_write(sb, 0xc0U);
    (void)sb16_dsp_write(sb, SB_DSP4_MODE_UNS_MONO);
    (void)sb16_dsp_write(sb, (uint8_t)((samples - 1U) & 0xffU));
    (void)sb16_dsp_write(sb, (uint8_t)(((samples - 1U) >> 8) & 0xffU));
    spin_unlock_irqrestore(&sb->reg_lock, rflags);

    msleep(ms + 10U);
    dma_disable(sb->dma8);
}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_SB16
