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

#include "types.h"

uintptr_t align_down(uintptr_t addr, uintptr_t size);
uintptr_t align_up(uintptr_t addr, uintptr_t size);

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define HUGE_PAGE_SIZE 0x400000
#define HUGE_PAGE_SHIFT 22

#define PT_P  0x01
#define PT_W  0x02
#define PT_U  0x04
#define PT_PS 0x80
#define PT_WT 0x08
#define PT_CD 0x10
#define PT_ADDRESS(x) ((x)&0xfffff000)
#define PT_ENTRIES (PAGE_SIZE/sizeof(uintptr_t))

/* 参考 boot.h 里的说明 */
#define CURRENT_PD_BASE   0xFFFFF000UL
#define SCRATCH_PD_BASE   0xFFFFE000UL
#define CURRENT_PT_BASE   0xFFC00000UL
#define SCRATCH_PT_BASE   0xFF800000UL
#define KERNEL_STACK_BASE 0xFF000000UL
#define KERNEL_STACK_TOP  0xFF800000UL
#define KERNEL_STACK_SIZE 32768

#define USER_STACK_TOP 0xb2000000
#define USER_AREA_START 0x90000000
#define USER_AREA_SIZE 0x2000000

#define FRAMEINFO_NONNULL 0x01

uintptr_t frame_alloc(size_t npages);
void frame_free(uintptr_t addr);

extern uintptr_t program_break;
extern uintptr_t program_break_end;

void page_map(uintptr_t vaddr, uintptr_t entry);
void page_alloc(uintptr_t vaddr, uintptr_t flags);
void *page_map_kernel_range(uintptr_t start, uintptr_t end, uintptr_t flags);

extern size_t kh_usage_memory_byte;

/* 返回内核使用的内存量 */
uint32_t get_kernel_memory_usage(void);

/* 释放一个页目录 */
void free_directory(uintptr_t dir);

/* 新建页目录 */
uintptr_t create_directory(void);

void init_frame(void);

/* 初始化内存分页 */
void init_page(void);

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
