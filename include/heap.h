/*
 *
 *		heap.h
 *		内存堆头文件
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_HEAP_H_
#define INCLUDE_HEAP_H_

#include "stdint.h"

/* 初始化内存堆 */
void init_heap(void);

/* 分配一块空内存 */
void *calloc(unsigned long a, unsigned long b);

#endif // INCLUDE_HEAP_H_
