/*
 *
 *		page.c
 *		Memory Pages
 *
 *		2025/2/16 By XIAOYI12
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
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

/* Page fault handling */
__attribute__((interrupt)) static void page_fault_handle(interrupt_frame_t *frame, uint64_t error_code)
{
	(void)frame;
	disable_intr();
	uint64_t faulting_address;
	__asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_address));

	int present = !(error_code & 0x1);	// Page does not exist
	int rw = error_code & 0x2;			// Read-only page is written
	int us = error_code & 0x4;			// User mode writes to kernel page
	int reserved = error_code & 0x8;	// Write CPU reserved bits
	int id = error_code & 0x10;			// Caused by instruction fetch

	if (present)
		panic("PAGE_FAULT-Present-Address: 0x%016x", faulting_address);
	else if (rw)
		panic("PAGE_FAULT-ReadOnly-Address: 0x%016x", faulting_address);
	else if (us)
		panic("PAGE_FAULT-UserMode-Address: 0x%016x", faulting_address);
	else if (reserved)
		panic("PAGE_FAULT-Reserved-Address: 0x%016x", faulting_address);
	else if (id)
		panic("PAGE_FAULT-DecodeAddress-Address: 0x%016x", faulting_address);
}

/* Determine whether the page table entry maps a huge page */
static int is_huge_page(page_table_entry_t *entry)
{
	return (((uint64_t)entry->value) & PTE_HUGE) != 0;
}

/* Clear all entries in a memory page table */
void page_table_clear(page_table_t *table)
{
	for (int i = 0; i < 512; i++)
		table->entries[i].value = 0;
}

/* Create a memory page table */
page_table_t *page_table_create(page_table_entry_t *entry)
{
	if (entry->value == (uint64_t)0) {
		uint64_t frame = alloc_frames(1);
		entry->value = frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
		page_table_t *table = (page_table_t *)phys_to_virt(entry->value & 0xfffffffffffff000);
		page_table_clear(table);
		return table;
	}
	page_table_t *table = (page_table_t *)phys_to_virt(entry->value & 0xfffffffffffff000);
	return table;
}

/* Returns the kernel's page directory */
page_directory_t *get_kernel_pagedir(void)
{
	return &kernel_page_dir;
}

/* Returns the page directory of the current process */
page_directory_t *get_current_directory(void)
{
	return current_directory;
}

/* Recursively copy memory page tables */
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
		page_table_t *source_next_level = (page_table_t *)phys_to_virt(source_table->entries[i].value & 0xfffffffffffff000);
		page_table_t *new_next_level = page_table_create(&(new_table->entries[i]));
		new_table->entries[i].value = (uint64_t)new_next_level | (source_table->entries[i].value & 0xfff);
		copy_page_table_recursive(source_next_level, new_next_level, level - 1);
	}
}

/* Recursively free memory page tables */
void free_page_table_recursive(page_table_t *table, int level)
{
	uint64_t virtual_address = (uint64_t)table;
	uint64_t physical_address = (uint64_t)virt_to_phys(virtual_address);

	if (level == 0) {
		free_frame(physical_address & 0x000fffffffff000);
		return;
	}
	for (int i = 0; i < 512; i++) {
		page_table_entry_t *entry = &table->entries[i];
		if (entry->value == 0 || is_huge_page(entry)) continue;
		if (level == 1) {
			if (entry->value & PTE_PRESENT && entry->value & PTE_WRITEABLE && entry->value & PTE_USER) {
				free_frame(entry->value & 0x000fffffffff000);
			}
		} else {
			free_page_table_recursive(phys_to_virt(entry->value & 0x000fffffffff000), level - 1);
		}
	}
	free_frame(physical_address & 0x000fffffffff000);
}

/* Clone a page directory */
page_directory_t *clone_directory(page_directory_t *src)
{
	page_directory_t *new_directory = malloc(sizeof(page_directory_t));
	new_directory->table = malloc(sizeof(page_table_t));
	copy_page_table_recursive(src->table, new_directory->table, 3);
	return new_directory;
}

/* Free a page directory */
void free_directory(page_directory_t *dir)
{
	free_page_table_recursive(dir->table, 4);
	free_frame((uint64_t)virt_to_phys((uint64_t)dir->table));
	free(dir);
}

/* Maps a virtual address to a physical frame */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
	uint64_t l4_index = (((addr >> 39)) & 0x1ff);
	uint64_t l3_index = (((addr >> 30)) & 0x1ff);
	uint64_t l2_index = (((addr >> 21)) & 0x1ff);
	uint64_t l1_index = (((addr >> 12)) & 0x1ff);

	page_table_t *l4_table = directory->table;
	page_table_t *l3_table = page_table_create(&(l4_table->entries[l4_index]));
	page_table_t *l2_table = page_table_create(&(l3_table->entries[l3_index]));
	page_table_t *l1_table = page_table_create(&(l2_table->entries[l2_index]));

	l1_table->entries[l1_index].value = (frame & 0xfffffffffffff000) | flags;
	flush_tlb(addr);
}

/* Switch the page directory of the current process */
void switch_page_directory(page_directory_t *dir)
{
	current_directory = dir;
	page_table_t *physical_table = virt_to_phys((uint64_t)dir->table);
	__asm__ volatile ("mov %0, %%cr3" :: "r"(physical_table));
}

/* Map a continuous section of physical memory to the virtual address space */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags)
{
	for (uint64_t i = 0; i < length; i += 0x1000) {
		uint64_t var = (uint64_t)phys_to_virt(frame + i);
		page_map_to(directory, var, frame + i, flags);
	}
}

/* Initialize memory page table */
void page_init(void)
{
	page_table_t *kernel_page_table = (page_table_t *)phys_to_virt(get_cr3());
	plogk("Page: Kernel page table base at 0x%016x (CR3 = 0x%016x)\n", kernel_page_table, get_cr3());

	kernel_page_dir = (page_directory_t){.table = kernel_page_table};
	plogk("Page: Kernel page directory initialized.\n");

	register_interrupt_handler(ISR_14, (void *)page_fault_handle, 0, 0x8e);

	current_directory = &kernel_page_dir;
	plogk("Page: Current directory set to kernel (0x%08x)\n", current_directory->table);
}
