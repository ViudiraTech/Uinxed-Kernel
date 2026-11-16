/*
 *
 *      intel8x0.c
 *      Intel 8x0 (ICH/nForce/SiS) AC'97 PCI audio controller driver
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <arch/tables/idt.h>
#include <drivers/audio/ac97/ac97.h>
#include <drivers/audio/ulsa/ulsa.h>
#include <drivers/bus/pci.h>
#include <drivers/firmware/apic.h>
#include <kernel/debug/printk.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

#if CONFIG_AUDIO && CONFIG_AUDIO_AC97

#    define ICH_MAX_DEVICES 4
#    define ICH_MAX_FRAGS   32
#    define ICH_LVI_MASK    0x1f

/* Busmaster channel register offsets */
#    define ICH_REG_PI_BASE 0x00
#    define ICH_REG_PO_BASE 0x10
#    define ICH_REG_MC_BASE 0x20

#    define ICH_REG_OFF_BDBAR 0x00
#    define ICH_REG_OFF_CIV   0x04
#    define ICH_REG_OFF_LVI   0x05
#    define ICH_REG_OFF_SR    0x06
#    define ICH_REG_OFF_PICB  0x08
#    define ICH_REG_OFF_PIV   0x0a
#    define ICH_REG_OFF_CR    0x0b

/* Status register bits (SR) */
#    define ICH_FIFOE 0x10
#    define ICH_BCIS  0x08
#    define ICH_LVBCI 0x04
#    define ICH_CELV  0x02
#    define ICH_DCH   0x01

/* Control register bits (CR) */
#    define ICH_IOCE      0x10
#    define ICH_FEIE      0x08
#    define ICH_LVBIE     0x04
#    define ICH_RESETREGS 0x02
#    define ICH_STARTBM   0x01

/* Global registers */
#    define ICH_REG_GLOB_CNT 0x2c
#    define ICH_PCM_20BIT    0x00400000U
#    define ICH_PCM_246_MASK 0x00300000U
#    define ICH_SIS_PCM_MASK 0x000000c0U
#    define ICH_ACLINK       0x00000008U
#    define ICH_AC97WARM     0x00000004U
#    define ICH_AC97COLD     0x00000002U
#    define ICH_REG_GLOB_STA 0x30
#    define ICH_TRI          0x20000000U
#    define ICH_TCR          0x10000000U
#    define ICH_SIS_TCR      0x00040000U
#    define ICH_RCS          0x00008000U
#    define ICH_SCR          0x00000200U
#    define ICH_PCR          0x00000100U
#    define ICH_MCINT        0x00000080U
#    define ICH_POINT        0x00000040U
#    define ICH_PIINT        0x00000020U
#    define ICH_NVSPINT      0x00000010U
#    define ICH_GSCI         0x00000001U
#    define ICH_REG_ACC_SEMA 0x34
#    define ICH_CAS          0x01
#    define ICH_REG_SDM      0x80
#    define ICH_LDI_MASK     0x03

/* Buffer descriptor entry flags */
#    define ICH_BD_IOC 0x80000000U

typedef enum {
    ICH_DEV_INTEL = 0,
    ICH_DEV_ICH4,
    ICH_DEV_SIS,
    ICH_DEV_NFORCE,
} ich_type_t;

typedef struct ich_pci_id {
        uint16_t    vendor;
        uint16_t    device;
        ich_type_t  type;
        const char *shortname;
} ich_pci_id_t;

static const ich_pci_id_t intel8x0_pci_ids[] = {
    {0x8086, 0x2415, ICH_DEV_INTEL,  "Intel 82801AA-ICH" },
    {0x8086, 0x2425, ICH_DEV_INTEL,  "Intel 82901AB-ICH0"},
    {0x8086, 0x2445, ICH_DEV_INTEL,  "Intel 82801BA-ICH2"},
    {0x8086, 0x2485, ICH_DEV_INTEL,  "Intel ICH3"        },
    {0x8086, 0x24c5, ICH_DEV_ICH4,   "Intel ICH4"        },
    {0x8086, 0x24d5, ICH_DEV_ICH4,   "Intel ICH5"        },
    {0x8086, 0x25a6, ICH_DEV_ICH4,   "Intel ESB"         },
    {0x8086, 0x266e, ICH_DEV_ICH4,   "Intel ICH6"        },
    {0x8086, 0x2698, ICH_DEV_ICH4,   "Intel ESB2"        },
    {0x8086, 0x27de, ICH_DEV_ICH4,   "Intel ICH7"        },
    {0x8086, 0x7195, ICH_DEV_INTEL,  "Intel 440MX"       },
    {0x10de, 0x01b1, ICH_DEV_NFORCE, "NVidia nForce"     },
    {0x10de, 0x006a, ICH_DEV_NFORCE, "NVidia nForce2"    },
    {0x10de, 0x00da, ICH_DEV_NFORCE, "NVidia nForce3"    },
    {0x10de, 0x00ea, ICH_DEV_NFORCE, "NVidia CK8S"       },
    {0x10de, 0x0059, ICH_DEV_NFORCE, "NVidia CK804"      },
    {0x1039, 0x7012, ICH_DEV_SIS,    "SiS SI7012"        },
};

typedef struct ich_io_region {
        bool              is_mmio;
        uint16_t          port;
        volatile uint8_t *mmio;
} ich_io_region_t;

typedef struct ich_bdl_entry {
        uint32_t addr;
        uint32_t ctl_len;
} __attribute__((packed)) ich_bdl_entry_t;

typedef struct ich_dev {
        uint32_t                  reg_offset;
        volatile ich_bdl_entry_t *bdbar;
        uint32_t                  bdbar_phys;
        ulsa_pcm_substream_t     *substream;
        uint32_t                  physbuf;
        size_t                    size;
        size_t                    fragsize;
        size_t                    fragsize1;
        size_t                    position;
        size_t                    last_pos;
        unsigned int              pos_shift;
        unsigned int              frags;
        unsigned int              lvi;
        unsigned int              lvi_frag;
        unsigned int              civ;
        int                       ack;
        int                       ack_reload;
        uint32_t                  roff_sr;
        uint32_t                  roff_picb;
        uint32_t                  int_sta_mask;
        bool                      prepared;
} ich_dev_t;

typedef struct intel8x0 {
        ich_type_t          type;
        pci_device_cache_t *pci;
        pci_irq_state_t     irq_state;
        uint8_t             irq_slot;
        volatile uint32_t   irq_active;
        bool                stopping;

        ich_io_region_t nam;
        ich_io_region_t nabm;

        ulsa_card_t  *card;
        ac97_bus_t   *ac97_bus;
        ac97_codec_t *ac97[3];
        unsigned int  ac97_sdin[3];
        unsigned int  max_codecs;
        unsigned int  ncodecs;
        unsigned int  codec_bit[3];
        uint32_t      codec_isr_bits;
        uint32_t      codec_ready_bits;
        bool          in_sdin_init;
        bool          in_ac97_init;

        ich_dev_t ichd[2]; // 0 = PCM in, 1 = PCM out
        uint64_t  bdbar_page_phys;
        uint32_t  int_sta_mask;

        spinlock_t reg_lock;
} intel8x0_t;

static intel8x0_t *intel8x0_irq_slots[ICH_MAX_DEVICES];
static spinlock_t  intel8x0_irq_lock;

/* Read an 8-bit NABM register. */
static uint8_t igetbyte(const intel8x0_t *chip, uint32_t offset)
{
    if (chip->nabm.is_mmio) return mmio_read8(chip->nabm.mmio + offset);
    return inb((uint16_t)(chip->nabm.port + offset));
}

/* Read a 16-bit NABM register. */
static uint16_t igetword(const intel8x0_t *chip, uint32_t offset)
{
    if (chip->nabm.is_mmio) return mmio_read16(chip->nabm.mmio + offset);
    return inw((uint16_t)(chip->nabm.port + offset));
}

/* Read a 32-bit NABM register. */
static uint32_t igetdword(const intel8x0_t *chip, uint32_t offset)
{
    if (chip->nabm.is_mmio) return mmio_read32(chip->nabm.mmio + offset);
    return inl((uint16_t)(chip->nabm.port + offset));
}

/* Write an 8-bit NABM register. */
static void iputbyte(const intel8x0_t *chip, uint32_t offset, uint8_t val)
{
    if (chip->nabm.is_mmio) {
        mmio_write8((volatile void *)(chip->nabm.mmio + offset), val);
    } else {
        outb((uint16_t)(chip->nabm.port + offset), val);
    }
}

/* Write a 32-bit NABM register. */
static void iputdword(const intel8x0_t *chip, uint32_t offset, uint32_t val)
{
    if (chip->nabm.is_mmio) {
        mmio_write32((volatile void *)(chip->nabm.mmio + offset), val);
    } else {
        outl((uint16_t)(chip->nabm.port + offset), val);
    }
}

/* Read a 16-bit NAM (AC'97 mixer/codec) register. */
static uint16_t iagetword(const intel8x0_t *chip, uint32_t offset)
{
    if (chip->nam.is_mmio) return mmio_read16(chip->nam.mmio + offset);
    return inw((uint16_t)(chip->nam.port + offset));
}

/* Write a 16-bit NAM (AC'97 mixer/codec) register. */
static void iaputword(const intel8x0_t *chip, uint32_t offset, uint16_t val)
{
    if (chip->nam.is_mmio) {
        mmio_write16((volatile void *)(chip->nam.mmio + offset), val);
    } else {
        outw((uint16_t)(chip->nam.port + offset), val);
    }
}

/* Wait for the AC'97 codec semaphore and verify codec ready. */
static int intel8x0_codec_semaphore(intel8x0_t *chip, unsigned int codec_num)
{
    if (codec_num > 2) return -EIO;
    uint32_t ready_bit = chip->in_sdin_init ? chip->codec_isr_bits : chip->codec_bit[chip->ac97_sdin[codec_num]];
    if ((igetdword(chip, ICH_REG_GLOB_STA) & ready_bit) == 0) return -EIO;

    for (int i = 0; i < 100; i++) {
        if (!(igetbyte(chip, ICH_REG_ACC_SEMA) & ICH_CAS)) return EOK;
        usleep(10);
    }
    (void)iagetword(chip, 0);
    return -EBUSY;
}

/* AC'97 bus write callback. */
static void intel8x0_codec_write(ac97_codec_t *codec, uint16_t reg, uint16_t val)
{
    intel8x0_t *chip = codec->private_data;
    (void)intel8x0_codec_semaphore(chip, codec->num);
    iaputword(chip, (uint32_t)reg + codec->num * 0x80U, val);
}

/* AC'97 bus read callback. */
static uint16_t intel8x0_codec_read(ac97_codec_t *codec, uint16_t reg)
{
    intel8x0_t *chip = codec->private_data;
    if (intel8x0_codec_semaphore(chip, codec->num) < 0) return 0xffffU;

    uint16_t res = iagetword(chip, (uint32_t)reg + codec->num * 0x80U);
    uint32_t sta = igetdword(chip, ICH_REG_GLOB_STA);
    if (sta & ICH_RCS) {
        iputdword(chip, ICH_REG_GLOB_STA, sta & ~(chip->codec_ready_bits | ICH_GSCI));
        res = 0xffffU;
    }
    return res;
}

/* Test-read codec register 0 to latch ICH4 SDIN routing. */
static void intel8x0_codec_read_test(intel8x0_t *chip, unsigned int codec_num)
{
    if (intel8x0_codec_semaphore(chip, codec_num) < 0) return;
    (void)iagetword(chip, codec_num * 0x80U);
    uint32_t sta = igetdword(chip, ICH_REG_GLOB_STA);
    if (sta & ICH_RCS) iputdword(chip, ICH_REG_GLOB_STA, sta & ~(chip->codec_ready_bits | ICH_GSCI));
}

/* Program BDL descriptors and reset stream registers prior to DMA start. */
static void intel8x0_setup_periods(intel8x0_t *chip, ich_dev_t *ichdev)
{
    uint32_t port = ichdev->reg_offset;

    iputbyte(chip, port + ICH_REG_OFF_CR, 0);
    iputbyte(chip, port + ICH_REG_OFF_CR, ICH_RESETREGS);
    for (int i = 0; i < 10000; i++) {
        if (!(igetbyte(chip, port + ICH_REG_OFF_CR) & ICH_RESETREGS)) break;
        usleep(1);
    }

    iputdword(chip, port + ICH_REG_OFF_BDBAR, ichdev->bdbar_phys);

    if (ichdev->size == ichdev->fragsize) {
        ichdev->ack_reload = ichdev->ack = 2;
        ichdev->fragsize1                = ichdev->fragsize >> 1;
        ichdev->frags                    = 2;
        for (unsigned int idx = 0; idx < ICH_MAX_FRAGS; idx += 2) {
            ichdev->bdbar[idx + 0].addr    = ichdev->physbuf;
            ichdev->bdbar[idx + 0].ctl_len = ICH_BD_IOC | (uint32_t)(ichdev->fragsize1 >> ichdev->pos_shift);
            ichdev->bdbar[idx + 1].addr    = ichdev->physbuf + (uint32_t)(ichdev->size >> 1);
            ichdev->bdbar[idx + 1].ctl_len = ICH_BD_IOC | (uint32_t)(ichdev->fragsize1 >> ichdev->pos_shift);
        }
    } else {
        ichdev->ack_reload = ichdev->ack = 1;
        ichdev->fragsize1                = ichdev->fragsize;
        ichdev->frags                    = (unsigned int)(ichdev->size / ichdev->fragsize);
        for (unsigned int idx = 0; idx < ICH_MAX_FRAGS; idx++) {
            uint32_t off               = (uint32_t)(((size_t)idx * ichdev->fragsize) % ichdev->size);
            ichdev->bdbar[idx].addr    = ichdev->physbuf + off;
            ichdev->bdbar[idx].ctl_len = ICH_BD_IOC | (uint32_t)(ichdev->fragsize >> ichdev->pos_shift);
        }
    }

    ichdev->lvi      = ICH_LVI_MASK;
    ichdev->civ      = 0;
    ichdev->lvi_frag = ICH_LVI_MASK % ichdev->frags;
    ichdev->position = 0;
    ichdev->last_pos = 0;

    iputbyte(chip, port + ICH_REG_OFF_LVI, (uint8_t)ichdev->lvi);
    iputbyte(chip, port + ichdev->roff_sr, ICH_FIFOE | ICH_BCIS | ICH_LVBCI);
}

/* Process busmaster completion for one channel and notify ULSA outside reg_lock. */
static void intel8x0_update(intel8x0_t *chip, ich_dev_t *ichdev)
{
    uint32_t port = ichdev->reg_offset;
    bool     ack  = false;

    uint64_t rflags = spin_lock_irqsave(&chip->reg_lock);
    if (ichdev->prepared) {
        uint8_t status = igetbyte(chip, port + ichdev->roff_sr);
        uint8_t civ    = igetbyte(chip, port + ICH_REG_OFF_CIV) & ICH_LVI_MASK;
        int     step   = 0;

        if (status & (ICH_BCIS | ICH_LVBCI)) {
            if (civ == ichdev->civ) {
                step        = 1;
                ichdev->civ = (ichdev->civ + 1U) & ICH_LVI_MASK;
            } else {
                step = (int)civ - (int)ichdev->civ;
                if (step < 0) step += ICH_MAX_FRAGS;
                ichdev->civ = civ;
            }
        }

        if (step > 0) {
            ichdev->position = (ichdev->position + (size_t)step * ichdev->fragsize1) % ichdev->size;
            for (int i = 0; i < step; i++) {
                ichdev->lvi                     = (ichdev->lvi + 1U) & ICH_LVI_MASK;
                ichdev->lvi_frag                = (ichdev->lvi_frag + 1U) % ichdev->frags;
                ichdev->bdbar[ichdev->lvi].addr = ichdev->physbuf + (uint32_t)(ichdev->lvi_frag * ichdev->fragsize1);
                if (--ichdev->ack == 0) {
                    ichdev->ack = ichdev->ack_reload;
                    ack         = true;
                }
            }
            iputbyte(chip, port + ICH_REG_OFF_LVI, (uint8_t)ichdev->lvi);
        }

        if (status & (ICH_FIFOE | ICH_BCIS | ICH_LVBCI)) { iputbyte(chip, port + ichdev->roff_sr, status & (ICH_FIFOE | ICH_BCIS | ICH_LVBCI)); }
    }
    spin_unlock_irqrestore(&chip->reg_lock, rflags);

    if (ack && ichdev->substream) ulsa_pcm_period_elapsed(ichdev->substream);
}

/* Dispatch controller interrupt to active DMA channels and acknowledge GLOB_STA. */
static void intel8x0_interrupt_slot(size_t slot)
{
    uint64_t    rflags = spin_lock_irqsave(&intel8x0_irq_lock);
    intel8x0_t *chip   = intel8x0_irq_slots[slot];
    if (chip && !chip->stopping) {
        chip->irq_active++;
    } else {
        chip = NULL;
    }
    spin_unlock_irqrestore(&intel8x0_irq_lock, rflags);

    if (chip) {
        uint32_t status = igetdword(chip, ICH_REG_GLOB_STA);
        if (status != 0xffffffffU && (status & chip->int_sta_mask)) {
            for (size_t i = 0; i < 2; i++) {
                if (status & chip->ichd[i].int_sta_mask) intel8x0_update(chip, &chip->ichd[i]);
            }
            iputdword(chip, ICH_REG_GLOB_STA, status & chip->int_sta_mask);
        } else if (status != 0xffffffffU && status != 0) {
            iputdword(chip, ICH_REG_GLOB_STA, status);
        }

        rflags = spin_lock_irqsave(&intel8x0_irq_lock);
        chip->irq_active--;
        spin_unlock_irqrestore(&intel8x0_irq_lock, rflags);
    }
    send_eoi();
}

#    define ICH_IRQ_WRAPPER(n)                                                           \
        INTERRUPT_BEGIN static void intel8x0_idt_interrupt_##n(interrupt_frame_t *frame) \
        {                                                                                \
            irq_enter_gs(frame);                                                         \
            intel8x0_interrupt_slot(n);                                                  \
            irq_leave_gs(frame);                                                         \
        }                                                                                \
        INTERRUPT_END

ICH_IRQ_WRAPPER(0)
ICH_IRQ_WRAPPER(1)
ICH_IRQ_WRAPPER(2)
ICH_IRQ_WRAPPER(3)

static void *const intel8x0_idt_handlers[ICH_MAX_DEVICES] = {
    (void *)intel8x0_idt_interrupt_0,
    (void *)intel8x0_idt_interrupt_1,
    (void *)intel8x0_idt_interrupt_2,
    (void *)intel8x0_idt_interrupt_3,
};

/* Map AC'97 codec rate bitmask to inclusive min and max Hz bounds. */
static void intel8x0_rates_minmax(unsigned int rates, unsigned int *min_out, unsigned int *max_out)
{
    typedef struct rate_map {
            unsigned int bit;
            unsigned int hz;
    } rate_map_t;
    static const rate_map_t table[] = {
        {ULSA_PCM_RATE_8000,  8000 },
        {ULSA_PCM_RATE_11025, 11025},
        {ULSA_PCM_RATE_16000, 16000},
        {ULSA_PCM_RATE_22050, 22050},
        {ULSA_PCM_RATE_32000, 32000},
        {ULSA_PCM_RATE_44100, 44100},
        {ULSA_PCM_RATE_48000, 48000},
    };
    unsigned int rmin = 0, rmax = 0;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (rates & table[i].bit) {
            if (!rmin || table[i].hz < rmin) rmin = table[i].hz;
            if (table[i].hz > rmax) rmax = table[i].hz;
        }
    }
    *min_out = rmin ? rmin : 48000U;
    *max_out = rmax ? rmax : 48000U;
}

/* Open an Intel 8x0 playback or capture substream with codec rate constraints. */
static int intel8x0_pcm_open(ulsa_pcm_substream_t *substream)
{
    intel8x0_t         *chip    = substream->pcm->private_data;
    int                 is_play = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
    ich_dev_t          *ichdev  = &chip->ichd[is_play ? 1 : 0];
    ulsa_pcm_runtime_t *rt      = substream->runtime;
    unsigned int        rates   = ULSA_PCM_RATE_48000;
    if (chip->ac97[0]) rates = is_play ? chip->ac97[0]->rates_dac : chip->ac97[0]->rates_adc;
    unsigned int rmin = 48000U, rmax = 48000U;

    intel8x0_rates_minmax(rates, &rmin, &rmax);

    rt->hw = (ulsa_pcm_hardware_t) {
        .info             = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME,
        .formats          = ULSA_PCM_FMTBIT(SNDRV_PCM_FORMAT_S16_LE),
        .rates            = rates,
        .rate_min         = rmin,
        .rate_max         = rmax,
        .channels_min     = 2,
        .channels_max     = 2,
        .buffer_bytes_max = 128UL * 1024UL,
        .period_bytes_min = 32,
        .period_bytes_max = 64UL * 1024UL,
        .periods_min      = 1,
        .periods_max      = 1024,
    };

    int err = ulsa_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
    if (err < 0) return err;

    ichdev->substream = substream;
    rt->private_data  = ichdev;
    return EOK;
}

/* Close an Intel 8x0 substream and clear its backpointer. */
static int intel8x0_pcm_close(ulsa_pcm_substream_t *substream)
{
    ich_dev_t *ichdev = substream->runtime->private_data;
    if (ichdev) {
        ichdev->prepared  = false;
        ichdev->substream = NULL;
    }
    return EOK;
}

/* Program AC'97 codec sample rate during hw_params. */
static int intel8x0_pcm_hw_params(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    intel8x0_t *chip   = substream->pcm->private_data;
    ich_dev_t  *ichdev = substream->runtime->private_data;
    ichdev->prepared   = false;
    if (!chip->ac97[0]) return -ENODEV;
    uint16_t reg = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? AC97_PCM_FRONT_DAC_RATE : AC97_PCM_LR_ADC_RATE;
    return ac97_set_rate(chip->ac97[0], reg, ulsa_params_rate(params));
}

/* Tear down per-hw_params state. */
static int intel8x0_pcm_hw_free(ulsa_pcm_substream_t *substream)
{
    ich_dev_t *ichdev = substream->runtime->private_data;
    if (ichdev) ichdev->prepared = false;
    return EOK;
}

/* Configure global channel count and BDL entries before starting DMA. */
static int intel8x0_pcm_prepare(ulsa_pcm_substream_t *substream)
{
    intel8x0_t         *chip   = substream->pcm->private_data;
    ulsa_pcm_runtime_t *rt     = substream->runtime;
    ich_dev_t          *ichdev = rt->private_data;

    ichdev->physbuf  = (uint32_t)rt->dma_addr;
    ichdev->size     = ulsa_pcm_lib_buffer_bytes(substream);
    ichdev->fragsize = ulsa_pcm_lib_period_bytes(substream);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        uint64_t rflags = spin_lock_irqsave(&chip->reg_lock);
        uint32_t cnt    = igetdword(chip, ICH_REG_GLOB_CNT);
        if (chip->type == ICH_DEV_SIS) {
            cnt &= ~ICH_SIS_PCM_MASK;
        } else {
            cnt &= ~(ICH_PCM_246_MASK | ICH_PCM_20BIT);
        }
        iputdword(chip, ICH_REG_GLOB_CNT, cnt);
        spin_unlock_irqrestore(&chip->reg_lock, rflags);
    }

    uint64_t rflags = spin_lock_irqsave(&chip->reg_lock);
    intel8x0_setup_periods(chip, ichdev);
    ichdev->prepared = true;
    spin_unlock_irqrestore(&chip->reg_lock, rflags);
    return EOK;
}

/* Start, pause, resume, or halt busmaster DMA under substream->lock. */
static int intel8x0_pcm_trigger(ulsa_pcm_substream_t *substream, int cmd)
{
    intel8x0_t *chip   = substream->pcm->private_data;
    ich_dev_t  *ichdev = substream->runtime->private_data;
    uint32_t    port   = ichdev->reg_offset;
    uint8_t     val;

    switch (cmd) {
        case ULSA_PCM_TRIGGER_START :
        case ULSA_PCM_TRIGGER_RESUME :
        case ULSA_PCM_TRIGGER_PAUSE_RELEASE :
            val = ICH_IOCE | ICH_STARTBM;
            break;
        case ULSA_PCM_TRIGGER_STOP :
        case ULSA_PCM_TRIGGER_SUSPEND :
            val = 0;
            break;
        case ULSA_PCM_TRIGGER_PAUSE_PUSH :
            val = ICH_IOCE;
            break;
        default :
            return -EINVAL;
    }

    spin_lock(&chip->reg_lock);
    if (cmd == ULSA_PCM_TRIGGER_START || cmd == ULSA_PCM_TRIGGER_RESUME || cmd == ULSA_PCM_TRIGGER_PAUSE_RELEASE) { ichdev->last_pos = ichdev->position; }
    iputbyte(chip, port + ICH_REG_OFF_CR, val);
    if (cmd == ULSA_PCM_TRIGGER_STOP || cmd == ULSA_PCM_TRIGGER_SUSPEND) {
        for (int i = 0; i < 10000; i++) {
            if (igetbyte(chip, port + ichdev->roff_sr) & ICH_DCH) break;
            usleep(1);
        }
        iputbyte(chip, port + ICH_REG_OFF_CR, ICH_RESETREGS);
    }
    spin_unlock(&chip->reg_lock);
    return EOK;
}

/* Read hardware DMA ring position in frames. */
static snd_pcm_uframes_t intel8x0_pcm_pointer(ulsa_pcm_substream_t *substream)
{
    intel8x0_t *chip   = substream->pcm->private_data;
    ich_dev_t  *ichdev = substream->runtime->private_data;
    uint32_t    port   = ichdev->reg_offset;

    spin_lock(&chip->reg_lock);
    uint8_t  civ      = 0;
    uint16_t picb     = 0;
    uint8_t  sr       = 0;
    size_t   position = ichdev->position;

    for (int retry = 0; retry < 10; retry++) {
        civ      = igetbyte(chip, port + ICH_REG_OFF_CIV) & ICH_LVI_MASK;
        sr       = igetbyte(chip, port + ichdev->roff_sr);
        picb     = igetword(chip, port + ichdev->roff_picb);
        position = ichdev->position;
        if (sr & ICH_DCH) break;
        if (picb == 0) {
            usleep(10);
            continue;
        }
        if (civ == (igetbyte(chip, port + ICH_REG_OFF_CIV) & ICH_LVI_MASK)) break;
    }

    size_t ptr = ichdev->last_pos;
    if (!(sr & ICH_DCH) && picb != 0) {
        size_t rem_bytes  = (size_t)picb << ichdev->pos_shift;
        size_t done_in_bd = rem_bytes <= ichdev->fragsize1 ? ichdev->fragsize1 - rem_bytes : 0;
        int    step_bds   = (int)civ - (int)ichdev->civ;
        if (step_bds < 0) step_bds += ICH_MAX_FRAGS;

        ptr = (position + (size_t)step_bds * ichdev->fragsize1 + done_in_bd) % ichdev->size;
        if (ptr < ichdev->last_pos && (position / ichdev->fragsize1) == (ichdev->last_pos / ichdev->fragsize1) && step_bds == 0) { ptr = ichdev->last_pos; }
    }
    ichdev->last_pos = ptr;
    spin_unlock(&chip->reg_lock);

    if (ptr >= ichdev->size) return 0;
    return (snd_pcm_uframes_t)ulsa_bytes_to_frames(substream->runtime, ptr);
}

static const ulsa_pcm_ops_t intel8x0_pcm_ops = {
    .open      = intel8x0_pcm_open,
    .close     = intel8x0_pcm_close,
    .hw_params = intel8x0_pcm_hw_params,
    .hw_free   = intel8x0_pcm_hw_free,
    .prepare   = intel8x0_pcm_prepare,
    .trigger   = intel8x0_pcm_trigger,
    .pointer   = intel8x0_pcm_pointer,
};

/* Reset AC-link codecs and busmaster DMA channels on chip initialization. */
static int intel8x0_chip_init(intel8x0_t *chip)
{
    uint32_t sta = igetdword(chip, ICH_REG_GLOB_STA);
    iputdword(chip, ICH_REG_GLOB_STA, sta & (ICH_RCS | ICH_MCINT | ICH_POINT | ICH_PIINT | ICH_NVSPINT));

    uint32_t cnt = igetdword(chip, ICH_REG_GLOB_CNT);
    cnt &= ~(ICH_ACLINK | ICH_PCM_246_MASK);
    iputdword(chip, ICH_REG_GLOB_CNT, cnt & ~ICH_AC97COLD);
    cnt = igetdword(chip, ICH_REG_GLOB_CNT);
    usleep(10);
    iputdword(chip, ICH_REG_GLOB_CNT, cnt | ICH_AC97COLD);
    msleep(1);

    uint32_t ready = 0;
    for (int i = 0; i < 200; i++) {
        ready = igetdword(chip, ICH_REG_GLOB_STA) & chip->codec_isr_bits;
        if (ready) break;
        msleep(5);
    }
    if (!ready) {
        plogk("ac97: Primary codec not ready (GLOB_STA=0x%08x)\n", igetdword(chip, ICH_REG_GLOB_STA));
        return -EIO;
    }

    if (chip->type == ICH_DEV_SIS) iputbyte(chip, 0x4c, igetbyte(chip, 0x4c) | 1);
    (void)iagetword(chip, 0);

    for (size_t i = 0; i < 2; i++) {
        uint32_t port = chip->ichd[i].reg_offset;
        iputbyte(chip, port + ICH_REG_OFF_CR, 0);
        iputbyte(chip, port + ICH_REG_OFF_CR, ICH_RESETREGS);
        for (int t = 0; t < 10000; t++) {
            if (!(igetbyte(chip, port + ICH_REG_OFF_CR) & ICH_RESETREGS)) break;
            usleep(1);
        }
    }
    return EOK;
}

/* Free resources owned by an Intel 8x0 ULSA card. */
static void intel8x0_card_free(ulsa_card_t *card)
{
    intel8x0_t *chip = card ? card->private_data : NULL;
    if (!chip) return;

    chip->stopping  = true;
    uint64_t rflags = spin_lock_irqsave(&intel8x0_irq_lock);
    if (chip->irq_slot < ICH_MAX_DEVICES) intel8x0_irq_slots[chip->irq_slot] = NULL;
    spin_unlock_irqrestore(&intel8x0_irq_lock, rflags);

    if (chip->irq_state.mode) pci_free_irq(chip->pci, &chip->irq_state);
    spin_until_zero(&chip->irq_active, &intel8x0_irq_lock);

    if (chip->ac97_bus) ac97_bus_destroy(chip->ac97_bus);
    if (chip->bdbar_page_phys) free_frames(chip->bdbar_page_phys, 1);
    free(chip);
}

/* Map an AC'97 NAM/NABM BAR preferring MMIO when present. */
static int intel8x0_setup_bars(intel8x0_t *chip, pci_device_cache_t *pci)
{
    base_address_register_t bar2 = get_base_address_register(pci, 2);
    if (bar2.type == mem_mapping && bar2.size != 0) {
        pci_bar_t mapped;
        if (pci_map_bar(pci, 2, &mapped) < 0) return -ENODEV;
        chip->nam.is_mmio = true;
        chip->nam.mmio    = mapped.virt;
    } else {
        base_address_register_t bar0 = get_base_address_register(pci, 0);
        if (bar0.type != input_output || bar0.size == 0) return -ENODEV;
        chip->nam.is_mmio = false;
        chip->nam.port    = (uint16_t)(uintptr_t)bar0.address;
    }

    base_address_register_t bar3 = get_base_address_register(pci, 3);
    if (bar3.type == mem_mapping && bar3.size != 0) {
        pci_bar_t mapped;
        if (pci_map_bar(pci, 3, &mapped) < 0) return -ENODEV;
        chip->nabm.is_mmio = true;
        chip->nabm.mmio    = mapped.virt;
    } else {
        base_address_register_t bar1 = get_base_address_register(pci, 1);
        if (bar1.type != input_output || bar1.size == 0) return -ENODEV;
        chip->nabm.is_mmio = false;
        chip->nabm.port    = (uint16_t)(uintptr_t)bar1.address;
    }
    return EOK;
}

/* Look up an Intel 8x0 PCI device entry by vendor/device ID. */
static const ich_pci_id_t *intel8x0_match_pci(const pci_device_cache_t *pci)
{
    for (size_t i = 0; i < sizeof(intel8x0_pci_ids) / sizeof(intel8x0_pci_ids[0]); i++) {
        if (intel8x0_pci_ids[i].vendor == pci->vendor_id && intel8x0_pci_ids[i].device == pci->device_id) { return &intel8x0_pci_ids[i]; }
    }
    return NULL;
}

/* Probe one matched AC'97 PCI controller and register its ULSA card. */
static int intel8x0_probe_one(pci_device_cache_t *pci, const ich_pci_id_t *id)
{
    ulsa_card_t *card = NULL;
    int          err  = ulsa_card_new(-1, "ICH", &card);
    if (err < 0) return err;

    intel8x0_t *chip = calloc(1, sizeof(intel8x0_t));
    if (!chip) {
        ulsa_card_free(card);
        return -ENOMEM;
    }

    chip->type     = id->type;
    chip->pci      = pci;
    chip->card     = card;
    chip->irq_slot = ICH_MAX_DEVICES;

    card->private_data = chip;
    card->private_free = intel8x0_card_free;

    const char *drv_name = "ICH";
    if (id->type == ICH_DEV_ICH4) drv_name = "ICH4";
    if (id->type == ICH_DEV_NFORCE) drv_name = "NFORCE";
    snprintf(card->driver, sizeof(card->driver), "%s", drv_name);
    snprintf(card->shortname, sizeof(card->shortname), "%s", id->shortname);

    pci_enable_device(pci, PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_BUSMASTER);
    if (intel8x0_setup_bars(chip, pci) < 0) goto fail;

    chip->bdbar_page_phys = alloc_frames_below(1, 0x100000000ULL);
    if (!chip->bdbar_page_phys) goto fail;
    uint8_t *bdbar_virt = phys_to_virt(chip->bdbar_page_phys);
    memset(bdbar_virt, 0, PAGE_4K_SIZE);

    chip->ichd[0].reg_offset   = ICH_REG_PI_BASE;
    chip->ichd[0].int_sta_mask = ICH_PIINT;
    chip->ichd[1].reg_offset   = ICH_REG_PO_BASE;
    chip->ichd[1].int_sta_mask = ICH_POINT;

    for (size_t i = 0; i < 2; i++) {
        size_t off               = i * (ICH_MAX_FRAGS * sizeof(ich_bdl_entry_t));
        chip->ichd[i].bdbar      = (volatile ich_bdl_entry_t *)(bdbar_virt + off);
        chip->ichd[i].bdbar_phys = (uint32_t)(chip->bdbar_page_phys + off);
        chip->ichd[i].pos_shift  = id->type == ICH_DEV_SIS ? 0U : 1U;
        if (id->type == ICH_DEV_SIS) {
            chip->ichd[i].roff_sr   = ICH_REG_OFF_PICB;
            chip->ichd[i].roff_picb = ICH_REG_OFF_SR;
        } else {
            chip->ichd[i].roff_sr   = ICH_REG_OFF_SR;
            chip->ichd[i].roff_picb = ICH_REG_OFF_PICB;
        }
        chip->int_sta_mask |= chip->ichd[i].int_sta_mask;
    }

    chip->codec_bit[0] = ICH_PCR;
    chip->codec_bit[1] = ICH_SCR;
    chip->codec_bit[2] = id->type == ICH_DEV_SIS ? ICH_SIS_TCR : ICH_TCR;
    chip->max_codecs   = (id->type == ICH_DEV_ICH4 || id->type == ICH_DEV_SIS) ? 3U : 2U;
    for (unsigned int i = 0; i < chip->max_codecs && i < 3; i++) {
        chip->codec_isr_bits |= chip->codec_bit[i];
        chip->codec_ready_bits |= chip->codec_bit[i];
    }

    if (intel8x0_chip_init(chip) < 0) goto fail;

    uint64_t rflags = spin_lock_irqsave(&intel8x0_irq_lock);
    size_t   slot   = ICH_MAX_DEVICES;
    for (size_t i = 0; i < ICH_MAX_DEVICES; i++) {
        if (!intel8x0_irq_slots[i]) {
            slot                  = i;
            intel8x0_irq_slots[i] = chip;
            break;
        }
    }
    spin_unlock_irqrestore(&intel8x0_irq_lock, rflags);
    if (slot == ICH_MAX_DEVICES) goto fail;
    chip->irq_slot = (uint8_t)slot;

    pci_irq_request_t req = {
        .modes         = PCI_IRQ_MSI | PCI_IRQ_LEGACY,
        .idt_handler   = intel8x0_idt_handlers[slot],
        .legacy_base   = IRQ_0,
        .legacy_ioapic = 1,
    };
    if (pci_request_irq(pci, &req, &chip->irq_state) < 0) goto fail;

    static const ac97_bus_ops_t bus_ops = {
        .write = intel8x0_codec_write,
        .read  = intel8x0_codec_read,
    };
    chip->ac97_bus = ac97_bus_create(card, &bus_ops, chip);
    if (!chip->ac97_bus) goto fail;

    chip->in_ac97_init = true;
    chip->in_sdin_init = true;
    uint32_t glob_sta  = igetdword(chip, ICH_REG_GLOB_STA);
    for (unsigned int i = 0; i < chip->max_codecs && i < 3 && chip->ncodecs < 3; i++) {
        if (!(glob_sta & chip->codec_bit[i])) continue;
        if (id->type == ICH_DEV_ICH4) {
            intel8x0_codec_read_test(chip, chip->ncodecs);
            unsigned int sdin              = igetbyte(chip, ICH_REG_SDM) & ICH_LDI_MASK;
            chip->ac97_sdin[chip->ncodecs] = sdin < 3 ? sdin : 0U;
        } else {
            chip->ac97_sdin[chip->ncodecs] = i;
        }
        chip->ncodecs++;
    }
    chip->in_sdin_init = false;
    if (chip->ncodecs == 0) {
        chip->ncodecs      = 1;
        chip->ac97_sdin[0] = 0;
    }

    for (unsigned int i = 0; i < chip->ncodecs; i++) {
        if (ac97_codec_probe(chip->ac97_bus, i, chip, &chip->ac97[i]) < 0 && i == 0) goto fail;
    }
    chip->in_ac97_init = false;

    ulsa_pcm_t *pcm = NULL;
    err             = ulsa_pcm_new(card, card->shortname, 0, 1, 1, &pcm);
    if (err < 0) goto fail;
    ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &intel8x0_pcm_ops);
    ulsa_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &intel8x0_pcm_ops);
    pcm->private_data = chip;
    ulsa_pcm_set_managed_buffer_all(pcm, 0x100000000ULL, 128UL * 1024UL);

    unsigned int irq_num = chip->irq_state.mode == PCI_IRQ_LEGACY ? chip->irq_state.irq : (unsigned int)chip->irq_state.vector;
    snprintf(card->longname, sizeof(card->longname), "%s with %s at irq %u", card->shortname, chip->ac97[0]->name, irq_num);

    err = ulsa_card_register(card);
    if (err < 0) goto fail;

    plogk("ac97: %s (%s) IRQ vector %d\n", card->shortname, chip->ac97[0]->name, chip->irq_state.vector);
    return EOK;

fail:
    ulsa_card_free(card);
    return -ENODEV;
}

/* Scan all PCI devices for Intel 8x0 / AC'97 audio controllers. */
void ac97_init(void)
{
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return;

    for (pci_device_cache_t *dev = cache->head; dev; dev = dev->next) {
        const ich_pci_id_t *id = intel8x0_match_pci(dev);
        if (id) (void)intel8x0_probe_one(dev, id);
    }
}

#endif // CONFIG_AUDIO && CONFIG_AUDIO_AC97
