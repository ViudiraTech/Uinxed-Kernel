/*
 *
 *      alloc.h
 *      Memory heap allocator header file
 *
 *      2025/2/16 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ALLOC_H_
#define INCLUDE_ALLOC_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
    invalid_free,
    layout_error,
} heap_error_t;

typedef void (*error_handler)(heap_error_t error, void *ptr);

/* Initializes the heap memory arena */
int heap_init(uint8_t *address, size_t size);

/* Set a custom error handling function */
void heap_onerror(error_handler handler);

/* Returns the usable size of the memory block pointed */
size_t usable_size(void *ptr);

/* Allocates memory with default alignment */
void *malloc(size_t size);

/* Allocates memory with specified alignment */
void *aligned_alloc(size_t alignment, size_t size);

/* Reallocates memory previously allocated */
void *realloc(void *ptr, size_t new_size);

/* Frees memory previously allocated */
void free(void *ptr);

#endif // INCLUDE_ALLOC_H_
