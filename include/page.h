/*
 *
 *		page.h
 *		内存页头文件
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_PAGE_H_
#define INCLUDE_PAGE_H_

#include "stdint.h"

#define PTE_PRESENT (0x1 << 0)
#define PTE_WRITEABLE (0x1 << 1)
#define PTE_USER (0x1 << 2)
#define PTE_HUGE (0x1 << 3)
#define PTE_NO_EXECUTE (((uint64_t)0x1) << 63)
#define KERNEL_PTE_FLAGS (PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE)

typedef struct page_table_entry {
	uint64_t value;
} page_table_entry_t;

typedef struct {
	page_table_entry_t entries[512];
} page_table_t;

typedef struct page_directory {
	page_table_t *table;
} page_directory_t;

/* 清空一个内存页表的所有条目 */
void page_table_clear(page_table_t *table);

/* 创建一个内存页表 */
page_table_t *page_table_create(page_table_entry_t *entry);

/* 返回内核的页目录 */
page_directory_t *get_kernel_pagedir(void);

/* 返回当前进程的页目录 */
page_directory_t *get_current_directory(void);

/* 递归地复制内存页表 */
void copy_page_table_recursive(page_table_t *source_table, page_table_t *new_table, int level);

/* 克隆一个页目录 */
page_directory_t *clone_directory(page_directory_t *src);

/* 将一个虚拟地址映射到物理帧 */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags);

/* 切换当前进程的页目录 */
void switch_page_directory(page_directory_t *dir);

/* 将一段连续的物理内存映射到虚拟地址空间 */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags);

/* 初始化内存页表 */
void page_init(void);

#endif // INCLUDE_PAGE_H_
