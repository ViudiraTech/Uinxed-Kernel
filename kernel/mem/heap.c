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

/* Initialize the memory heap */
void init_heap(void)
{
    page_map_range_to_random(get_kernel_pagedir(), KERNEL_HEAP_START, KERNEL_HEAP_SIZE, KERNEL_PTE_FLAGS);
    heap_init((uint8_t *)KERNEL_HEAP_START, KERNEL_HEAP_SIZE);
}

/* Allocate an empty memory */
void *calloc(size_t nmemb, size_t size)
{
    void *p = malloc(nmemb * size);
    memset(p, 0, nmemb * size);
    return p;
}
