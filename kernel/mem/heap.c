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

uint64_t heap_start = 0;
uint64_t heap_size  = 0;

/* Initialize the memory heap */
void init_heap(void)
{
    uint64_t heap_frame_count     = frame_allocator.usable_frames / 8;
    uint64_t min_heap_frame_count = MIN_HEAP_SIZE / PAGE_SIZE;
    heap_frame_count              = heap_frame_count > min_heap_frame_count ? heap_frame_count : min_heap_frame_count;

    pointer_cast_t heap_virt;
    heap_virt.ptr = phys_to_virt((uint64_t)1 << get_cpu_phys_bits()); // End of HHDM for heap
    heap_virt.val = ALIGN_UP(heap_virt.val, 16);

    uint64_t heap_virt_start = heap_virt.val;
    heap_start               = heap_virt_start;
    heap_size                = heap_frame_count * PAGE_SIZE;
    page_map_range_to_random(get_kernel_pagedir(), heap_start, heap_size, KERNEL_PTE_FLAGS);

    heap_init(heap_virt.ptr, heap_size);
}

/* Allocate an empty memory */
void *calloc(size_t nmemb, size_t size)
{
    void *p = malloc(nmemb * size);
    memset(p, 0, nmemb * size);
    return p;
}
