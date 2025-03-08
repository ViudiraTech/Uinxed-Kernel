/*
 *
 *		frame.h
 *		内存帧头文件
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_FRAME_H_
#define INCLUDE_FRAME_H_

#include "stdint.h"
#include "bitmap.h"

typedef struct {
	Bitmap bitmap;
	unsigned long origin_frames;
	unsigned long usable_frames;
} FrameAllocator;

extern FrameAllocator frame_allocator;

/* 初始化内存帧 */
void init_frame(void);

/* 分配内存帧 */
uint64_t alloc_frames(unsigned long count);

#endif // INCLUDE_FRAME_H_
