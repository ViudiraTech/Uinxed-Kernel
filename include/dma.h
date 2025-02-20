/*
 *
 *		dma.h
 *		直接内存访问功能头文件
 *
 *		2025/1/9 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#ifndef INCLUDE_DMA_H_
#define INCLUDE_DMA_H_

#include "types.h"

/* 向DMA控制器发送命令 */
void dma_start(byte mode, byte channel, void *address, unsigned int size);

/* 使用DMA发送数据 */
void dma_send(byte channel, void *address, unsigned int size);

/* 使用DMA接收数据 */
void dma_recv(byte channel, void *address, unsigned int size);

#endif // INCLUDE_DMA_H_
