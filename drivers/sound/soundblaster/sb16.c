/*
 *
 *      sb16.c
 *      Sound Blaster 16 driver
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <chipset/dma.h>
#include <drivers/pci/pci.h>
#include <drivers/sound/soundblaster/sb16.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

#if CONFIG_SOUND_SB16

static const uint16_t sb16_ports[] = {0x220, 0x240, 0x260, 0x280};

static sb16_device_t sb16_dev;
static spinlock_t    sb16_lock;

static int    sb16_audio_start(audio_card_t *card);
static int    sb16_audio_stop(audio_card_t *card);
static int    sb16_audio_drain(audio_card_t *card);
static size_t sb16_audio_write(audio_card_t *card, const void *addr, size_t offset, size_t size);
static size_t sb16_audio_read(audio_card_t *card, void *addr, size_t offset, size_t size);
static int    sb16_audio_set_format(audio_card_t *card, const audio_pcm_format_t *format);
static int    sb16_audio_set_volume(audio_card_t *card, const audio_volume_t *volume);
static int    sb16_audio_get_volume(audio_card_t *card, audio_volume_t *volume);
static int    sb16_audio_get_position(audio_card_t *card, snd_pcm_uframes_t *pos);
static int    sb16_audio_set_params(audio_card_t *card, const audio_pcm_format_t *fmt, size_t buffer_bytes, size_t period_bytes);

static const audio_card_ops_t sb16_audio_ops = {
    .pcm_read     = sb16_audio_read,
    .pcm_write    = sb16_audio_write,
    .set_format   = sb16_audio_set_format,
    .start        = sb16_audio_start,
    .stop         = sb16_audio_stop,
    .drain        = sb16_audio_drain,
    .set_volume   = sb16_audio_set_volume,
    .get_volume   = sb16_audio_get_volume,
    .get_position = sb16_audio_get_position,
    .get_avail    = 0,
    .set_params   = sb16_audio_set_params,
};

static inline uint8_t sb16_inb(uint16_t port)
{
    return inb(port);
}
static inline void sb16_outb(uint16_t port, uint8_t val)
{
    outb(port, val);
}

/* ------------------------------------------------------------------ */
/* DMA channel helpers                                                */
/* ------------------------------------------------------------------ */

/*
 * Program ISA DMA controller for a transfer.
 * channel: 0-3 for 8-bit, 4-7 for 16-bit (but 4 is cascade)
 * mode: 0x48 = single write, 0x44 = single read
 */
static void sb16_dma_program(uint8_t channel, uint32_t phys_addr, uint32_t size, uint8_t mode)
{
    /* Validate address range (< 16MB for 8-bit, < 16MB for 16-bit too on most chips) */
    if (phys_addr + size > 0x1000000) return;

    uint8_t  page = (uint8_t)(phys_addr >> 16);
    uint16_t offset;

    uint8_t mask_reg  = (channel < 4) ? 0x0A : 0xD4;
    uint8_t mode_reg  = (channel < 4) ? 0x0B : 0xD6;
    uint8_t clear_reg = (channel < 4) ? 0x0C : 0xD8;
    uint8_t page_port;
    uint8_t addr_port;
    uint8_t count_port;

    static const uint8_t page_ports[8]  = {0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A};
    static const uint8_t addr_ports[8]  = {0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC};
    static const uint8_t count_ports[8] = {0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE};

    page_port  = page_ports[channel];
    addr_port  = addr_ports[channel];
    count_port = count_ports[channel];

    mode |= (channel & 3);
    offset         = (uint16_t)(channel < 4 ? phys_addr : phys_addr / 2);
    uint16_t count = (uint16_t)(channel < 4 ? size - 1 : size / 2 - 1);

    disable_intr();

    /* Mask channel */
    outb(mask_reg, 0x04 | (channel & 3));

    /* Clear flip-flop */
    outb(clear_reg, 0);

    /* Set mode (write/read, single/autoinit, channel) */
    outb(mode_reg, mode);

    /* Set page */
    outb(page_port, page);

    /* Set address offset */
    outb(addr_port, LOW_BYTE(offset));
    outb(addr_port, HIGH_BYTE(offset));

    /* Set count */
    outb(count_port, LOW_BYTE(count));
    outb(count_port, HIGH_BYTE(count));

    /* Unmask channel */
    outb(mask_reg, channel & 3);

    enable_intr();
}

/* ------------------------------------------------------------------ */
/* DSP low-level I/O                                                  */
/* ------------------------------------------------------------------ */
int sb16_dsp_wait_write(sb16_device_t *dev)
{
    for (int i = 0; i < 10000; i++)
        if (!(sb16_inb(dev->base + SB16_DSP_WRITE) & 0x80)) return 0;
    return -1;
}

int sb16_dsp_write(sb16_device_t *dev, uint8_t cmd)
{
    if (sb16_dsp_wait_write(dev)) return -1;
    sb16_outb(dev->base + SB16_DSP_WRITE, cmd);
    return 0;
}

int sb16_dsp_read(sb16_device_t *dev, uint8_t *val)
{
    for (int i = 0; i < 10000; i++) {
        if (sb16_inb(dev->base + SB16_DSP_STATUS) & 0x80) {
            *val = sb16_inb(dev->base + SB16_DSP_READ);
            return 0;
        }
    }
    return -1;
}

int sb16_dsp_reset(sb16_device_t *dev)
{
    sb16_outb(dev->base + SB16_DSP_RESET, 1);
    for (int i = 0; i < 100; i++) sb16_outb(0x80, 0);
    sb16_outb(dev->base + SB16_DSP_RESET, 0);
    for (int i = 0; i < 100; i++) sb16_outb(0x80, 0);

    uint8_t val;
    if (sb16_dsp_read(dev, &val)) return -1;
    return (val == 0xAA) ? 0 : -1;
}

int sb16_dsp_version(sb16_device_t *dev, uint8_t *major, uint8_t *minor)
{
    if (sb16_dsp_write(dev, SB16_DSP_CMD_VERSION)) return -1;
    if (sb16_dsp_read(dev, major)) return -1;
    if (sb16_dsp_read(dev, minor)) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mixer                                                              */
/* ------------------------------------------------------------------ */
uint8_t sb16_mixer_read(sb16_device_t *dev, uint8_t reg)
{
    sb16_outb(dev->base + SB16_MIXER_ADDR, reg);
    return sb16_inb(dev->base + SB16_MIXER_DATA);
}

void sb16_mixer_write(sb16_device_t *dev, uint8_t reg, uint8_t value)
{
    sb16_outb(dev->base + SB16_MIXER_ADDR, reg);
    sb16_outb(dev->base + SB16_MIXER_DATA, value);
}

void sb16_set_master_volume(sb16_device_t *dev, uint8_t left, uint8_t right)
{
    sb16_mixer_write(dev, SB16_MIXER_MASTER_L, left);
    sb16_mixer_write(dev, SB16_MIXER_MASTER_R, right);
}

void sb16_set_dac_volume(sb16_device_t *dev, uint8_t left, uint8_t right)
{
    sb16_mixer_write(dev, SB16_MIXER_DAC_L, left);
    sb16_mixer_write(dev, SB16_MIXER_DAC_R, right);
}

void sb16_set_input_source(sb16_device_t *dev, uint8_t source)
{
    uint8_t l = 0, r = 0;
    switch (source) {
        case SB16_MIXER_INPUT_MIC :
            l = 0x00;
            r = 0x00;
            break;
        case SB16_MIXER_INPUT_CD :
            l = 0x03;
            r = 0x03;
            break;
        case SB16_MIXER_INPUT_LINE :
            l = 0x01;
            r = 0x01;
            break;
        default :
            return;
    }
    sb16_mixer_write(dev, SB16_MIXER_INPUT_SRC_L, l);
    sb16_mixer_write(dev, SB16_MIXER_INPUT_SRC_R, r);
    dev->input_source = source;
}

/* ------------------------------------------------------------------ */
/* Sample rate setting                                                */
/* ------------------------------------------------------------------ */
int sb16_set_rate8(sb16_device_t *dev, uint16_t rate)
{
    if (rate < 4000) rate = 4000;
    if (rate > 23000) rate = 23000;
    uint8_t tc = (uint8_t)(256 - (1000000 / rate));
    if (sb16_dsp_write(dev, SB16_DSP_CMD_SET_TIME_CONST)) return -1;
    return sb16_dsp_write(dev, tc);
}

int sb16_set_rate16(sb16_device_t *dev, uint16_t rate)
{
    if (rate < 4000) rate = 4000;
    if (rate > 48000) rate = 48000;
    if (sb16_dsp_write(dev, SB16_DSP_CMD_SET_RATE16)) return -1;
    if (sb16_dsp_write(dev, (rate >> 8) & 0xFF)) return -1;
    return sb16_dsp_write(dev, rate & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Playback                                                           */
/* ------------------------------------------------------------------ */
int sb16_play_8bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size)
{
    if (!dev->detected || !buffer || !size) return -1;
    if (size > dev->dma_buffer_size) return -1;

    memcpy(dev->dma_buffer_virt, buffer, size);
    sb16_dma_program(dev->dma8, dev->dma_buffer_phys, size, 0x48);

    if (sb16_set_rate8(dev, dev->sample_rate)) return -1;

    uint16_t block = (uint16_t)(size - 1);
    if (sb16_dsp_write(dev, SB16_DSP_CMD_DMA8_OUT)) return -1;
    if (sb16_dsp_write(dev, block & 0xFF)) return -1;
    if (sb16_dsp_write(dev, (block >> 8) & 0xFF)) return -1;

    dev->playing = 1;
    return 0;
}

int sb16_play_16bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size)
{
    if (!dev->detected || !buffer || !size) return -1;
    if (size > dev->dma_buffer_size) return -1;
    if (size % 2) return -1;

    memcpy(dev->dma_buffer_virt, buffer, size);
    sb16_dma_program(dev->dma16, dev->dma_buffer_phys, size, 0x48);

    if (sb16_set_rate16(dev, dev->sample_rate)) return -1;

    uint16_t words = (uint16_t)(size / 2 - 1);
    uint8_t  d0    = 0x10; // FIFO threshold
    if (sb16_dsp_write(dev, SB16_DSP_CMD_DMA16_OUT)) return -1;
    if (sb16_dsp_write(dev, d0)) return -1;
    if (sb16_dsp_write(dev, words & 0xFF)) return -1;
    if (sb16_dsp_write(dev, (words >> 8) & 0xFF)) return -1;

    dev->playing = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Capture                                                            */
/* ------------------------------------------------------------------ */
int sb16_capture_8bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size)
{
    if (!dev->detected || !buffer || !size) return -1;
    if (size > dev->dma_buffer_size) return -1;

    sb16_dma_program(dev->dma8, dev->dma_buffer_phys, size, 0x44);

    if (sb16_set_rate8(dev, dev->sample_rate)) return -1;

    uint16_t block = (uint16_t)(size - 1);
    if (sb16_dsp_write(dev, SB16_DSP_CMD_DMA8_IN)) return -1;
    if (sb16_dsp_write(dev, block & 0xFF)) return -1;
    if (sb16_dsp_write(dev, (block >> 8) & 0xFF)) return -1;

    /* Wait for DMA to complete */
    int got_ack = 0;
    for (int i = 0; i < 50000; i++) {
        uint8_t sts;
        if (sb16_dsp_read(dev, &sts) == 0 && sts == 0xAA) {
            got_ack = 1;
            break;
        }
        usleep(10);
    }
    if (!got_ack) plogk("sb16: capture DMA ack timeout (8-bit, size=%u)\n", size);

    memcpy(buffer, dev->dma_buffer_virt, size);
    dev->capturing = 1;
    return 0;
}

int sb16_capture_16bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size)
{
    if (!dev->detected || !buffer || !size) return -1;
    if (size % 2) return -1;
    if (size > dev->dma_buffer_size) return -1;

    sb16_dma_program(dev->dma16, dev->dma_buffer_phys, size, 0x44);

    if (sb16_set_rate16(dev, dev->sample_rate)) return -1;

    uint16_t words = (uint16_t)(size / 2 - 1);
    uint8_t  d0    = 0x10;
    if (sb16_dsp_write(dev, SB16_DSP_CMD_DMA16_IN)) return -1;
    if (sb16_dsp_write(dev, d0)) return -1;
    if (sb16_dsp_write(dev, words & 0xFF)) return -1;
    if (sb16_dsp_write(dev, (words >> 8) & 0xFF)) return -1;

    int got_ack = 0;
    for (int i = 0; i < 50000; i++) {
        uint8_t sts;
        if (sb16_dsp_read(dev, &sts) == 0 && sts == 0xAA) {
            got_ack = 1;
            break;
        }
        usleep(10);
    }
    if (!got_ack) plogk("sb16: capture DMA ack timeout (16-bit, size=%u)\n", size);

    memcpy(buffer, dev->dma_buffer_virt, size);
    dev->capturing = 1;
    return 0;
}

void sb16_stop(sb16_device_t *dev)
{
    sb16_dsp_write(dev, SB16_DSP_CMD_HALT_DMA);
    dev->playing   = 0;
    dev->capturing = 0;
}

/* ------------------------------------------------------------------ */
/* Audio subsystem callbacks                                          */
/* ------------------------------------------------------------------ */
static int sb16_audio_start(audio_card_t *card)
{
    (void)card;
    spin_lock(&sb16_lock);
    if (sb16_dev.playing || sb16_dev.capturing) {
        spin_unlock(&sb16_lock);
        return EOK;
    }

    if (sb16_dev.bits == 16) {
        sb16_play_16bit(&sb16_dev, sb16_dev.dma_buffer_virt, 512);
    } else {
        sb16_play_8bit(&sb16_dev, sb16_dev.dma_buffer_virt, 512);
    }
    spin_unlock(&sb16_lock);
    return EOK;
}

static int sb16_audio_stop(audio_card_t *card)
{
    (void)card;
    spin_lock(&sb16_lock);
    sb16_stop(&sb16_dev);
    spin_unlock(&sb16_lock);
    return EOK;
}

static int sb16_audio_drain(audio_card_t *card)
{
    (void)card;
    spin_lock(&sb16_lock);
    if (sb16_dev.playing) {
        msleep(50);
        sb16_stop(&sb16_dev);
    }
    spin_unlock(&sb16_lock);
    return EOK;
}

static size_t sb16_audio_write(audio_card_t *card, const void *addr, size_t offset, size_t size)
{
    sb16_device_t *dev = card->driver_data;
    (void)offset;

    if (!dev || !dev->detected || !addr || !size) return 0;

    spin_lock(&sb16_lock);

    size_t chunk = (size > dev->dma_buffer_size) ? dev->dma_buffer_size : size;

    if (card->format.bits == 16) {
        if (sb16_play_16bit(dev, (uint8_t *)addr, (uint32_t)chunk)) {
            plogk("sb16: playback start failed (chunk=%zu, bits=%u)\n", chunk, card->format.bits);
            spin_unlock(&sb16_lock);
            return 0;
        }
    } else {
        if (sb16_play_8bit(dev, (uint8_t *)addr, (uint32_t)chunk)) {
            plogk("sb16: playback start failed (chunk=%zu, bits=%u)\n", chunk, card->format.bits);
            spin_unlock(&sb16_lock);
            return 0;
        }
    }

    spin_unlock(&sb16_lock);
    return chunk;
}

static size_t sb16_audio_read(audio_card_t *card, void *addr, size_t offset, size_t size)
{
    sb16_device_t *dev = card->driver_data;
    (void)offset;

    if (!dev || !dev->detected || !addr || !size) return 0;

    spin_lock(&sb16_lock);

    size_t chunk = (size > dev->dma_buffer_size) ? dev->dma_buffer_size : size;

    if (card->format.bits == 16) {
        if (sb16_capture_16bit(dev, (uint8_t *)addr, (uint32_t)chunk)) {
            plogk("sb16: capture start failed (chunk=%zu, bits=%u)\n", chunk, card->format.bits);
            spin_unlock(&sb16_lock);
            return 0;
        }
    } else {
        if (sb16_capture_8bit(dev, (uint8_t *)addr, (uint32_t)chunk)) {
            plogk("sb16: capture start failed (chunk=%zu, bits=%u)\n", chunk, card->format.bits);
            spin_unlock(&sb16_lock);
            return 0;
        }
    }

    spin_unlock(&sb16_lock);
    return chunk;
}

static int sb16_audio_set_format(audio_card_t *card, const audio_pcm_format_t *format)
{
    sb16_device_t *dev;

    if (!card || !format) return -EINVAL;
    if (format->bits != 8 && format->bits != 16) return -EINVAL;
    if (format->channels != 1 && format->channels != 2) return -EINVAL;
    if (format->sample_rate < 4000 || format->sample_rate > 48000) return -EINVAL;

    dev = card->driver_data;
    if (!dev) return -ENODEV;

    spin_lock(&sb16_lock);
    card->format     = *format;
    dev->sample_rate = format->sample_rate;
    dev->bits        = format->bits;
    dev->channels    = format->channels;
    spin_unlock(&sb16_lock);
    return EOK;
}

static int sb16_audio_set_params(audio_card_t *card, const audio_pcm_format_t *fmt, size_t buffer_bytes, size_t period_bytes)
{
    sb16_device_t *dev = card->driver_data;
    (void)buffer_bytes;
    (void)period_bytes;

    if (!card || !fmt) return -EINVAL;
    if (!dev) return -ENODEV;

    return sb16_audio_set_format(card, fmt);
}

static int sb16_audio_set_volume(audio_card_t *card, const audio_volume_t *volume)
{
    sb16_device_t *dev;

    if (!card || !volume) return -EINVAL;
    dev = card->driver_data;
    if (!dev) return -ENODEV;

    spin_lock(&sb16_lock);
    dev->volume_left  = volume->left;
    dev->volume_right = volume->right;
    sb16_set_master_volume(dev, volume->left, volume->right);
    sb16_set_dac_volume(dev, volume->left, volume->right);
    spin_unlock(&sb16_lock);
    return EOK;
}

static int sb16_audio_get_volume(audio_card_t *card, audio_volume_t *volume)
{
    sb16_device_t *dev;

    if (!card || !volume) return -EINVAL;
    dev = card->driver_data;
    if (!dev) return -ENODEV;

    spin_lock(&sb16_lock);
    volume->left      = sb16_mixer_read(dev, SB16_MIXER_MASTER_L);
    volume->right     = sb16_mixer_read(dev, SB16_MIXER_MASTER_R);
    dev->volume_left  = volume->left;
    dev->volume_right = volume->right;
    spin_unlock(&sb16_lock);
    return EOK;
}

static int sb16_audio_get_position(audio_card_t *card, snd_pcm_uframes_t *pos)
{
    (void)card;
    if (!pos) return -EINVAL;
    /* SB16 has no hardware position register; approximate via state */
    *pos = 0;
    return EOK;
}

/* ------------------------------------------------------------------ */
/* Detection and initialization                                       */
/* ------------------------------------------------------------------ */
int sb16_detect(sb16_device_t *dev)
{
    for (size_t i = 0; i < sizeof(sb16_ports) / sizeof(sb16_ports[0]); i++) {
        dev->base = sb16_ports[i];
        if (sb16_dsp_reset(dev)) continue;

        uint8_t major = 0, minor = 0;
        if (sb16_dsp_version(dev, &major, &minor)) continue;

        plogk("sb16: DSP version %u.%u at port 0x%x\n", major, minor, dev->base);
        dev->detected = 1;
        dev->dma8     = SB16_DMA8;
        dev->dma16    = SB16_DMA16;
        dev->irq      = SB16_IRQ_5;
        return 0;
    }
    return -1;
}

void sb16_beep(uint16_t freq, uint32_t ms)
{
    if (!sb16_dev.detected) return;

    uint32_t samples  = (uint32_t)(sb16_dev.sample_rate * ms / 1000);
    size_t   buf_size = samples;

    uint8_t *buf = malloc(buf_size);
    if (!buf) return;

    uint32_t period = sb16_dev.sample_rate / freq;
    for (uint32_t i = 0; i < samples; i++) { buf[i] = (i % period) < (period / 2) ? 200 : 55; }

    sb16_play_8bit(&sb16_dev, buf, buf_size);

    msleep(ms);

    sb16_stop(&sb16_dev);
    free(buf);
}

void sb16_init(void)
{
    memset(&sb16_dev, 0, sizeof(sb16_device_t));
    sb16_dev.input_source = SB16_MIXER_INPUT_MIC;

    if (sb16_detect(&sb16_dev)) {
        sb16_dev.base  = 0x220;
        sb16_dev.irq   = SB16_IRQ_5;
        sb16_dev.dma8  = SB16_DMA8;
        sb16_dev.dma16 = SB16_DMA16;
        if (sb16_dsp_reset(&sb16_dev)) return;
        uint8_t major = 0, minor = 0;
        if (sb16_dsp_version(&sb16_dev, &major, &minor)) return;
        plogk("sb16: DSP version %u.%u at port 0x%x\n", major, minor, sb16_dev.base);
        sb16_dev.detected = 1;
    }

    sb16_dev.sample_rate  = 22050;
    sb16_dev.bits         = 8;
    sb16_dev.channels     = 1;
    sb16_dev.volume_left  = 0xCC;
    sb16_dev.volume_right = 0xCC;

    sb16_set_master_volume(&sb16_dev, sb16_dev.volume_left, sb16_dev.volume_right);
    sb16_set_dac_volume(&sb16_dev, sb16_dev.volume_left, sb16_dev.volume_right);
    sb16_mixer_write(&sb16_dev, SB16_MIXER_OUT_SRC, SB16_MIXER_SRC_DAC);

    sb16_dev.dma_buffer_size = 65536;
    uint64_t frame           = alloc_frames(ALIGN_UP(sb16_dev.dma_buffer_size, PAGE_4K_SIZE) / PAGE_4K_SIZE);
    if (!frame) {
        plogk("sb16: Failed to allocate DMA buffer.\n");
        return;
    }
    sb16_dev.dma_buffer_phys = (uint32_t)frame;
    sb16_dev.dma_buffer_virt = (uint8_t *)phys_to_virt(frame);

    plogk("sb16: IRQ %u, DMA8 %u, DMA16 %u, DMA buffer %u bytes.\n", sb16_dev.irq, sb16_dev.dma8, sb16_dev.dma16, sb16_dev.dma_buffer_size);

    audio_pcm_format_t format = {
        .sample_rate = sb16_dev.sample_rate,
        .bits        = sb16_dev.bits,
        .channels    = sb16_dev.channels,
    };
    audio_register_card("Sound Blaster 16", &format, &sb16_audio_ops, &sb16_dev);
}

#else
void sb16_init(void)
{
}
#endif
