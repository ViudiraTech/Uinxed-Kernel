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
#include "page.h"
#include "printk.h"
#include "stddef.h"
#include "stdint.h"
#include "string.h"

/* Initialize the memory heap */
void init_heap(void)
{
    page_map_range_to_random(get_kernel_pagedir(), (uint64_t)phys_to_virt(KERNEL_HEAP_START), KERNEL_HEAP_SIZE,
                             KERNEL_PTE_FLAGS);
    heap_init(phys_to_virt(KERNEL_HEAP_START), KERNEL_HEAP_SIZE);
}

/* Allocate 4k-aligned memory */
void *alloc_4k_aligned_mem(size_t size)
{
    void *p         = malloc(size < PAGE_SIZE ? size + PAGE_SIZE : size);
    void *p_aligned = (void *)(((uint64_t)p + 0xfff) & ~0xfff);
    return p_aligned;
}

/* Allocate an empty memory */
void *calloc(size_t a, size_t b)
{
    void *p = malloc(a * b);
    memset(p, 0, a * b);
    return p;
}
