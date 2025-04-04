/*
 *
 *		frame.h
 *		Memory frame header file
 *
 *		2025/2/16 By XIAOYI12
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
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

/* Initialize memory frame */
void init_frame(void);

/* Allocate memory frame */
uint64_t alloc_frames(unsigned long count);

/* Print memory map */
void print_memory_map(void);

#endif // INCLUDE_FRAME_H_
