/*
 *
 *      heap.h
 *      Memory heap header file
 *
 *      2025/2/16 By XIAOYI12
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_HEAP_H_
#define INCLUDE_HEAP_H_

#include <alloc.h>
#include <stddef.h>
#include <stdint.h>

extern uint64_t KERNEL_HEAP_START; // Kernel heap start
extern uint64_t KERNEL_HEAP_SIZE;  // Kernel heap size

/* Initialize the memory heap */
void init_heap(void);

/* Allocate an empty memory */
void *calloc(size_t nmemb, size_t size);

#endif // INCLUDE_HEAP_H_
