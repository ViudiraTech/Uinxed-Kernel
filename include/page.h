/*
 *
 *      page.h
 *      Memory page header file
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_PAGE_H_
#define INCLUDE_PAGE_H_

#include "stdint.h"

#define PTE_PRESENT      (0x1 << 0)
#define PTE_WRITEABLE    (0x1 << 1)
#define PTE_USER         (0x1 << 2)
#define PTE_HUGE         (0x1 << 7)
#define PTE_NO_EXECUTE   (((uint64_t)0x1) << 63)
#define KERNEL_PTE_FLAGS (PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE)

#define PAGE_SIZE 0x1000

typedef struct page_table_entry {
        uint64_t value;
} page_table_entry_t;

typedef struct {
        page_table_entry_t entries[512];
} page_table_t;

typedef struct page_directory {
        page_table_t *table;
} page_directory_t;

/* Clear all entries in a memory page table */
void page_table_clear(page_table_t *table);

/* Create a memory page table */
page_table_t *page_table_create(page_table_entry_t *entry);

/* Returns the kernel's page directory */
page_directory_t *get_kernel_pagedir(void);

/* Returns the page directory of the current process */
page_directory_t *get_current_directory(void);

/* Recursively copy memory page tables */
void copy_page_table_recursive(page_table_t *source_table, page_table_t *new_table, int level);

/* Recursively free memory page tables */
void free_page_table_recursive(page_table_t *table, int level);

/* Clone a page directory */
page_directory_t *clone_directory(page_directory_t *src);

/* Free a page directory */
void free_directory(page_directory_t *dir);

/* Maps a virtual address to a physical frame */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags);

/* Switch the page directory of the current process */
void switch_page_directory(page_directory_t *dir);

/* Map a continuous section of physical memory to the virtual address space */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags);

/* Mapping random portions of non-contiguous physical memory into the virtual address space */
void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags);

/* Initialize memory page table */
void page_init(void);

#endif // INCLUDE_PAGE_H_
