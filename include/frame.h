/*
 *
 *      frame.h
 *      Memory frame header file
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_FRAME_H_
#define INCLUDE_FRAME_H_

#include "bitmap.h"
#include "stdint.h"

typedef struct {
        Bitmap bitmap;
        size_t origin_frames;
        size_t usable_frames;
} FrameAllocator;

extern FrameAllocator frame_allocator;

/* Initialize memory frame */
void init_frame(void);

/* Allocate memory frame */
uint64_t alloc_frames(size_t count);

/* Free memory frames */
void free_frame(uint64_t addr);

/* Print memory map */
void print_memory_map(void);

#endif // INCLUDE_FRAME_H_
