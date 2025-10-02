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

#define MIN_HEAP_SIZE (32 * 1024 * 1024) // Minimum heap size: 32 MiB
extern uint64_t heap_start;              // Kernel heap start address
extern uint64_t heap_size;               // Kernel heap size

/* Initialize the memory heap */
void init_heap(void);

/* Allocate an empty memory */
void *calloc(size_t nmemb, size_t size);

#endif // INCLUDE_HEAP_H_
