/*
 *
 *      bitmap.c
 *      Memory bitmap
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "bitmap.h"
#include "string.h"

/* Initialize the memory bitmap */
void bitmap_init(bitmap_t *bitmap, uint8_t *buffer, size_t size) {
  bitmap->buffer = buffer;
  bitmap->length = size * 8;
  memset(buffer, 0, size);
}

/* Get memory bitmap */
int bitmap_get(const bitmap_t *bitmap, size_t index) {
  size_t word_index = index / 8;
  size_t bit_index = index % 8;
  return (bitmap->buffer[word_index] >> bit_index) & 1;
}

/* Setting the memory bitmap */
void bitmap_set(bitmap_t *bitmap, size_t index, int value) // NOLINT
{
  size_t word_index = index / 8;
  size_t bit_index = index % 8;
  if (value)
    bitmap->buffer[word_index] |= ((size_t)1 << bit_index);
  else
    bitmap->buffer[word_index] &= ~((size_t)1 << bit_index);
}

/* Set the memory bitmap range */
void bitmap_set_range(bitmap_t *bitmap, size_t start, size_t end,
                      int value) // NOLINT
{
  if (start >= end || start >= bitmap->length)
    return;
  while (start < end && (start % 8 != 0)) {
    bitmap_set(bitmap, start, value);
    start++;
  }
  size_t byte_start = start / 8;
  size_t byte_end = end / 8;
  uint8_t fill = value ? 0xff : 0x00;

  for (size_t i = byte_start; i < byte_end; i++)
    bitmap->buffer[i] = fill;
  start = byte_end * 8;

  while (start < end) {
    bitmap_set(bitmap, start, value);
    start++;
  }
}

/* Memory bitmap search range */
size_t bitmap_find_range(const bitmap_t *bitmap, size_t length,
                         int value) // NOLINT
{
  size_t count = 0, start_index = 0;
  uint8_t byte_match = value ? (uint8_t)-1 : 0;
  for (size_t byte_idx = 0; byte_idx < bitmap->length / 8; byte_idx++) {
    size_t byte = bitmap->buffer[byte_idx];
    if (byte == !byte_match) {
      count = 0;
    } else if (byte == byte_match) {
      if (length < 8)
        return byte_idx * 8;
      if (count == 0)
        start_index = byte_idx * 8;
      count += 8;
      if (count >= length)
        return start_index;
    } else {
      for (size_t bit = 0; bit < 8; bit++) {
        int bit_value = (int)((byte >> bit) & 1);
        if (bit_value == value) {
          if (count == 0)
            start_index = byte_idx * 8 + bit;
          count++;
          if (count == length)
            return start_index;
        } else {
          count = 0;
        }
      }
    }
  }
  return (size_t)-1;
}
