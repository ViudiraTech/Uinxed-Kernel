/*
 *
 *      bitmap.h
 *      Memory bitmap header file
 *
 *      2025/2/16 By XIAOYI12
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BITMAP_H_
#define INCLUDE_BITMAP_H_

#include <stddef.h>
#include <stdint.h>

typedef struct {
        uint8_t *buffer;
        size_t   length;
} bitmap_t;

/* Initialize the memory bitmap */
void bitmap_init(bitmap_t *bitmap, uint8_t *buffer, size_t size);

/* Get memory bitmap */
int bitmap_get(const bitmap_t *bitmap, size_t index);

/* Setting the memory bitmap */
void bitmap_set(bitmap_t *bitmap, size_t index, int value);

/* Fill the memory bitmap */
void bitmap_fill(bitmap_t *bitmap, int value);

/* Set the memory bitmap range */
void bitmap_set_range(bitmap_t *bitmap, size_t start, size_t end, int value);

/* Memory bitmap search range */
size_t bitmap_find_range(const bitmap_t *bitmap, size_t length, int value);

/* Check memory bitmap range value */
int bitmap_range_all(const bitmap_t *bitmap, size_t start, size_t end, int value);

#endif // INCLUDE_BITMAP_H_
