/*
 *
 *		memory.h
 *		内核内存分配头文件
 *
 *		2024/6/30 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
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
#include "multiboot.h"

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

void copy_page_physical(uintptr_t src, uintptr_t dst);

extern page_directory_t *kernel_directory;
extern page_directory_t *current_directory;
extern uint32_t end;
extern void *program_break;
extern void *program_break_end;
extern uint32_t placement_address;
extern char kern_stack[STACK_SIZE] __attribute__ ((aligned(16)));
extern uint32_t kern_stack_top;

/* 分配内存，同时返回物理地址 */
uint32_t kmalloc_i_ap(uint32_t size, uint32_t *phys);

/* 分配对齐的内存 */
uint32_t kmalloc_a(uint32_t size);

/* 分配内存并返回物理地址 */
uint32_t kmalloc_p(uint32_t size, uint32_t *phys);

/* 分配对齐的内存并返回物理地址 */
uint32_t kmalloc_ap(uint32_t size, uint32_t *phys);

/* 分配内存，不返回物理地址 */
uint32_t kmalloc(uint32_t size);

/* 重新调整内存块 */
void *krealloc(void *block, size_t size);

/* 改变进程的堆栈大小 */
void *ksbrk(int incr);

/* 尝试在现有的内存块中找到足够大的空闲块 */
void *alloc(size_t size);

/* 将内存块标记为空闲并尝试将其与相邻的空闲块合并 */
void kfree(void *block);

/* 清除帧位图中的特定位 */
uint32_t first_frame(void);

/* 分配一个帧给一个页表项 */
void alloc_frame(page_t *page, int is_kernel, int is_writable);

/* 手动分配特定帧给页表项 */
void alloc_frame_line(page_t *page, unsigned line,int is_kernel, int is_writable);

/* 释放页表项所占用的帧 */
void free_frame(page_t *page);

/* 刷新当前CPU的TLB并更新当前正在使用的页目录 */
void page_flush(page_directory_t *dir);

/* 切换当前进程的页目录 */
void page_switch(page_directory_t *dir);

/* 切换当前进程的页目录 */
void switch_page_directory(page_directory_t *dir);

/* 获取给定虚拟地址对应的页表项 */
page_t *get_page(uint32_t address, int make, page_directory_t *dir);

/* 克隆页目录 */
page_directory_t *clone_directory(page_directory_t *src);

/* 初始化内存分页 */
void init_page(multiboot_t *mboot);

#endif // UINXED_KERNEL_MEMORY_H
