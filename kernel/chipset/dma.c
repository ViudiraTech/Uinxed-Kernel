/*
 *
 *      dma.c
 *      Direct memory access
 *
 *      2025/1/9 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <dma.h>

/* Fast access registers and ports for each DMA channel */
static const uint8_t MASK_REG[8]  = {0x0A, 0x0A, 0x0A, 0x0A, 0xD4, 0xD4, 0xD4, 0xD4};
static const uint8_t MODE_REG[8]  = {0x0B, 0x0B, 0x0B, 0x0B, 0xD6, 0xD6, 0xD6, 0xD6};
static const uint8_t CLEAR_REG[8] = {0x0C, 0x0C, 0x0C, 0x0C, 0xD8, 0xD8, 0xD8, 0xD8};

static const uint8_t PAGE_PORT[8]  = {0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A};
static const uint8_t ADDR_PORT[8]  = {0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC};
static const uint8_t COUNT_PORT[8] = {0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE};

static const uint32_t DMA_ADDR_MAX = 1 << 24;

/* Sending commands to the DMA controller */
void dma_start(uint8_t mode, uint8_t channel, uint32_t *address, uint32_t size)
{
    mode |= (channel % 4);

    if (channel > 4 && size % 2 != 0) return;

    uint32_t addr = (uint32_t)(uintptr_t)address;
    if (!(addr < DMA_ADDR_MAX)) return;
    if (!(addr + size < DMA_ADDR_MAX)) return;

    uint8_t  page   = addr >> 16;
    uint16_t offset = (channel > 4 ? addr / 2 : addr) & 0xffff;
    size            = (channel > 4 ? size / 2 : size) - 1;

    disable_intr();

    /* Setting up DMA channels */
    outb(MASK_REG[channel], 0x04 | (channel % 4));

    /* Unmask the DMA channel */
    outb(CLEAR_REG[channel], 0x00);

    /* Send the specified pattern to DMA */
    outb(MODE_REG[channel], mode);

    /* The physical page where the data is sent */
    outb(PAGE_PORT[channel], page);

    /* Send offset address */
    outb(ADDR_PORT[channel], LOW_BYTE(offset));
    outb(ADDR_PORT[channel], HIGH_BYTE(offset));

    /* The length of the data sent */
    outb(COUNT_PORT[channel], LOW_BYTE(size));
    outb(COUNT_PORT[channel], HIGH_BYTE(size));

    /* So enable DMA_channel */
    outb(MASK_REG[channel], (channel % 4));
    enable_intr();
}

/* Sending data using DMA */
void dma_send(uint8_t channel, uint32_t *address, uint32_t size)
{
    dma_start(0x48, channel, address, size);
}

/* Receiving data using DMA */
void dma_recv(uint8_t channel, uint32_t *address, uint32_t size)
{
    dma_start(0x44, channel, address, size);
}
