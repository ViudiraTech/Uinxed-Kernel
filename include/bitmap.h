/*
 *
 *		bitmap.h
 *		内存位映射头文件
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_BITMAP_H_
#define INCLUDE_BITMAP_H_

#include "stdint.h"

typedef struct {
	uint8_t *buffer;
	unsigned long length;
} Bitmap;

/* 初始化内存位映射 */
void bitmap_init(Bitmap *bitmap, uint8_t *buffer, unsigned long size);

/* 获取内存位映射 */
int bitmap_get(const Bitmap *bitmap, unsigned long index);

/* 设置内存位映射 */
void bitmap_set(Bitmap *bitmap, unsigned long index, int value);

/* 设置内存位映射范围 */
void bitmap_set_range(Bitmap *bitmap, unsigned long start, unsigned long end, int value);

/* 内存位映射查找范围 */
unsigned long bitmap_find_range(const Bitmap *bitmap, unsigned long length, int value);

#endif // INCLUDE_BITMAP_H_
