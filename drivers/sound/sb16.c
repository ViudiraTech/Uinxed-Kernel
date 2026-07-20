/*
 *
 *      sb16.c
 *      Sound Blaster 16 driver
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <sound/sb16.h>
#include <common.h>
#include <dma.h>
#include <frame.h>
#include <heap.h>
#include <hhdm.h>
#include <interrupt.h>
#include <page.h>
#include <pci.h>
#include <printk.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>

static const uint16_t sb16_ports[] = { 0x220, 0x240, 0x260, 0x280 };

static sb16_device_t sb16_dev;
static spinlock_t sb16_lock;

static inline uint8_t sb16_inb(uint16_t port)
{ return inb(port); }

static inline void sb16_outb(uint16_t port, uint8_t val)
{ outb(port, val); }

int sb16_dsp_wait_write(sb16_device_t *dev)
{
    for (int i = 0; i < 10000; i++) {
        if (!(sb16_inb(dev->base + SB16_DSP_WRITE) & 0x80)) return 0;
    }
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

int sb16_play_8bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size)
{
    if (!dev->detected) return -1;
    if (!buffer || !size) return -1;
    if (size > dev->dma_buffer_size) return -1;

    memcpy(dev->dma_buffer_virt, buffer, size);

    dma_send(dev->dma8, (uint32_t *)(uintptr_t)dev->dma_buffer_phys, size);

    if (sb16_set_rate8(dev, dev->sample_rate)) return -1;

    uint16_t block = (uint16_t)(size - 1);
    if (sb16_dsp_write(dev, SB16_DSP_CMD_DMA8_OUT)) return -1;
    if (sb16_dsp_write(dev, block & 0xFF)) return -1;
    if (sb16_dsp_write(dev, (block >> 8) & 0xFF)) return -1;

    return 0;
}

int sb16_play_16bit(sb16_device_t *dev, uint8_t *buffer, uint32_t size)
{
    if (!dev->detected) return -1;
    if (!buffer || !size) return -1;
    if (size > dev->dma_buffer_size) return -1;
    if (size % 2) return -1;

    memcpy(dev->dma_buffer_virt, buffer, size);

    dma_send(dev->dma16, (uint32_t *)(uintptr_t)dev->dma_buffer_phys, size);

    if (sb16_set_rate16(dev, dev->sample_rate)) return -1;

    uint16_t words = (uint16_t)(size / 2 - 1);
    uint8_t d0 = 0x10;
    if (sb16_dsp_write(dev, SB16_DSP_CMD_DMA16_OUT)) return -1;
    if (sb16_dsp_write(dev, d0)) return -1;
    if (sb16_dsp_write(dev, words & 0xFF)) return -1;
    if (sb16_dsp_write(dev, (words >> 8) & 0xFF)) return -1;

    return 0;
}

void sb16_stop(sb16_device_t *dev)
{
    sb16_dsp_write(dev, SB16_DSP_CMD_HALT_DMA);
}

int sb16_detect(sb16_device_t *dev)
{
    for (size_t i = 0; i < sizeof(sb16_ports) / sizeof(sb16_ports[0]); i++) {
        dev->base = sb16_ports[i];
        if (sb16_dsp_reset(dev)) continue;

        uint8_t major = 0, minor = 0;
        if (sb16_dsp_version(dev, &major, &minor)) continue;

        plogk("sb16: DSP version %u.%u at port 0x%x\n", major, minor, dev->base);
        dev->detected = 1;
        dev->dma8 = 1;
        dev->dma16 = 5;
        dev->irq = 5;
        return 0;
    }
    return -1;
}

void sb16_beep(uint16_t freq, uint32_t ms)
{
    if (!sb16_dev.detected) return;

    uint32_t samples = (uint32_t)(sb16_dev.sample_rate * ms / 1000);
    size_t buf_size = samples;

    uint8_t *buf = malloc(buf_size);
    if (!buf) return;

    uint32_t period = sb16_dev.sample_rate / freq;
    for (uint32_t i = 0; i < samples; i++) {
        buf[i] = (i % period) < (period / 2) ? 200 : 55;
    }

    sb16_play_8bit(&sb16_dev, buf, buf_size);

    msleep(ms);

    sb16_stop(&sb16_dev);
    free(buf);
}

void sb16_init(void)
{
    memset(&sb16_dev, 0, sizeof(sb16_device_t));
    sb16_lock.lock = 0;
    sb16_lock.rflags = 0;

    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) {
        plogk("sb16: PCI cache not ready, trying legacy ports\n");
    }

    if (sb16_detect(&sb16_dev)) {
        sb16_dev.base = 0x220;
        sb16_dev.irq = 5;
        sb16_dev.dma8 = 1;
        sb16_dev.dma16 = 5;
        if (sb16_dsp_reset(&sb16_dev)) {
            plogk("sb16: No SB16 card found\n");
            return;
        }
        uint8_t major = 0, minor = 0;
        if (sb16_dsp_version(&sb16_dev, &major, &minor)) {
            plogk("sb16: DSP version check failed\n");
            return;
        }
        plogk("sb16: Found DSP version %u.%u at port 0x%x\n", major, minor, sb16_dev.base);
        sb16_dev.detected = 1;
    }

    sb16_dev.sample_rate = 22050;

    sb16_set_master_volume(&sb16_dev, 0xCC, 0xCC);
    sb16_set_dac_volume(&sb16_dev, 0xCC, 0xCC);
    sb16_mixer_write(&sb16_dev, SB16_MIXER_OUT_SRC, SB16_MIXER_SRC_DAC);

    plogk("sb16: SB16 initialized at port 0x%x, IRQ %u, DMA8 %u, DMA16 %u\n",
          sb16_dev.base, sb16_dev.irq, sb16_dev.dma8, sb16_dev.dma16);

    sb16_dev.dma_buffer_size = 65536;
    uint64_t frame = alloc_frames(ALIGN_UP(sb16_dev.dma_buffer_size, PAGE_4K_SIZE) / PAGE_4K_SIZE);
    if (frame) {
        sb16_dev.dma_buffer_phys = frame;
        sb16_dev.dma_buffer_virt = (uint8_t *)phys_to_virt(frame);
        plogk("sb16: DMA buffer at phys=%p virt=%p size=%u\n",
              (void *)frame, sb16_dev.dma_buffer_virt, sb16_dev.dma_buffer_size);
    } else {
        plogk("sb16: Failed to allocate DMA buffer\n");
    }
}
