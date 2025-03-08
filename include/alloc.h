/*
 *
 *		alloc.h
 *		内存堆分配器头文件
 *
 *		2025/2/16 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_ALLOC_H_
#define INCLUDE_ALLOC_H_

/* 初始化一个内存堆 */
int heap_init(uint8_t *address, unsigned long size);

/* 在堆上分配一块指定大小的内存空间 */
void *malloc(unsigned long size);

/* 释放先前分配的内存 */
void free(void *ptr);

#endif // INCLUDE_ALLOC_H_
