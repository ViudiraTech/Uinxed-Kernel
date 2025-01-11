/*
 *
 *		memory.h
 *		内核内存分配头文件
 *
 *		2024/12/7 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_MEMORY_H_
#define INCLUDE_MEMORY_H_

#include "multiboot.h"
#include "types.h"

#define PAGE_SIZE 4096
#define MAX_FREE_QUEUE 64

#define KHEAP_INITIAL_SIZE 0xf00000
#define STACK_SIZE 32768

#define USER_STACK_TOP 0xb2000000
#define USER_AREA_START 0x90000000
#define USER_AREA_SIZE 0x2000000

#define INDEX_FROM_BIT(a) (a / (8*4))
#define OFFSET_FROM_BIT(a) (a % (8*4))

#define page_line(ptr) do { \
	alloc_frame_line(get_page((uint32_t)ptr, 1, current_directory), (uint32_t)ptr, 1, 1);                       \
} while(0)

typedef struct page {
	uint8_t present: 1;
	uint8_t rw: 1;
	uint8_t user:1;
	uint8_t pwt:1;
	uint8_t pcd:1;
	uint8_t accessed: 1;
	uint8_t dirty: 1;
	uint8_t pat: 1;
	uint8_t global: 1;
	uint8_t ignored: 3;
	uint32_t frame: 20;
} __attribute__((packed)) page_t;

typedef struct page_table {
	page_t pages[1024];
} __attribute__((packed)) page_table_t;

typedef struct page_directory {
	page_table_t *tables[1024];
	uint32_t table_phy[1024];
	uint32_t physicalAddr;
} __attribute__((packed)) page_directory_t;

union overhead {
	union overhead *ov_next;
	struct {
		unsigned char ovu_magic;
		unsigned char ovu_index;
	} ovu;
	#define ov_magic ovu.ovu_magic
	#define ov_index ovu.ovu_index
	#define ov_rmagic ovu.ovu_rmagic
	#define ov_size ovu.ovu_size
};

extern void *program_break;
extern void *program_break_end;
extern char kern_stack[STACK_SIZE] __attribute__ ((aligned(16)));
extern uint32_t kern_stack_top;
extern page_directory_t *kernel_directory;
extern page_directory_t *current_directory;

void copy_page_physical(uintptr_t src, uintptr_t dst);

/* 清除帧位图中的特定位 */
uint32_t first_frame(void);

/* 分配一个帧给一个页表项 */
void alloc_frame(page_t *page, int is_kernel, int is_writable);

/* 手动分配特定帧给页表项 */
void alloc_frame_line(page_t *page, uint32_t line,int is_kernel, int is_writable);

/* 释放页表项所占用的帧 */
void free_frame(page_t *page);

/* 切换当前进程的页目录 */
void switch_page_directory(page_directory_t *dir);

/* 获取给定虚拟地址对应的页表项 */
page_t *get_page(uint32_t address, int make, page_directory_t *dir);

/* 释放一个页目录 */
void free_directory(page_directory_t *dir);

/* 初始化一个用于存储空闲页目录的FIFO */
void setup_free_page(void);

/* 返回内核使用的内存量 */
uint32_t get_kernel_memory_usage(void);

/* 克隆页目录 */
page_directory_t *clone_directory(page_directory_t *src);

/* 初始化内存分页 */
void init_page(multiboot_t *multiboot);

/* 分配内存并返回地址 */
void *kmalloc(size_t nbytes);

/* 分配内存并返回清零后的内存地址 */
void *kcalloc(size_t nelem, size_t elsize);

/* 释放分配的内存并合并 */
void kfree(void *cp);

/* 计算分配的内存块的实际可用大小 */
size_t kmalloc_usable_size(void *cp);

/* 重新分配内存区域 */
void *krealloc(void *cp, size_t nbytes);

#endif // INCLUDE_MEMORY_H_
