/*
 *
 *		fifo.h
 *		FIFO先进先出结构头文件
 *
 *		2024/2/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_FIFO_H_
#define INCLUDE_FIFO_H_

#include "common.h"

typedef struct FIFO {										// FIFO结构体
	uint32_t *buf;
	int p, q, size, free, flags;
} fifo_t;

typedef struct FIFO8 {										// FIFO8结构体
	uint8_t *buf;
	int p, q, size, free, flags;
} fifo8_t;

#define FIFO_FLAGS_OVERRUN 1

void fifo_init(fifo_t *fifo, int size, uint32_t *buf);		// 初始化FIFO
int fifo_put(fifo_t *fifo, uint32_t data);					// FIFO缓冲区写入
int fifo_get(fifo_t *fifo);									// 获取FIFO缓冲区扫描码
int fifo_status(fifo_t *fifo);								// 获取FIFO缓冲区扫描码并解码

void fifo8_init(fifo8_t *fifo, int size, uint8_t *buf);		// 初始化FIFO8
int fifo8_put(fifo8_t *fifo, uint8_t data);					// FIFO8缓冲区写入
int fifo8_get(fifo8_t *fifo);								// 获取FIFO8缓冲区扫描码
int fifo8_status(fifo8_t *fifo);							// 获取FIFO8缓冲区扫描码并解码

#endif // INCLUDE_FIFO_H_
