/*
 *
 *      random.h
 *      Shared byte source for memory devices and kernel identifiers
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RANDOM_H_
#define INCLUDE_RANDOM_H_

#include <libs/std/stddef.h>

void mem_random_bytes(void *buffer, size_t size);

#endif // INCLUDE_RANDOM_H_
