/*
 *
 *		memory.h
 *		内核内存分配头文件
 *
 *		2024/6/30 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef UINXED_KERNEL_MEMORY_H
#define UINXED_KERNEL_MEMORY_H


#define KHEAP_INITIAL_SIZE	0xf00000
#define KHEAP_START			0xc0000000
#define STACK_SIZE			32768

#define INDEX_FROM_BIT(a)	(a / (8*4))
#define OFFSET_FROM_BIT(a)	(a % (8*4))

typedef char ALIGN[16];

#include "types.h"

typedef struct page {
	uint32_t present: 1;
	uint32_t rw: 1;
	uint32_t user: 1;
	uint32_t accessed: 1;
	uint32_t dirty: 1;
	uint32_t unused: 7;
	uint32_t frame: 20;
} page_t;

typedef struct page_table {
	page_t pages[1024];
} page_table_t;

typedef struct page_directory {
	page_table_t *tables[1024];
	uint32_t tablesPhysical[1024];
	uint32_t physicalAddr;
} page_directory_t;

typedef union header {
	struct {
		uint32_t size;
		uint32_t is_free;
		union header *next;
	} s;
	ALIGN stub;
} header_t;

/* 初始化内存分页 */
void init_page(void);

/* 获取给定虚拟地址对应的页表项 */
page_t *get_page(uint32_t address, int make, page_directory_t *dir);

/* 将内存块标记为空闲并尝试将其与相邻的空闲块合并 */
void kfree(void *block);

/* 分配内存，同时返回物理地址 */
uint32_t kmalloc_i_ap(uint32_t size, uint32_t *phys); // 该malloc的内存不可回收, 谨慎使用

/* 分配对齐的内存 */
uint32_t kmalloc_a(uint32_t size);

/* 分配内存并返回物理地址 */
uint32_t kmalloc_p(uint32_t size, uint32_t *phys);

/* 分配对齐的内存并返回物理地址 */
uint32_t kmalloc_ap(uint32_t size, uint32_t *phys);

/* 分配内存，不返回物理地址 */
uint32_t kmalloc(uint32_t size);

/* 改变进程的堆栈大小 */
void *ksbrk(int incr);

/* 尝试在现有的内存块中找到足够大的空闲块 */
void *alloc(size_t size);

#endif
