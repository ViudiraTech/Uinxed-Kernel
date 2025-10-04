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

#include "stddef.h"
#include "stdint.h"

#define MSR_IA32_PAT 0x277

#define PTE_PRESENT      (0x1 << 0)
#define PTE_WRITEABLE    (0x1 << 1)
#define PTE_USER         (0x1 << 2)
#define PTE_HUGE         (0x1 << 7)
#define PTE_NO_EXECUTE   (((uint64_t)0x1) << 63)
#define KERNEL_PTE_FLAGS (PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE)

#define PAGE_FLAGS_MASK (~(0xFFF0000000000FFFULL))

#define PAGE_SIZE 0x1000

typedef struct {
        uint64_t value;
} page_table_entry_t;

typedef struct {
        page_table_entry_t entries[512];
} page_table_t;

typedef struct {
        page_table_t *table;
} page_directory_t;

typedef struct {
        char    pat_str[64];
        uint8_t entries[8];
        uint8_t types[8];
} pat_config_t;

/* Determine whether the page table entry maps a huge page */
inline static int is_huge_page(page_table_entry_t *entry)
{
    return (((uint64_t)entry->value) & PTE_HUGE) != 0;
}

/* Enable paging with a phys page directory address */
static inline void enable_paging(uintptr_t page_directory_phys)
{
    __asm__ volatile("mov %0, %%cr3\n\t"
                     "mov %%cr0, %%rax\n\t"
                     "or $0x80000000, %%eax\n\t"
                     "mov %%rax, %%cr0\n\t"
                     "jmp 1f\n\t"
                     "1:\n\t"
                     :
                     : "r"(page_directory_phys)
                     : "eax", "memory");
}

/* Clear all entries in a memory page table */
void page_table_clear(page_table_t *table);

/* Create a memory page table */
page_table_t *page_table_create(page_table_entry_t *entry);

/* Returns the kernel's page directory */
page_directory_t *get_kernel_pagedir(void);

/* Returns the page directory of the current process */
page_directory_t *get_current_directory(void);

/* Recursively copy memory page tables using an explicit stack */
void copy_page_table_recursive(page_table_t *source_table, page_table_t *new_table, int level);

/* Recursively free memory page tables using an explicit stack */
void free_page_table_recursive(page_table_t *table, int level);

/* Clone a page directory */
page_directory_t *clone_directory(page_directory_t *src);

/* Free a page directory */
void free_directory(page_directory_t *dir);

/* Maps a virtual address to a physical frame */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags);

/* Switch the page directory of the current process */
void switch_page_directory(page_directory_t *dir);

/* Maps a contiguous physical memory range to the specified virtual address range */
void page_map_range(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t length, uint64_t flags);

/* Maps a contiguous physical memory range to virtual memory */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags);

/* Maps random non-contiguous physical pages to the virtual address range */
void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags);

/* Get the PAT configuration */
pat_config_t get_pat_config(void);

/* Initialize memory page table */
void page_init(void);

#endif // INCLUDE_PAGE_H_
