/*
 *
 *		page.c
 *		内存页
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "page.h"
#include "hhdm.h"
#include "common.h"
#include "idt.h"
#include "frame.h"
#include "alloc.h"
#include "printk.h"
#include "debug.h"

page_directory_t kernel_page_dir;
page_directory_t *current_directory = 0;

/* 页错误处理 */
__attribute__((interrupt)) static void page_fault_handle(interrupt_frame_t *frame, uint64_t error_code)
{
	(void)frame;
	disable_intr();
	uint64_t faulting_address;
	__asm__ __volatile__("mov %%cr2, %0" : "=r"(faulting_address));

	int present = !(error_code & 0x1);	// 页不存在
	int rw = error_code & 0x2;			// 只读页被写入
	int us = error_code & 0x4;			// 用户态写入内核页
	int reserved = error_code & 0x8;	// 写入CPU保留位
	int id = error_code & 0x10;			// 由取指引起

	if (present) {
		panic("PAGE_FAULT-Present-Address: 0x%016x", faulting_address);
	} else if (rw) {
		panic("PAGE_FAULT-ReadOnly-Address: 0x%016x", faulting_address);
	} else if (us) {
		panic("PAGE_FAULT-UserMode-Address: 0x%016x", faulting_address);
	} else if (reserved) {
		panic("PAGE_FAULT-Reserved-Address: 0x%016x", faulting_address);
	} else if (id) {
		panic("PAGE_FAULT-DecodeAddress-Address: 0x%016x", faulting_address);
	}
}

/* 清空一个内存页表的所有条目 */
void page_table_clear(page_table_t *table)
{
	for (int i = 0; i < 512; i++) {
		table->entries[i].value = 0;
	}
}

/* 创建一个内存页表 */
page_table_t *page_table_create(page_table_entry_t *entry)
{
	if (entry->value == (uint64_t)0) {
		uint64_t frame = alloc_frames(1);
		entry->value = frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
		page_table_t *table = (page_table_t *)phys_to_virt(entry->value & 0xFFFFFFFFFFFFF000);
		page_table_clear(table);
		return table;
	}
	page_table_t *table = (page_table_t *)phys_to_virt(entry->value & 0xFFFFFFFFFFFFF000);
	return table;
}

/* 返回内核的页目录 */
page_directory_t *get_kernel_pagedir(void)
{
	return &kernel_page_dir;
}

/* 返回当前进程的页目录 */
page_directory_t *get_current_directory(void)
{
	return current_directory;
}

/* 递归地复制内存页表 */
void copy_page_table_recursive(page_table_t *source_table, page_table_t *new_table, int level)
{
	if (level == 0) {
		for (int i = 0; i < 512; i++) {
			new_table->entries[i].value = source_table->entries[i].value;
		}
		return;
	}
	for (int i = 0; i < 512; i++) {
		if (source_table->entries[i].value == 0) {
			new_table->entries[i].value = 0;
			continue;
		}
		page_table_t *source_next_level = (page_table_t *)phys_to_virt(source_table->entries[i].value & 0xFFFFFFFFFFFFF000);
		page_table_t *new_next_level = page_table_create(&(new_table->entries[i]));
		new_table->entries[i].value = (uint64_t)new_next_level | (source_table->entries[i].value & 0xFFF);
		copy_page_table_recursive(source_next_level, new_next_level, level - 1);
	}
}

/* 克隆一个页目录 */
page_directory_t *clone_directory(page_directory_t *src)
{
	page_directory_t *new_directory = malloc(sizeof(page_directory_t));
	new_directory->table = malloc(sizeof(page_table_t));
	copy_page_table_recursive(src->table, new_directory->table, 3);
	return new_directory;
}

/* 将一个虚拟地址映射到物理帧 */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
	uint64_t l4_index = (((addr >> 39)) & 0x1FF);
	uint64_t l3_index = (((addr >> 30)) & 0x1FF);
	uint64_t l2_index = (((addr >> 21)) & 0x1FF);
	uint64_t l1_index = (((addr >> 12)) & 0x1FF);

	page_table_t *l4_table = directory->table;
	page_table_t *l3_table = page_table_create(&(l4_table->entries[l4_index]));
	page_table_t *l2_table = page_table_create(&(l3_table->entries[l3_index]));
	page_table_t *l1_table = page_table_create(&(l2_table->entries[l2_index]));

	l1_table->entries[l1_index].value = (frame & 0xFFFFFFFFFFFFF000) | flags;
	flush_tlb(addr);
}

/* 切换当前进程的页目录 */
void switch_page_directory(page_directory_t *dir)
{
	current_directory = dir;
	page_table_t *physical_table = virt_to_phys((uint64_t)dir->table);
	__asm__ __volatile__("mov %0, %%cr3" :: "r"(physical_table));
}

/* 将一段连续的物理内存映射到虚拟地址空间 */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags)
{
	for (uint64_t i = 0; i < length; i += 0x1000) {
		uint64_t var = (uint64_t)phys_to_virt(frame + i);
		page_map_to(directory, var, frame + i, flags);
	}
}

/* 初始化内存页表 */
void page_init(void)
{
	page_table_t *kernel_page_table = (page_table_t *)phys_to_virt(get_cr3());
	plogk("Kernel Page Table base address: 0x%016x\n", (unsigned long long)kernel_page_table);

	kernel_page_dir = (page_directory_t){.table = kernel_page_table};
	plogk("Kernel Page Directory initialized at 0x%016x\n", (unsigned long long)kernel_page_dir.table);

	register_interrupt_handler(ISR_14, (void *)page_fault_handle, 0, 0x8e);

	current_directory = &kernel_page_dir;
	plogk("Current Page Directory set to 0x%016x\n", (unsigned long long)current_directory->table);
}
