/*
 *
 *      frame.h
 *      Memory frame header file
 *
 *      2025/2/16 By XIAOYI12
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FRAME_H_
#define INCLUDE_FRAME_H_

#include <kernel/debug/ringlog.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/buddy.h>
#include <sync/spin_lock.h>

typedef struct {
        buddy_allocator_t buddy;
        size_t            frame_count;
        size_t            origin_frames;
        size_t            usable_frames;
        size_t            metadata_frames;
        spinlock_t        lock;
} frame_allocator_t;

typedef struct {
        size_t   total_frames;
        size_t   free_frames;
        size_t   metadata_frames;
        size_t   free_blocks[BUDDY_MAX_ORDER + 1];
        unsigned max_order;
} frame_stats_t;

extern log_buffer_t      frame_log;
extern frame_allocator_t frame_allocator;

/* Initialize memory frame */
void init_frame(void);

/* Allocate memory frames */
uint64_t alloc_frames(size_t count);

/* Allocate frames without entering swap reclaim from a locked caller. */
uint64_t alloc_frames_noreclaim(size_t count);

/* Reclaim pages under the frame allocator's recursion guard. */
int frame_reclaim_pages(size_t target);

/* Refill the low-memory watermark from a caller that holds no VM locks. */
void frame_reclaim_if_needed(size_t requested);

/* Allocate 2M memory frames */
uint64_t alloc_frames_2M(size_t count);

/* Allocate 1G memory frames */
uint64_t alloc_frames_1G(size_t count);

/* Retain ownership of a contiguous range of 4 KiB frames. */
int frame_retain_range(uint64_t addr, size_t count);

/* Release ownership of a range, returning final references to the bitmap. */
int frame_release_range(uint64_t addr, size_t count);

/* Return the current ownership count of a 4 KiB physical frame. */
uint32_t frame_refcount(uint64_t addr);

/* Snapshot allocator accounting and validate all buddy-list invariants. */
void frame_get_stats(frame_stats_t *stats);
int  frame_validate(void);

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
