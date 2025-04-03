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

#include "stdint.h"

typedef struct {
	uint8_t *buffer;
	unsigned long length;
} Bitmap;

/* Initialize the memory bitmap */
void bitmap_init(Bitmap *bitmap, uint8_t *buffer, unsigned long size);

/* Get memory bitmap */
int bitmap_get(const Bitmap *bitmap, unsigned long index);

/* Setting the memory bitmap */
void bitmap_set(Bitmap *bitmap, unsigned long index, int value);

/* Set the memory bitmap range */
void bitmap_set_range(Bitmap *bitmap, unsigned long start, unsigned long end, int value);

/* Memory bitmap search range */
unsigned long bitmap_find_range(const Bitmap *bitmap, unsigned long length, int value);

#endif // INCLUDE_BITMAP_H_
