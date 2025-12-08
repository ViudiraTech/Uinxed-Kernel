/*
 *
 *      dma.h
 *      Direct memory access header files
 *
 *      2025/1/9 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DMA_H_
#define INCLUDE_DMA_H_

#include <stdint.h>

#define LOW_BYTE(x)  ((x) & 0x00ff)
#define HIGH_BYTE(x) (((x) & 0xff00) >> 8)

/* Sending commands to the DMA controller */
void dma_start(uint8_t mode, uint8_t channel, uint32_t *address, uint32_t size);

/* Sending data using DMA */
void dma_send(uint8_t channel, uint32_t *address, uint32_t size);

/* Receiving data using DMA */
void dma_recv(uint8_t channel, uint32_t *address, uint32_t size);

#endif // INCLUDE_DMA_H_
