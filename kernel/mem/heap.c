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

/* Initialize the memory heap */
void init_heap(void)
{
	uint8_t *heap_base = (uint8_t *)(physical_memory_offset + 0x3c0f000);
	unsigned long heap_size = 0x800000; // 8MB

	heap_init(heap_base, heap_size);
}

/* Allocate 4k-aligned memory */
void *alloc_4k_aligned_mem(unsigned long size)
{
	void *p = malloc(size < PAGE_SIZE ? size + PAGE_SIZE : size);
	void *pAligned = (void *)(((uint64_t)p + 0xfff) & ~0xfff);
	return pAligned;
}

/* Allocate an empty memory */
void *calloc(unsigned long a, unsigned long b)
{
	void *p = malloc(a * b);
	memset(p, 0, a * b);
	return p;
}
