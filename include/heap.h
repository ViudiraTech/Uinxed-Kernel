/*
 *
 *      heap.h
 *      Memory heap header file
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_HEAP_H_
#define INCLUDE_HEAP_H_

#include "stddef.h"
#include "stdint.h"

#define KERNEL_HEAP_START 0x3c0f000 // Kernel heap start address (without offset)
#define KERNEL_HEAP_SIZE  0x1600000 // Kernel heap size 16MB

/* Initialize the memory heap */
void init_heap(void);

/* Allocate 4k-aligned memory */
void *alloc_4k_aligned_mem(size_t size);

/* Allocate an empty memory */
void *calloc(size_t a, size_t b);

#endif // INCLUDE_HEAP_H_
