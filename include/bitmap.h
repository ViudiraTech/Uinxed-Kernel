/*
 *
 *		bitmap.h
 *		Memory bitmap header file
 *
 *		2025/2/16 By XIAOYI12
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_BITMAP_H_
#define INCLUDE_BITMAP_H_

#include "stddef.h"
#include "stdint.h"

typedef struct {
	uint8_t *buffer;
	size_t	 length;
} Bitmap;

/* Initialize the memory bitmap */
void bitmap_init(Bitmap *bitmap, uint8_t *buffer, size_t size);

/* Get memory bitmap */
int bitmap_get(const Bitmap *bitmap, size_t index);

/* Setting the memory bitmap */
void bitmap_set(Bitmap *bitmap, size_t index, int value);

/* Set the memory bitmap range */
void bitmap_set_range(Bitmap *bitmap, size_t start, size_t end, int value);

/* Memory bitmap search range */
size_t bitmap_find_range(const Bitmap *bitmap, size_t length, int value);

#endif // INCLUDE_BITMAP_H_
