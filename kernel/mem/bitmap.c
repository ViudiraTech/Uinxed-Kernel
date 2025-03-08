/*
 *
 *		bitmap.c
 *		内存位映射
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "bitmap.h"
#include "string.h"
#include "printk.h"

/* 初始化内存位映射 */
void bitmap_init(Bitmap *bitmap, uint8_t *buffer, unsigned long size)
{
	bitmap->buffer = buffer;
	bitmap->length = size * 8;
	memset(buffer, 0, size);
}

/* 获取内存位映射 */
int bitmap_get(const Bitmap *bitmap, unsigned long index)
{
	unsigned long word_index = index / 8;
	unsigned long bit_index = index % 8;
	return (bitmap->buffer[word_index] >> bit_index) & 1;
}

/* 设置内存位映射 */
void bitmap_set(Bitmap *bitmap, unsigned long index, int value)
{
	unsigned long word_index = index / 8;
	unsigned long bit_index = index % 8;
	if (value) {
		bitmap->buffer[word_index] |= ((unsigned long)1 << bit_index);
	} else {
		bitmap->buffer[word_index] &= ~((unsigned long)1 << bit_index);
	}
}

/* 设置内存位映射范围 */
void bitmap_set_range(Bitmap *bitmap, unsigned long start, unsigned long end, int value)
{
	unsigned long start_word = (start + 7) / 8;
	unsigned long end_word = end / 8;
	if (start >= end || start >= bitmap->length) {
		return;
	}
	for (unsigned long i = start; i < start_word * 8 && i < end; i++) {
		bitmap_set(bitmap, i, value);
	}
	if (start_word > end_word) {
		return;
	}
	if (start_word <= end_word) {
		unsigned long fill_value = value ? (unsigned long)-1 : 0;
		for (unsigned long i = start_word; i < end_word; i++) {
			bitmap->buffer[i] = fill_value;
		}
	}
	for (unsigned long i = end_word * 8; i < end; i++) {
		bitmap_set(bitmap, i, value);
	}
}

/* 内存位映射查找范围 */
unsigned long bitmap_find_range(const Bitmap *bitmap, unsigned long length, int value)
{
	unsigned long count = 0, start_index = 0;
	uint8_t byte_match = value ? (uint8_t)-1 : 0;
	for (unsigned long byte_idx = 0; byte_idx < bitmap->length / 8; byte_idx++) {
		unsigned long byte = bitmap->buffer[byte_idx];
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
			for (unsigned long bit = 0; bit < 8; bit++) {
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
	return (unsigned long)-1;
}
