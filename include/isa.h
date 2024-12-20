/*
 *
 *		isa.h
 *		ISA设备驱动程序头文件
 *
 *		2024/8/20 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_ISA_H_
#define INCLUDE_ISA_H_

#include "types.h"

#define DMA_MODE_CHECK		0x00 // 自检模式
#define DMA_MODE_READ		0x04 // 外部设备读出（写内存）
#define DMA_MODE_WRITE		0x08 // 写入外部设备（读内存）
#define DMA_MODE_AUTO		0x10 // 自动模式
#define DMA_MODE_DOWN		0x20 // 从高地址向低地址访问内存
#define DMA_MODE_DEMAND		0x00 // 按需传输；
#define DMA_MODE_SINGLE		0x40 // 单次 DMA 传输；
#define DMA_MODE_BLOCK		0x80 // 块 DMA 传输；
#define DMA_MODE_CASCADE	0xC0 // 级联模式(用于级联另一个 DMA 控制器)；

/* 设置 DMA 掩码 */
void isa_dma_mask(uint8_t channel, int mask);

/* 设置起始地址 */
void isa_dma_addr(uint8_t channel, void *addr);

/* 设置传输大小 */
void isa_dma_size(uint8_t channel, uint32_t size);

/* 设置 DMA 模式 */
void isa_dma_mode(uint8_t channel, uint8_t mode);

/* 重置 DMA */
void isa_dma_reset(uint8_t channel);

#endif // INCLUDE_ISA_H_
