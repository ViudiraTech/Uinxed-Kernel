/*
 *
 *		bitmap.c
 *		Memory bitmap
 *
 *		2025/2/16 By XIAOYI12
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "bitmap.h"
#include "string.h"
#include "printk.h"

/* Initialize the memory bitmap */
void bitmap_init(Bitmap *bitmap, uint8_t *buffer, size_t size)
{
	bitmap->buffer = buffer;
	bitmap->length = size * 8;
	memset(buffer, 0, size);
}

/* Get memory bitmap */
int bitmap_get(const Bitmap *bitmap, size_t index)
{
	size_t word_index = index / 8;
	size_t bit_index = index % 8;
	return (bitmap->buffer[word_index] >> bit_index) & 1;
}

/* Setting the memory bitmap */
void bitmap_set(Bitmap *bitmap, size_t index, int value)
{
	size_t word_index = index / 8;
	size_t bit_index = index % 8;
	if (value) {
		bitmap->buffer[word_index] |= ((size_t)1 << bit_index);
	} else {
		bitmap->buffer[word_index] &= ~((size_t)1 << bit_index);
	}
}

/* Set the memory bitmap range */
void bitmap_set_range(Bitmap *bitmap, size_t start, size_t end, int value)
{
	size_t start_word = (start + 7) / 8;
	size_t end_word = end / 8;
	if (start >= end || start >= bitmap->length) {
		return;
	}
	for (size_t i = start; i < start_word * 8 && i < end; i++) {
		bitmap_set(bitmap, i, value);
	}
	if (start_word > end_word) {
		return;
	}
	if (start_word <= end_word) {
		size_t fill_value = value ? (size_t)-1 : 0;
		for (size_t i = start_word; i < end_word; i++) {
			bitmap->buffer[i] = fill_value;
		}
	}
	for (size_t i = end_word * 8; i < end; i++) {
		bitmap_set(bitmap, i, value);
	}
}

/* Memory bitmap search range */
size_t bitmap_find_range(const Bitmap *bitmap, size_t length, int value)
{
	size_t count = 0, start_index = 0;
	uint8_t byte_match = value ? (uint8_t)-1 : 0;
	for (size_t byte_idx = 0; byte_idx < bitmap->length / 8; byte_idx++) {
		size_t byte = bitmap->buffer[byte_idx];
		if (byte == !byte_match) {
			count = 0;
		} else if (byte == byte_match) {
			if (length < 8) {
				return byte_idx * 8;
			}
			if (count == 0) {
				start_index = byte_idx * 8;
			}
			count += 8;
			if (count >= length) {
				return start_index;
			}
		} else {
			for (size_t bit = 0; bit < 8; bit++) {
				int bit_value = (byte >> bit) & 1;
				if (bit_value == value) {
					if (count == 0) {
						start_index = byte_idx * 8 + bit;
					}
					count++;
					if (count == length) {
						return start_index;
					}
				} else {
					count = 0;
				}
			}
		}
	}
	return (size_t)-1;
}
