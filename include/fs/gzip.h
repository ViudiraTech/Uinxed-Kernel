/*
 *
 *      gzip.h
 *      Gzip decompression
 *
 */

#ifndef INCLUDE_GZIP_H_
#define INCLUDE_GZIP_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

int gzip_decompress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size);

#endif // INCLUDE_GZIP_H_
