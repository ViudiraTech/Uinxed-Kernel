/*
 *
 *      frame.h
 *      Memory frame header file
 *
 *      2025/2/16 By XIAOYI12
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FRAME_H_
#define INCLUDE_FRAME_H_

#include <bitmap.h>
#include <ringlog.h>
#include <stdint.h>

typedef struct {
        bitmap_t bitmap;
        size_t   origin_frames;
        size_t   usable_frames;
} frame_allocator_t;

extern log_buffer_t      frame_log;
extern frame_allocator_t frame_allocator;

/* Initialize memory frame */
void init_frame(void);

/* Allocate memory frames */
uint64_t alloc_frames(size_t count);

/* Allocate 2M memory frames */
uint64_t alloc_frames_2M(size_t count);

/* Allocate 1G memory frames */
uint64_t alloc_frames_1G(size_t count);

/* Free a memory frame */
void free_frame(uint64_t addr);

/* Free memory frames */
void free_frames(uint64_t addr, size_t count);

/* Free 2M memory frames */
void free_frames_2M(uint64_t addr);

/* Free 1G memory frames */
void free_frames_1G(uint64_t addr);

/* Print memory map */
void print_memory_map(void);

#endif // INCLUDE_FRAME_H_
