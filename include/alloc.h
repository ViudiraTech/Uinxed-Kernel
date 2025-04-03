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

/* Initialize a memory heapInitialize a memory heap */
int heap_init(uint8_t *address, unsigned long size);

/* Allocate a memory space of a specified size on the heap */
void *malloc(unsigned long size);

/* Free previously allocated memory */
void free(void *ptr);

#endif // INCLUDE_ALLOC_H_
