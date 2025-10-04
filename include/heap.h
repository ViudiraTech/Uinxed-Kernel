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

#define KERNEL_HEAP_START 0xffff900000000000 // Kernel heap start
#define KERNEL_HEAP_SIZE  0x6400000          // Kernel heap size (100MiB)

/* Initialize the memory heap */
void init_heap(void);

/* Allocate an empty memory */
void *calloc(size_t nmemb, size_t size);

#endif // INCLUDE_HEAP_H_
