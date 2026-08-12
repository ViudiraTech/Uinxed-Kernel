/*
 *
 *      gzip.h
 *      Gzip decompression
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_GZIP_H_
#define INCLUDE_GZIP_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Decompress a gzip stream into a freshly allocated buffer. */
int gzip_decompress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size);

#endif // INCLUDE_GZIP_H_
