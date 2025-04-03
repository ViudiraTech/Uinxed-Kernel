/*
 *
 *		heap.h
 *		Memory heap header file
 *
 *		2025/2/16 By XIAOYI12
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_HEAP_H_
#define INCLUDE_HEAP_H_

#include "stdint.h"

/* Initialize the memory heap */
void init_heap(void);

/* Allocate an empty memory */
void *calloc(unsigned long a, unsigned long b);

#endif // INCLUDE_HEAP_H_
