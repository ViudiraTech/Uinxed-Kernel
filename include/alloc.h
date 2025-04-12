/*
 *
 *		alloc.h
 *		Memory heap allocator header file
 *
 *		2025/2/16 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_ALLOC_H_
#define INCLUDE_ALLOC_H_

#include "stddef.h"
#include "stdint.h"

/* Initialize a memory heapInitialize a memory heap */
int heap_init(uint8_t *address, size_t size);

/* Allocate a memory space of a specified size on the heap */
void *malloc(size_t size);

/* Free previously allocated memory */
void free(void *ptr);

#endif // INCLUDE_ALLOC_H_
