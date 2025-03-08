/*
 *
 *		heap.c
 *		内存堆
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "heap.h"
#include "printk.h"
#include "string.h"
#include "hhdm.h"
#include "alloc.h"

/* 初始化内存堆 */
void init_heap(void)
{
	uint8_t *heap_base = (uint8_t *)(physical_memory_offset + 0x3c0f000);
	unsigned long heap_size = 0x400000; // 4MB

	heap_init(heap_base, heap_size);
	plogk("Heap: Initialized kernel heap at base 0x%08x, size 0x%08x (%u MiB)\n",
          (unsigned long long)heap_base, heap_size, heap_size / (1024 * 1024));
}

/* 分配一块空内存 */
void *calloc(unsigned long a, unsigned long b)
{
	void *p = malloc(a * b);
	memset(p, 0, a * b);
	return p;
}
