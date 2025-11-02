/*
 *
 *      heap.c
 *      Memory heap
 *
 *      2025/2/16 By XIAOYI12
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include "heap.h"
#include "alloc.h"
#include "frame.h"
#include "hhdm.h"
#include "limine.h"
#include "page.h"
#include "page_walker.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"
#include "uinxed.h"

uint64_t KERNEL_HEAP_START = 0;
uint64_t KERNEL_HEAP_SIZE  = 0;

/* Initialize the memory heap */
void init_heap(void)
{
    struct limine_memmap_response *memmap_response = memmap_request.response;
    struct limine_memmap_entry    *last_entry      = memmap_response->entries[memmap_response->entry_count - 1];
    if (!KERNEL_HEAP_START) KERNEL_HEAP_START = (last_entry->base + last_entry->length) + get_physical_memory_offset();
    if (!KERNEL_HEAP_SIZE) KERNEL_HEAP_SIZE = frame_allocator.usable_frames / 2 * PAGE_4K_SIZE; // 1/2 of usable memory

    KERNEL_HEAP_START = ALIGN_UP(KERNEL_HEAP_START, PAGE_1G_SIZE);
    KERNEL_HEAP_START = walk_page_tables_find_free(get_kernel_pagedir(), KERNEL_HEAP_START, KERNEL_HEAP_SIZE, PAGE_1G_SIZE);
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
