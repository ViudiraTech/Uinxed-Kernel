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
#include "hhdm.h"
#include "limine.h"
#include "page.h"
#include "printk.h"
#include "stddef.h"
#include "stdint.h"
#include "string.h"
#include "uinxed.h"

uint64_t heap_start = 0;
uint64_t heap_size  = 0;

static void select_heap_space(size_t min_size)
{
    struct limine_memmap_response *memmap     = memmap_request.response;
    uint64_t                       best_start = 0;
    uint64_t                       best_size  = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t aligned_start = (entry->base + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE; // 4k alignment
        uint64_t usable_size   = entry->length - (aligned_start - entry->base);

        if (usable_size >= min_size && usable_size > best_size) {
            best_start = aligned_start;
            best_size  = usable_size;
        }
    }
    if (best_start == 0) printk_unsafe("No suitable heap region found (need %lld KiB)", heap_size / 1024);
    heap_start = best_start;
    heap_size  = best_size / PAGE_SIZE * PAGE_SIZE;
}

/* Initialize the memory heap */
void init_heap(void)
{
    select_heap_space(KERNEL_HEAP_MIN_SIZE);
    page_map_range_to_random(get_kernel_pagedir(), (uint64_t)phys_to_virt(heap_start), heap_size, KERNEL_PTE_FLAGS);
    heap_init(phys_to_virt(heap_start), heap_size);
}

/* Allocate 4k-aligned memory */
void *alloc_4k_aligned_mem(size_t size)
{
    void          *p = malloc(size < PAGE_SIZE ? size + PAGE_SIZE : size);
    pointer_cast_t p_aligned;
    p_aligned.val = (((uint64_t)p + 0xfff) & ~0xfff);
    return p_aligned.ptr;
}

/* Allocate an empty memory */
void *calloc(size_t a, size_t b)
{
    void *p = malloc(a * b);
    memset(p, 0, a * b);
    return p;
}
