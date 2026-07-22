/*
 *
 *      heap.c
 *      Memory heap
 *
 *      2025/2/16 By XIAOYI12
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <chipset/common.h>
#include <kernel/uinxed.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/page_walker.h>

uint64_t KERNEL_HEAP_START = 0;
uint64_t KERNEL_HEAP_SIZE  = 0;

#define KERNEL_HEAP_SEARCH_BASE 0xffffc00000000000ULL
#define KERNEL_HEAP_MAX_SIZE    (256ULL * 1024ULL * 1024ULL)

/* Initialize the memory heap */
void init_heap(void)
{
    struct limine_memmap_response *memmap_response = memmap_request.response;
    uint64_t                       usable_ram      = 0;

    if (!memmap_response) krn_halt();

    for (uint64_t i = 0; i < memmap_response->entry_count; i++) {
        if (memmap_response->entries[i]->type == LIMINE_MEMMAP_USABLE) { usable_ram += memmap_response->entries[i]->length; }
    }

    if (!KERNEL_HEAP_SIZE && !KERNEL_HEAP_START) {
        KERNEL_HEAP_SIZE = usable_ram / 4;
        if (KERNEL_HEAP_SIZE > KERNEL_HEAP_MAX_SIZE) KERNEL_HEAP_SIZE = KERNEL_HEAP_MAX_SIZE;
        KERNEL_HEAP_SIZE  = ALIGN_UP(KERNEL_HEAP_SIZE, PAGE_4K_SIZE);
        KERNEL_HEAP_START = walk_page_tables_find_free(get_kernel_pagedir(), KERNEL_HEAP_SEARCH_BASE, KERNEL_HEAP_SIZE, PAGE_2M_SIZE);
        KERNEL_HEAP_START = ALIGN_UP(KERNEL_HEAP_START, PAGE_2M_SIZE);
    }
    if (!KERNEL_HEAP_START || !KERNEL_HEAP_SIZE) krn_halt();

    page_map_range_to_random(get_kernel_pagedir(), KERNEL_HEAP_START, KERNEL_HEAP_SIZE, PTE_PRESENT | PTE_WRITEABLE);

    pointer_cast_t cast;
    cast.val = KERNEL_HEAP_START;
    heap_init(cast.ptr, KERNEL_HEAP_SIZE);
}

/* Allocate an empty memory */
void *calloc(size_t nmemb, size_t size)
{
    /* Check for multiplication overflow */
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return 0;

    size_t total = nmemb * size;
    void  *p     = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
