/*
 *
 *      hda.c
 *      Intel High Definition Audio (HDA) controller driver
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <arch/tables/idt.h>
#include <drivers/audio/intel/hda.h>
#include <drivers/audio/ulsa/ulsa.h>
#include <drivers/bus/pci.h>
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
#include <sync/mutex.h>
#include <sync/spin_lock.h>

#if CONFIG_AUDIO && CONFIG_AUDIO_HDA

static hda_controller_t *hda_irq_slots[HDA_MAX_CONTROLLERS];
static spinlock_t        hda_irq_lock;

/* Read an 8-bit HDA controller register. */
static uint8_t hda_read8(const hda_controller_t *ctrl, uint32_t offset)
{
    return mmio_read8((const volatile uint8_t *)ctrl->bar.virt + offset);
}

/* Read a 16-bit HDA controller register. */
static uint16_t hda_read16(const hda_controller_t *ctrl, uint32_t offset)
{
    return mmio_read16((const volatile uint8_t *)ctrl->bar.virt + offset);
}

/* Read a 32-bit HDA controller register. */
static uint32_t hda_read32(const hda_controller_t *ctrl, uint32_t offset)
{
    return mmio_read32((const volatile uint8_t *)ctrl->bar.virt + offset);
}

/* Write an 8-bit HDA controller register. */
static void hda_write8(const hda_controller_t *ctrl, uint32_t offset, uint8_t val)
{
    mmio_write8((volatile uint8_t *)ctrl->bar.virt + offset, val);
}

/* Write a 16-bit HDA controller register. */
static void hda_write16(const hda_controller_t *ctrl, uint32_t offset, uint16_t val)
{
    mmio_write16((volatile uint8_t *)ctrl->bar.virt + offset, val);
}

/* Write a 32-bit HDA controller register. */
static void hda_write32(const hda_controller_t *ctrl, uint32_t offset, uint32_t val)
{
    mmio_write32((volatile uint8_t *)ctrl->bar.virt + offset, val);
}

/* Allocate DMA pages respecting controller 64-bit addressing capability. */
static uint64_t hda_alloc_dma_pages(const hda_controller_t *ctrl, size_t count)
{
    return ctrl->ok64 ? alloc_frames(count) : alloc_frames_below(count, 0x100000000ULL);
}

/* Drain completed and unsolicited RIRB entries with reg_lock held. */
static void hda_update_rirb_locked(hda_controller_t *ctrl)
{
    if (!ctrl->rirb || ctrl->rirb_entries == 0) return;

    uint16_t mask = (uint16_t)(ctrl->rirb_entries - 1U);
    uint16_t wp   = hda_read16(ctrl, HDA_RIRBWP) & mask;

    while (ctrl->rirb_rp != wp) {
        ctrl->rirb_rp = (ctrl->rirb_rp + 1U) & mask;
        uint32_t resp = ctrl->rirb[ctrl->rirb_rp].resp;
        uint32_t ex   = ctrl->rirb[ctrl->rirb_rp].resp_ex;
        uint8_t  cad  = (uint8_t)(ex & 0x0fU);

        if (ex & AC_RIRB_EX_UNSOL) {
            uint16_t next_tail = (uint16_t)((ctrl->unsol_tail + 1U) % HDA_UNSOL_RING_SIZE);
            if (next_tail != ctrl->unsol_head) {
                ctrl->unsol_ring[ctrl->unsol_tail].resp    = resp;
                ctrl->unsol_ring[ctrl->unsol_tail].resp_ex = ex;
                ctrl->unsol_tail                           = next_tail;
            }
        } else if (cad < HDA_MAX_CODECS) {
            ctrl->rirb_resp[cad] = resp;
            ctrl->rirb_got[cad]  = true;
        }
    }

    /* Clearing RIRBSTS unblocks QEMU's RINTCNT=1 stall after each response. */
    uint8_t rsts = hda_read8(ctrl, HDA_RIRBSTS);
    if (rsts & AZX_RBSTS_INT_MASK) hda_write8(ctrl, HDA_RIRBSTS, rsts & AZX_RBSTS_INT_MASK);
}

/* Dispatch a controller interrupt for IRQ slot and notify elapsed streams outside reg_lock. */
static void hda_interrupt_slot(size_t slot)
{
    uint64_t          irq_flags = spin_lock_irqsave(&hda_irq_lock);
    hda_controller_t *ctrl      = hda_irq_slots[slot];
    if (ctrl && !ctrl->stopping) {
        ctrl->irq_active++;
    } else {
        ctrl = NULL;
    }
    spin_unlock_irqrestore(&hda_irq_lock, irq_flags);

    if (ctrl) {
        ulsa_pcm_substream_t *notify[2] = {NULL, NULL};
        uint32_t              intsts    = hda_read32(ctrl, HDA_INTSTS);

        if (intsts != 0 && intsts != 0xffffffffU) {
            uint64_t rflags = spin_lock_irqsave(&ctrl->reg_lock);

            if (intsts & AZX_INT_CTRL_EN) {
                hda_update_rirb_locked(ctrl);
                uint16_t state_sts = hda_read16(ctrl, HDA_STATESTS);
                if (state_sts != 0) hda_write16(ctrl, HDA_STATESTS, state_sts);
                uint8_t corbsts = hda_read8(ctrl, HDA_CORBSTS);
                if (corbsts & AZX_CORBSTS_CMEI) hda_write8(ctrl, HDA_CORBSTS, AZX_CORBSTS_CMEI);
            }

            for (size_t dir = 0; dir < 2; dir++) {
                const hda_stream_t *st = &ctrl->streams[dir];
                if (st->index == HDA_STREAM_NONE || !(intsts & (1U << st->index))) continue;

                uint8_t sd_sts = hda_read8(ctrl, st->sd_base + HDA_SD_STS);
                if (sd_sts & HDA_SD_INT_MASK) hda_write8(ctrl, st->sd_base + HDA_SD_STS, sd_sts & HDA_SD_INT_MASK);
                if ((sd_sts & HDA_SD_STS_BCIS) && st->running && st->substream) notify[dir] = st->substream;
            }

            spin_unlock_irqrestore(&ctrl->reg_lock, rflags);

            for (size_t dir = 0; dir < 2; dir++) {
                if (notify[dir]) ulsa_pcm_period_elapsed(notify[dir]);
            }
        }

        irq_flags = spin_lock_irqsave(&hda_irq_lock);
        ctrl->irq_active--;
        spin_unlock_irqrestore(&hda_irq_lock, irq_flags);
    }
    send_eoi();
}

#    define HDA_IRQ_WRAPPER(n)                                                      \
        INTERRUPT_BEGIN static void hda_idt_interrupt_##n(interrupt_frame_t *frame) \
        {                                                                           \
            irq_enter_gs(frame);                                                    \
            hda_interrupt_slot(n);                                                  \
            irq_leave_gs(frame);                                                    \
        }                                                                           \
        INTERRUPT_END

HDA_IRQ_WRAPPER(0)
HDA_IRQ_WRAPPER(1)
HDA_IRQ_WRAPPER(2)
HDA_IRQ_WRAPPER(3)

static void *const hda_idt_handlers[HDA_MAX_CONTROLLERS] = {
    (void *)hda_idt_interrupt_0,
    (void *)hda_idt_interrupt_1,
    (void *)hda_idt_interrupt_2,
    (void *)hda_idt_interrupt_3,
};

/* Pack an HDA codec command word from address, NID, verb, and payload. */
static uint32_t hda_make_cmd(uint8_t cad, uint16_t nid, uint32_t verb, uint32_t param)
{
    uint32_t pmask = ((verb & 0x700U) == 0x700U) ? 0xffU : 0xffffU;
    return ((uint32_t)(cad & 0x0fU) << 28) | ((uint32_t)(nid & 0xffU) << 20) | ((verb & 0xfffU) << 8) | (param & pmask);
}

/* Send a command through the single-slot PIO Immediate Command registers. */
static int hda_verb_pio(hda_controller_t *ctrl, uint32_t cmd, uint32_t *resp)
{
    for (int t = 0; t < 1000; t++) {
        if (!(hda_read16(ctrl, HDA_IRS) & AZX_IRS_BUSY)) break;
        usleep(10);
    }
    if (hda_read16(ctrl, HDA_IRS) & AZX_IRS_BUSY) return -EBUSY;

    hda_write16(ctrl, HDA_IRS, AZX_IRS_VALID);
    hda_write32(ctrl, HDA_IC, cmd);
    hda_write16(ctrl, HDA_IRS, AZX_IRS_BUSY);

    for (int t = 0; t < 1000; t++) {
        uint16_t irs = hda_read16(ctrl, HDA_IRS);
        if ((irs & AZX_IRS_VALID) && !(irs & AZX_IRS_BUSY)) {
            if (resp) *resp = hda_read32(ctrl, HDA_IR);
            hda_write16(ctrl, HDA_IRS, AZX_IRS_VALID);
            return EOK;
        }
        usleep(10);
    }
    return -ETIMEDOUT;
}

/* Execute one HDA codec verb over CORB/RIRB with automatic PIO fallback on timeout. */
int hda_verb_exec(hda_controller_t *ctrl, uint8_t cad, uint16_t nid, uint32_t verb, uint32_t param, uint32_t *resp)
{
    if (!ctrl || cad >= HDA_MAX_CODECS) return -EINVAL;
    uint32_t cmd = hda_make_cmd(cad, nid, verb, param);

    mutex_lock(&ctrl->cmd_mutex);

    if (!ctrl->use_pio && ctrl->corb && ctrl->corb_entries > 0) {
        uint64_t rflags     = spin_lock_irqsave(&ctrl->reg_lock);
        uint16_t wp         = (uint16_t)((ctrl->corb_wp + 1U) & (ctrl->corb_entries - 1U));
        ctrl->corb_wp       = wp;
        ctrl->corb[wp]      = cmd;
        ctrl->rirb_got[cad] = false;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        hda_write16(ctrl, HDA_CORBWP, wp);
        spin_unlock_irqrestore(&ctrl->reg_lock, rflags);

        for (int spin = 0; spin < 200; spin++) {
            rflags = spin_lock_irqsave(&ctrl->reg_lock);
            hda_update_rirb_locked(ctrl);
            if (ctrl->rirb_got[cad]) {
                if (resp) *resp = ctrl->rirb_resp[cad];
                spin_unlock_irqrestore(&ctrl->reg_lock, rflags);
                mutex_unlock(&ctrl->cmd_mutex);
                return EOK;
            }
            spin_unlock_irqrestore(&ctrl->reg_lock, rflags);
            cpu_relax();
        }

        for (int wait = 0; wait < 500; wait++) {
            usleep(100);
            rflags = spin_lock_irqsave(&ctrl->reg_lock);
            hda_update_rirb_locked(ctrl);
            if (ctrl->rirb_got[cad]) {
                if (resp) *resp = ctrl->rirb_resp[cad];
                spin_unlock_irqrestore(&ctrl->reg_lock, rflags);
                mutex_unlock(&ctrl->cmd_mutex);
                return EOK;
            }
            spin_unlock_irqrestore(&ctrl->reg_lock, rflags);
        }
    }

    int ret = hda_verb_pio(ctrl, cmd, resp);
    mutex_unlock(&ctrl->cmd_mutex);
    return ret;
}

/* Drain and process any pending unsolicited jack-detect responses. */
void hda_process_unsol(hda_controller_t *ctrl)
{
    hda_unsol_event_t events[HDA_UNSOL_RING_SIZE];
    size_t            count = 0;

    uint64_t rflags = spin_lock_irqsave(&ctrl->reg_lock);
    while (ctrl->unsol_head != ctrl->unsol_tail && count < HDA_UNSOL_RING_SIZE) {
        events[count++]  = ctrl->unsol_ring[ctrl->unsol_head];
        ctrl->unsol_head = (uint16_t)((ctrl->unsol_head + 1U) % HDA_UNSOL_RING_SIZE);
    }
    spin_unlock_irqrestore(&ctrl->reg_lock, rflags);

    for (size_t i = 0; i < count; i++) { hda_codec_handle_unsol(ctrl, (uint8_t)(events[i].resp_ex & 0x0fU), events[i].resp); }
}

/* Negotiate 2/16/256 ring entry count for CORB or RIRB from hardware capability bits. */
static uint8_t hda_pick_ring_size(uint8_t size_reg, uint16_t *entries_out)
{
    uint8_t cap = (uint8_t)(size_reg >> 4);
    if (cap & 0x04U) {
        *entries_out = 256U;
        return 0x02U;
    }
    if (cap & 0x02U) {
        *entries_out = 16U;
        return 0x01U;
    }
    *entries_out = 2U;
    return 0x00U;
}

/* Perform a cold CRST cycle on the HDA controller and leave it in run state. */
static int hda_reset_controller(hda_controller_t *ctrl)
{
    hda_write32(ctrl, HDA_GCTL, hda_read32(ctrl, HDA_GCTL) & ~AZX_GCTL_RESET);
    for (int t = 0; t < 500; t++) {
        if (!(hda_read32(ctrl, HDA_GCTL) & AZX_GCTL_RESET)) break;
        usleep(100);
    }
    if (hda_read32(ctrl, HDA_GCTL) & AZX_GCTL_RESET) return -ETIMEDOUT;

    usleep(100);
    hda_write32(ctrl, HDA_GCTL, AZX_GCTL_RESET);
    for (int t = 0; t < 500; t++) {
        if (hda_read32(ctrl, HDA_GCTL) & AZX_GCTL_RESET) break;
        usleep(100);
    }
    if (!(hda_read32(ctrl, HDA_GCTL) & AZX_GCTL_RESET)) return -ETIMEDOUT;

    /* Per HDA spec wait >= 521 us for codecs to deassert reset and latch STATESTS. */
    usleep(1000);
    hda_write32(ctrl, HDA_GCTL, AZX_GCTL_RESET | AZX_GCTL_UNSOL);
    return EOK;
}

/* Allocate the shared CORB/POSBUF/RIRB page and initialize controller command DMA. */
static int hda_init_cmd_io(hda_controller_t *ctrl)
{
    uint64_t rb_phys = hda_alloc_dma_pages(ctrl, 1);
    if (!rb_phys) return -ENOMEM;

    uint8_t *rb_virt = phys_to_virt(rb_phys);
    memset(rb_virt, 0, PAGE_4K_SIZE);

    ctrl->rb_page_phys = rb_phys;
    ctrl->corb         = (volatile uint32_t *)rb_virt;
    ctrl->corb_phys    = rb_phys;
    ctrl->posbuf       = (volatile uint32_t *)(rb_virt + 1024);
    ctrl->posbuf_phys  = rb_phys + 1024;
    ctrl->rirb         = (volatile hda_rirb_entry_t *)(rb_virt + 2048);
    ctrl->rirb_phys    = rb_phys + 2048;

    hda_write8(ctrl, HDA_CORBCTL, 0);
    hda_write8(ctrl, HDA_RIRBCTL, 0);
    for (int t = 0; t < 100; t++) {
        if (!(hda_read8(ctrl, HDA_CORBCTL) & AZX_CORBCTL_RUN) && !(hda_read8(ctrl, HDA_RIRBCTL) & AZX_RBCTL_DMA_EN)) break;
        usleep(10);
    }

    uint8_t corbsz = hda_pick_ring_size(hda_read8(ctrl, HDA_CORBSIZE), &ctrl->corb_entries);
    hda_write8(ctrl, HDA_CORBSIZE, corbsz);
    hda_write32(ctrl, HDA_CORBLBASE, (uint32_t)(ctrl->corb_phys & 0xffffffffULL));
    hda_write32(ctrl, HDA_CORBUBASE, (uint32_t)(ctrl->corb_phys >> 32));
    hda_write16(ctrl, HDA_CORBWP, 0);
    ctrl->corb_wp = 0;

    hda_write16(ctrl, HDA_CORBRP, AZX_CORBRP_RST);
    for (int t = 0; t < 1000; t++) {
        if (hda_read16(ctrl, HDA_CORBRP) & AZX_CORBRP_RST) break;
        usleep(1);
    }
    hda_write16(ctrl, HDA_CORBRP, 0);
    for (int t = 0; t < 1000; t++) {
        if (hda_read16(ctrl, HDA_CORBRP) == 0) break;
        usleep(1);
    }

    uint8_t rirbsz = hda_pick_ring_size(hda_read8(ctrl, HDA_RIRBSIZE), &ctrl->rirb_entries);
    hda_write8(ctrl, HDA_RIRBSIZE, rirbsz);
    hda_write32(ctrl, HDA_RIRBLBASE, (uint32_t)(ctrl->rirb_phys & 0xffffffffULL));
    hda_write32(ctrl, HDA_RIRBUBASE, (uint32_t)(ctrl->rirb_phys >> 32));
    hda_write16(ctrl, HDA_RIRBWP, AZX_RIRBWP_RST);
    ctrl->rirb_rp = 0;
    hda_write16(ctrl, HDA_RINTCNT, 1);
    hda_write8(ctrl, HDA_RIRBSTS, AZX_RBSTS_INT_MASK);

    hda_write8(ctrl, HDA_RIRBCTL, AZX_RBCTL_DMA_EN | AZX_RBCTL_IRQ_EN);
    hda_write8(ctrl, HDA_CORBCTL, AZX_CORBCTL_RUN);

    hda_write32(ctrl, HDA_DPLBASE, (uint32_t)(ctrl->posbuf_phys & ~0x7fULL) | AZX_DPLBASE_ENABLE);
    hda_write32(ctrl, HDA_DPUBASE, (uint32_t)(ctrl->posbuf_phys >> 32));
    return EOK;
}

/* Reset a hardware stream descriptor engine following the HDA SRST sequence. */
static void hda_stream_reset(const hda_controller_t *ctrl, const hda_stream_t *st)
{
    uint32_t ctl = hda_read32(ctrl, st->sd_base + HDA_SD_CTL);
    ctl &= ~(HDA_SD_CTL_DMA_START | HDA_SD_CTL_INT_MASK);
    hda_write32(ctrl, st->sd_base + HDA_SD_CTL, ctl);

    for (int t = 0; t < 500; t++) {
        if (!(hda_read32(ctrl, st->sd_base + HDA_SD_CTL) & HDA_SD_CTL_DMA_START)) break;
        usleep(10);
    }

    hda_write32(ctrl, st->sd_base + HDA_SD_CTL, ctl | HDA_SD_CTL_SRST);
    for (int t = 0; t < 500; t++) {
        if (hda_read32(ctrl, st->sd_base + HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
        usleep(10);
    }

    usleep(40);
    hda_write32(ctrl, st->sd_base + HDA_SD_CTL, ctl & ~HDA_SD_CTL_SRST);
    for (int t = 0; t < 500; t++) {
        if (!(hda_read32(ctrl, st->sd_base + HDA_SD_CTL) & HDA_SD_CTL_SRST)) break;
        usleep(10);
    }

    hda_write8(ctrl, st->sd_base + HDA_SD_STS, HDA_SD_INT_MASK);
}

/* Open a playback or capture substream with probed codec constraints. */
static int hda_pcm_open(ulsa_pcm_substream_t *substream)
{
    hda_controller_t *ctrl  = substream->pcm->private_data;
    int               dir   = substream->stream;
    hda_codec_t      *codec = ctrl->primary_codec;
    hda_stream_t     *st    = &ctrl->streams[dir];

    if (!codec || st->index == HDA_STREAM_NONE) return -ENODEV;
    hda_process_unsol(ctrl);

    const hda_dir_caps_t *caps = dir == SNDRV_PCM_STREAM_PLAYBACK ? &codec->play_caps : &codec->cap_caps;
    ulsa_pcm_runtime_t   *rt   = substream->runtime;

    rt->hw = (ulsa_pcm_hardware_t) {
        .info             = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME,
        .formats          = caps->formats,
        .subformats       = caps->subformats,
        .rates            = caps->rates,
        .rate_min         = caps->rate_min,
        .rate_max         = caps->rate_max,
        .channels_min     = caps->channels_min,
        .channels_max     = caps->channels_max,
        .buffer_bytes_max = HDA_DMA_BUFFER_MAX,
        .period_bytes_min = HDA_PERIOD_BYTES_MIN,
        .period_bytes_max = HDA_PERIOD_BYTES_MAX,
        .periods_min      = HDA_PERIODS_MIN,
        .periods_max      = HDA_PERIODS_MAX,
    };

    int err = ulsa_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 128);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 128);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
    if (err < 0) return err;

    if (caps->maxbps < 32 && (caps->formats & ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S32_LE))) {
        err = ulsa_pcm_hw_constraint_msbits(rt, 0, 32, caps->maxbps);
        if (err < 0) return err;
    }

    st->opened       = true;
    st->prepared     = false;
    st->running      = false;
    st->substream    = substream;
    rt->private_data = st;
    return EOK;
}

/* Close an HDA substream and detach it from stream interrupt notifications. */
static int hda_pcm_close(ulsa_pcm_substream_t *substream)
{
    hda_stream_t *st = substream->runtime->private_data;
    if (st) {
        st->opened    = false;
        st->prepared  = false;
        st->running   = false;
        st->substream = NULL;
    }
    return EOK;
}

/* Validate chosen hardware parameters against the HDA stream format encoder. */
static int hda_pcm_hw_params(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    const hda_controller_t *ctrl  = substream->pcm->private_data;
    const hda_codec_t      *codec = ctrl->primary_codec;
    const hda_dir_caps_t   *caps  = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? &codec->play_caps : &codec->cap_caps;
    hda_stream_t           *st    = substream->runtime->private_data;

    int fmt = hda_calc_stream_format(ulsa_params_rate(params), ulsa_params_channels(params), ulsa_params_format(params), caps->maxbps);
    if (fmt < 0) return fmt;

    st->format_val = (uint16_t)fmt;
    return EOK;
}

/* Disassociate the codec converter and reset the hardware DMA stream on teardown. */
static int hda_pcm_hw_free(ulsa_pcm_substream_t *substream)
{
    hda_controller_t *ctrl = substream->pcm->private_data;
    hda_stream_t     *st   = substream->runtime->private_data;

    if (st) {
        st->prepared = false;
        st->running  = false;
        if (ctrl->primary_codec) hda_codec_cleanup_stream(ctrl, ctrl->primary_codec, substream->stream);
        hda_stream_reset(ctrl, st);
    }
    return EOK;
}

/* Populate the BDL, reset and program the HDA stream descriptor, and bind the converter. */
static int hda_pcm_prepare(ulsa_pcm_substream_t *substream)
{
    hda_controller_t         *ctrl = substream->pcm->private_data;
    const ulsa_pcm_runtime_t *rt   = substream->runtime;
    hda_stream_t             *st   = rt->private_data;
    const hda_dir_caps_t     *caps = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? &ctrl->primary_codec->play_caps : &ctrl->primary_codec->cap_caps;

    hda_process_unsol(ctrl);

    size_t       buf_bytes    = ulsa_pcm_lib_buffer_bytes(substream);
    size_t       period_bytes = ulsa_pcm_lib_period_bytes(substream);
    unsigned int periods      = rt->periods;
    if (periods < HDA_PERIODS_MIN || periods > HDA_BDL_ENTRIES || period_bytes == 0 || buf_bytes == 0) return -EINVAL;

    int fmt = hda_calc_stream_format(rt->rate, rt->channels, rt->format, caps->maxbps);
    if (fmt < 0) return fmt;

    st->buf_bytes    = buf_bytes;
    st->period_bytes = period_bytes;
    st->periods      = periods;
    st->format_val   = (uint16_t)fmt;

    for (unsigned int i = 0; i < periods; i++) {
        uint64_t addr        = rt->dma_addr + (size_t)i * period_bytes;
        st->bdl[i].addr_low  = (uint32_t)(addr & 0xffffffffULL);
        st->bdl[i].addr_high = (uint32_t)(addr >> 32);
        st->bdl[i].length    = (uint32_t)period_bytes;
        st->bdl[i].ioc       = 1U;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    hda_stream_reset(ctrl, st);

    uint64_t rflags = spin_lock_irqsave(&ctrl->reg_lock);
    hda_write32(ctrl, st->sd_base + HDA_SD_CTL, (uint32_t)st->tag << HDA_SD_CTL_TAG_SHIFT);
    hda_write32(ctrl, st->sd_base + HDA_SD_CBL, (uint32_t)buf_bytes);
    hda_write16(ctrl, st->sd_base + HDA_SD_LVI, (uint16_t)(periods - 1U));
    hda_write16(ctrl, st->sd_base + HDA_SD_FORMAT, st->format_val);
    hda_write32(ctrl, st->sd_base + HDA_SD_BDLPL, (uint32_t)(st->bdl_phys & 0xffffffffULL));
    hda_write32(ctrl, st->sd_base + HDA_SD_BDLPU, (uint32_t)(st->bdl_phys >> 32));
    hda_write8(ctrl, st->sd_base + HDA_SD_STS, HDA_SD_INT_MASK);
    spin_unlock_irqrestore(&ctrl->reg_lock, rflags);

    hda_codec_setup_stream(ctrl, ctrl->primary_codec, substream->stream, st->tag, st->format_val);
    st->prepared = true;
    return EOK;
}

/* Start or halt HDA stream DMA under substream->lock (atomic context). */
static int hda_pcm_trigger(ulsa_pcm_substream_t *substream, int cmd)
{
    hda_controller_t *ctrl = substream->pcm->private_data;
    hda_stream_t     *st   = substream->runtime->private_data;
    bool              run;

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

    spin_lock(&ctrl->reg_lock);
    uint32_t intctl = hda_read32(ctrl, HDA_INTCTL);
    if (run) {
        hda_write8(ctrl, st->sd_base + HDA_SD_STS, HDA_SD_INT_MASK);
        hda_write32(ctrl, HDA_INTCTL, intctl | (1U << st->index));
        hda_write32(ctrl, st->sd_base + HDA_SD_CTL, ((uint32_t)st->tag << HDA_SD_CTL_TAG_SHIFT) | HDA_SD_CTL_DMA_START | HDA_SD_CTL_INT_MASK);
        st->running = true;
    } else {
        hda_write32(ctrl, st->sd_base + HDA_SD_CTL, (uint32_t)st->tag << HDA_SD_CTL_TAG_SHIFT);
        hda_write32(ctrl, HDA_INTCTL, intctl & ~(1U << st->index));
        hda_write8(ctrl, st->sd_base + HDA_SD_STS, HDA_SD_INT_MASK);
        st->running = false;
    }
    spin_unlock(&ctrl->reg_lock);
    return EOK;
}

/* Read current HDA stream DMA link position in frames via SD_LPIB. */
static snd_pcm_uframes_t hda_pcm_pointer(ulsa_pcm_substream_t *substream)
{
    const hda_controller_t *ctrl = substream->pcm->private_data;
    const hda_stream_t     *st   = substream->runtime->private_data;
    if (!st->running || st->buf_bytes == 0) return 0;

    uint32_t lpib = hda_read32(ctrl, st->sd_base + HDA_SD_LPIB);
    if (lpib >= st->buf_bytes) lpib = 0;
    return (snd_pcm_uframes_t)ulsa_bytes_to_frames(substream->runtime, lpib);
}

static const ulsa_pcm_ops_t hda_pcm_ops = {
    .open      = hda_pcm_open,
    .close     = hda_pcm_close,
    .hw_params = hda_pcm_hw_params,
    .hw_free   = hda_pcm_hw_free,
    .prepare   = hda_pcm_prepare,
    .trigger   = hda_pcm_trigger,
    .pointer   = hda_pcm_pointer,
};

/* Free all HDA controller, stream, and codec resources when tearing down the ULSA card. */
static void hda_card_free(ulsa_card_t *card)
{
    hda_controller_t *ctrl = card ? card->private_data : NULL;
    if (!ctrl) return;

    ctrl->stopping  = true;
    uint64_t rflags = spin_lock_irqsave(&hda_irq_lock);
    if (ctrl->irq_slot < HDA_MAX_CONTROLLERS) hda_irq_slots[ctrl->irq_slot] = NULL;
    spin_unlock_irqrestore(&hda_irq_lock, rflags);

    if (ctrl->irq_state.mode) pci_free_irq(ctrl->pci, &ctrl->irq_state);
    spin_until_zero(&ctrl->irq_active, &hda_irq_lock);

    for (size_t dir = 0; dir < 2; dir++) {
        if (ctrl->streams[dir].bdl_phys) free_frames(ctrl->streams[dir].bdl_phys, 1);
    }
    for (uint8_t c = 0; c < HDA_MAX_CODECS; c++) {
        if (ctrl->codecs[c]) {
            if (ctrl->codecs[c]->widgets) free(ctrl->codecs[c]->widgets);
            free(ctrl->codecs[c]);
        }
    }
    if (ctrl->rb_page_phys) free_frames(ctrl->rb_page_phys, 1);
    free(ctrl);
}

/* Assign hardware stream indices/tags from GCAP counts and allocate their BDL pages. */
static int hda_setup_streams(hda_controller_t *ctrl, bool has_play, bool has_cap)
{
    for (size_t dir = 0; dir < 2; dir++) ctrl->streams[dir].index = HDA_STREAM_NONE;

    if (has_cap && ctrl->iss > 0) {
        hda_stream_t *cap = &ctrl->streams[SNDRV_PCM_STREAM_CAPTURE];
        cap->index        = 0;
        cap->tag          = 1;
        cap->sd_base      = HDA_SD_BASE(cap->index);
        cap->bdl_phys     = hda_alloc_dma_pages(ctrl, 1);
        if (!cap->bdl_phys) return -ENOMEM;
        cap->bdl = phys_to_virt(cap->bdl_phys);
        memset((void *)cap->bdl, 0, PAGE_4K_SIZE);
    }

    if (has_play && (ctrl->oss > 0 || ctrl->bss > 0)) {
        hda_stream_t *play = &ctrl->streams[SNDRV_PCM_STREAM_PLAYBACK];
        play->index        = ctrl->iss;
        play->tag          = (uint8_t)(ctrl->iss + 1U);
        play->sd_base      = HDA_SD_BASE(play->index);
        play->bdl_phys     = hda_alloc_dma_pages(ctrl, 1);
        if (!play->bdl_phys) return -ENOMEM;
        play->bdl = phys_to_virt(play->bdl_phys);
        memset((void *)play->bdl, 0, PAGE_4K_SIZE);
    }
    return EOK;
}

/* Probe one Intel HD Audio PCI controller, discover codecs, and register a ULSA card. */
static int hda_probe_controller(pci_device_cache_t *pci)
{
    ulsa_card_t *card = NULL;
    int          err  = ulsa_card_new(-1, "Intel", &card);
    if (err < 0) return err;

    hda_controller_t *ctrl = calloc(1, sizeof(hda_controller_t));
    if (!ctrl) {
        ulsa_card_free(card);
        return -ENOMEM;
    }
    ctrl->pci          = pci;
    ctrl->card         = card;
    ctrl->irq_slot     = HDA_MAX_CONTROLLERS;
    card->private_data = ctrl;
    card->private_free = hda_card_free;
    for (size_t dir = 0; dir < 2; dir++) ctrl->streams[dir].index = HDA_STREAM_NONE;

    pci_enable_device(pci, PCI_CMD_MEM | PCI_CMD_BUSMASTER);
    if (pci->vendor_id == PCI_VENDOR_INTEL) {
        pci_device_reg_t tcsel_reg = {.parent = pci, .offset = HDA_PCI_TCSEL};
        uint32_t         tcsel     = read_pci(tcsel_reg);
        (void)pci_write_config(tcsel_reg, tcsel & ~0x07U, 1);
    }

    if (pci_map_bar(pci, 0, &ctrl->bar) < 0) goto fail;

    uint16_t gcap     = hda_read16(ctrl, HDA_GCAP);
    ctrl->ok64        = (gcap & AZX_GCAP_64OK) != 0;
    ctrl->bss         = (uint8_t)((gcap >> 3) & 0x1fU);
    ctrl->iss         = (uint8_t)((gcap >> 8) & 0x0fU);
    ctrl->oss         = (uint8_t)((gcap >> 12) & 0x0fU);
    ctrl->num_streams = (uint8_t)(ctrl->iss + ctrl->oss + ctrl->bss);
    if (ctrl->num_streams > HDA_MAX_STREAMS) ctrl->num_streams = HDA_MAX_STREAMS;
    ctrl->maj_rev = hda_read8(ctrl, HDA_VMAJ);
    ctrl->min_rev = hda_read8(ctrl, HDA_VMIN);

    if (hda_reset_controller(ctrl) < 0) goto fail;
    if (hda_init_cmd_io(ctrl) < 0) goto fail;

    for (uint8_t s = 0; s < ctrl->num_streams; s++) hda_write8(ctrl, HDA_SD_BASE(s) + HDA_SD_STS, HDA_SD_INT_MASK);
    hda_write16(ctrl, HDA_WAKEEN, 0);

    uint16_t state_sts = (uint16_t)(hda_read16(ctrl, HDA_STATESTS) & 0x7fffU);
    hda_write16(ctrl, HDA_STATESTS, state_sts);
    if (state_sts == 0) {
        plogk("hda: No codecs indicated in STATESTS\n");
        goto fail;
    }

    uint64_t rflags = spin_lock_irqsave(&hda_irq_lock);
    size_t   slot   = HDA_MAX_CONTROLLERS;
    for (size_t i = 0; i < HDA_MAX_CONTROLLERS; i++) {
        if (!hda_irq_slots[i]) {
            slot             = i;
            hda_irq_slots[i] = ctrl;
            break;
        }
    }
    spin_unlock_irqrestore(&hda_irq_lock, rflags);
    if (slot == HDA_MAX_CONTROLLERS) goto fail;
    ctrl->irq_slot = (uint8_t)slot;

    pci_irq_request_t irq_req = {
        .modes         = PCI_IRQ_MSI | PCI_IRQ_LEGACY,
        .idt_handler   = hda_idt_handlers[slot],
        .legacy_base   = IRQ_0,
        .legacy_ioapic = 1,
    };
    if (pci_request_irq(pci, &irq_req, &ctrl->irq_state) < 0) goto fail;
    hda_write32(ctrl, HDA_INTCTL, AZX_INT_GLOBAL_EN | AZX_INT_CTRL_EN);

    for (uint8_t cad = 0; cad < HDA_MAX_CODECS; cad++) {
        if (!(state_sts & (1U << cad))) continue;
        hda_codec_t *codec = calloc(1, sizeof(hda_codec_t));
        if (!codec) continue;
        if (hda_codec_probe_and_init(ctrl, cad, codec) == EOK) {
            ctrl->codecs[cad] = codec;
            ctrl->num_codecs++;
            if (!ctrl->primary_codec) ctrl->primary_codec = codec;
        } else {
            if (codec->widgets) free(codec->widgets);
            free(codec);
        }
    }
    if (!ctrl->primary_codec) goto fail;

    hda_codec_t *pc       = ctrl->primary_codec;
    bool         has_play = pc->num_play_dacs > 0 && (ctrl->oss > 0 || ctrl->bss > 0);
    bool         has_cap  = pc->num_cap_adcs > 0 && ctrl->iss > 0;
    if (!has_play && !has_cap) goto fail;

    if (hda_setup_streams(ctrl, has_play, has_cap) < 0) goto fail;

    strncpy(card->driver, "HDA-Intel", sizeof(card->driver) - 1);
    strncpy(card->shortname, "HDA Intel", sizeof(card->shortname) - 1);
    unsigned int irq_num = ctrl->irq_state.mode == PCI_IRQ_LEGACY ? ctrl->irq_state.irq : (unsigned int)ctrl->irq_state.vector;
    snprintf(card->longname, sizeof(card->longname), "HDA Intel at 0x%llx irq %u", (unsigned long long)ctrl->bar.phys, irq_num);
    strncpy(card->mixername, "HDA Generic", sizeof(card->mixername) - 1);

    char comp[32];
    snprintf(comp, sizeof(comp), "HDA:%08x,%08x,%08x", pc->vendor_id, pc->subsystem_id, pc->rev_id);
    (void)ulsa_component_add(card, comp);

    ulsa_pcm_t *pcm = NULL;
    err             = ulsa_pcm_new(card, "HDA Generic", 0, has_play ? 1 : 0, has_cap ? 1 : 0, &pcm);
    if (err < 0) goto fail;
    pcm->private_data = ctrl;
    if (has_play) ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &hda_pcm_ops);
    if (has_cap) ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &hda_pcm_ops);
    ulsa_pcm_set_managed_buffer_all(pcm, ctrl->ok64 ? 0ULL : 0x100000000ULL, HDA_DMA_BUFFER_MAX);

    hda_codec_build_controls(ctrl, pc);

    err = ulsa_card_register(card);
    if (err < 0) goto fail;

    plogk("hda: rev %u.%u GCAP=0x%04x (%s) registered on IRQ vector %d\n", ctrl->maj_rev, ctrl->min_rev, gcap, ctrl->irq_state.mode == PCI_IRQ_MSI ? "MSI" : "INTx", ctrl->irq_state.vector);
    return EOK;

fail:
    ulsa_card_free(card);
    return -ENODEV;
}

/* Probe and initialize all Intel HD Audio PCI controllers. */
void hda_init(void)
{
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return;

    for (pci_device_cache_t *dev = cache->head; dev; dev = dev->next) {
        if (dev->class_code == PCI_CLASS_HDA) (void)hda_probe_controller(dev);
    }
}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_HDA
