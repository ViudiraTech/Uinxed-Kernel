/*
 *
 *		bmp.h
 *		BMP位图图像解析头文件
 *
 *		2024/9/16 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_BMP_H_
#define INCLUDE_BMP_H_

#include "types.h"

typedef struct {
	uint16_t magic;
	uint32_t fileSize;
	uint32_t reserved;
	uint32_t bmpDataOffset;

	//  bmp信息头开始
	uint32_t bmpInfoSize;
	uint32_t frameWidth;
	uint32_t frameHeight;
	uint16_t reservedValue; // 必须为0x0001
	uint16_t bitsPerPixel;
	uint32_t compressionMode;
	uint32_t frameSize;
	uint32_t horizontalResolution;
	uint32_t verticalResolution;
	uint32_t usedColorCount;
	uint32_t importantColorCount;
} __attribute__((packed)) Bmp;

void bmp_analysis(Bmp *bmp, uint32_t x, uint32_t y, bool isTransparent);

#endif // INCLUDE_BMP_H_
