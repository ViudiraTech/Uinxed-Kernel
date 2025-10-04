/*
 *
 *      page.c
 *      Memory Pages
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "page.h"
#include "alloc.h"
#include "common.h"
#include "debug.h"
#include "frame.h"
#include "hhdm.h"
#include "interrupt.h"
#include "printk.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"

page_directory_t  kernel_page_dir;
page_directory_t *current_directory = 0;

/* Page fault handling */
INTERRUPT_BEGIN void page_fault_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    (void)frame;
    disable_intr();

    uint64_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));

    int         present  = !(error_code & 0x1); // Page does not exist
    uint64_t    rw       = error_code & 0x2;    // Read-only page is written
    uint64_t    us       = error_code & 0x4;    // User mode writes to kernel page
    uint64_t    reserved = error_code & 0x8;    // Write CPU reserved bits
    uint64_t    id       = error_code & 0x10;   // Caused by instruction fetch
    const char *pf_msg   = "Unknown";

    if (present)
        pf_msg = "Present";
    else if (rw)
        pf_msg = "ReadOnly";
    else if (us)
        pf_msg = "UserMode";
    else if (reserved)
        pf_msg = "Reserved";
    else if (id)
        pf_msg = "DecodeAddress";

    carry_error_code = 1; // carry error code
    panic("PAGE_FAULT-%s-Address: 0x%016llx", pf_msg, faulting_address);
}
INTERRUPT_END

/* Determine whether the page table entry maps a huge page */
int is_huge_page(page_table_entry_t *entry)
{
    return (((uint64_t)entry->value) & PTE_HUGE) != 0;
}

/* Enable paging with a phys page directory address */
void enable_paging(uintptr_t page_directory_phys)
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
void page_table_clear(page_table_t *table)
{
    for (int i = 0; i < 512; i++) table->entries[i].value = 0;
}

/* Create a memory page table */
page_table_t *page_table_create(page_table_entry_t *entry)
{
    if (entry->value == 0) {
        uint64_t frame      = alloc_frames(1);
        entry->value        = frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
        page_table_t *table = (page_table_t *)phys_to_virt(entry->value & 0x000fffffffff000);
        page_table_clear(table);
        return table;
    }
    page_table_t *table = (page_table_t *)phys_to_virt(entry->value & 0x000fffffffff000);
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
void copy_page_table_recursive(page_table_t *source_table, page_table_t *new_table, int level) // NOLINT
{
    if (level == 0) {
        for (int i = 0; i < 512; i++) new_table->entries[i].value = source_table->entries[i].value;
        return;
    }
    for (int i = 0; i < 512; i++) {
        if (source_table->entries[i].value == 0) {
            new_table->entries[i].value = 0;
            continue;
        }
        page_table_t *source_next_level = (page_table_t *)phys_to_virt(source_table->entries[i].value & 0x000fffffffff000);
        page_table_t *new_next_level    = page_table_create(&(new_table->entries[i]));
        new_table->entries[i].value     = (uint64_t)new_next_level | (source_table->entries[i].value & 0xfff);
        copy_page_table_recursive(source_next_level, new_next_level, level - 1);
    }
}

/* Recursively free memory page tables */
void free_page_table_recursive(page_table_t *table, int level) // NOLINT
{
    uint64_t virtual_address  = (uint64_t)table;
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
    uint64_t          frame         = alloc_frames(1);
    if (frame == 0) {
        free(new_directory);
        return 0;
    }
    new_directory->table = (page_table_t *)phys_to_virt(frame);
    memset(new_directory->table, 0, sizeof(page_table_t));
    copy_page_table_recursive(src->table, new_directory->table, 3);
    return new_directory;
}

/* Free a page directory */
void free_directory(page_directory_t *dir)
{
    free_page_table_recursive(dir->table, 3);
    free_frame((uint64_t)virt_to_phys((uint64_t)dir->table));
    free(dir);
}

/* Maps a virtual address to a physical frame */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags) // NOLINT
{
    uint64_t l4_index = (((addr >> 39)) & 0x1ff);
    uint64_t l3_index = (((addr >> 30)) & 0x1ff);
    uint64_t l2_index = (((addr >> 21)) & 0x1ff);
    uint64_t l1_index = (((addr >> 12)) & 0x1ff);

    page_table_t *l4_table = directory->table;
    page_table_t *l3_table = page_table_create(&(l4_table->entries[l4_index]));
    page_table_t *l2_table = page_table_create(&(l3_table->entries[l3_index]));
    page_table_t *l1_table = page_table_create(&(l2_table->entries[l2_index]));

    l1_table->entries[l1_index].value = (frame & 0x000fffffffff000) | flags;
    flush_tlb(addr);
}

/* Switch the page directory of the current process */
void switch_page_directory(page_directory_t *dir)
{
    current_directory            = dir;
    page_table_t *physical_table = virt_to_phys((uint64_t)dir->table);
    __asm__ volatile("mov %0, %%cr3" ::"r"(physical_table));
}

/* Maps a contiguous physical memory range to the specified virtual address range */
void page_map_range(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t length, uint64_t flags) // NOLINT
{
    for (uint64_t i = 0; i < length; i += 0x1000) page_map_to(directory, (uint64_t)addr + i, frame + i, flags);
}

/* Maps a contiguous physical memory range to virtual memory */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags) // NOLINT
{
    for (uint64_t i = 0; i < length; i += 0x1000) page_map_to(directory, (uint64_t)phys_to_virt(frame + i), frame + i, flags);
}

/* Maps random non-contiguous physical pages to the virtual address range */
void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags) // NOLINT
{
    uint64_t frame = 0;
    for (uint64_t i = 0; i < length; i += 0x1000) {
        frame = alloc_frames(1);
        if (frame != 0) page_map_to(directory, addr + i, frame, flags);
    }
}

/* Get the PAT configuration */
pat_config_t get_pat_config(void)
{
    pat_config_t config       = {0};
    const char  *pat_types[8] = {"WB ", "WC ", "UC-", "UC ", "WB ", "WP ", "UC-", "WT "};
    uint64_t     pat_value    = rdmsr(MSR_IA32_PAT);
    int          pos          = 0;

    for (int i = 0; i < 8; i++) {
        uint8_t entry = (pat_value >> (i * 8)) & 0xff;
        uint8_t type  = entry & 0x7;
        if (type > 7) type = 0;

        config.entries[i] = entry;
        config.types[i]   = type;
        pos += sprintf(config.pat_str + pos, "%s ", pat_types[type]);
    }
    if (pos > 0) config.pat_str[pos - 1] = '\0';
    return config;
}

/* Initialize memory page table */
void page_init(void)
{
    page_table_t *kernel_page_table = phys_to_virt(get_cr3());
    kernel_page_dir                 = (page_directory_t) {.table = kernel_page_table};
    current_directory               = &kernel_page_dir;
}
