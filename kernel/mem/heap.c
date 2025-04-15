/*
 *
 *		heap.c
 *		Memory Heap
 *
 *		2025/2/16 By XIAOYI12
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "heap.h"
#include "printk.h"
#include "string.h"
#include "hhdm.h"
#include "alloc.h"
#include "page.h"
#include "stdint.h"
#include "stddef.h"

/* Initialize the memory heap */
void init_heap(void)
{
	uint8_t *heap_base = (uint8_t *)(get_physical_memory_offset() + 0x3c0f000);
	size_t heap_size = 0x800000; // 8MB

	heap_init(heap_base, heap_size);
}

/* Allocate 4k-aligned memory */
void *alloc_4k_aligned_mem(size_t size)
{
	void *p = malloc(size < PAGE_SIZE ? size + PAGE_SIZE : size);
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
