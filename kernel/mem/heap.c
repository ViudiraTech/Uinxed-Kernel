/*
 *
 *      heap.c
 *      Memory Heap
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "heap.h"
#include "alloc.h"
#include "cpuid.h"
#include "frame.h"
#include "hhdm.h"
#include "page.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"

uint64_t KERNEL_HEAP_START = 0;
uint64_t KERNEL_HEAP_SIZE  = 0;

/* Initialize the memory heap */
void init_heap(void)
{
    if (!KERNEL_HEAP_START) KERNEL_HEAP_START = (1ULL << (get_cpu_phys_bits() + 1)) + get_physical_memory_offset();
    if (!KERNEL_HEAP_SIZE) KERNEL_HEAP_SIZE = frame_allocator.usable_frames / 2 * PAGE_SIZE; // 1/2 of usable memory

    page_map_range_to_random(get_kernel_pagedir(), KERNEL_HEAP_START, KERNEL_HEAP_SIZE, KERNEL_PTE_FLAGS);
    pointer_cast_t cast;
    cast.val = KERNEL_HEAP_START;
    heap_init(cast.ptr, KERNEL_HEAP_SIZE);
}

/* Allocate an empty memory */
void *calloc(size_t nmemb, size_t size)
{
    void *p = malloc(nmemb * size);
    memset(p, 0, nmemb * size);
    return p;
}
