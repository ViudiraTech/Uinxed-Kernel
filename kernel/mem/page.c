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
#include "idt.h"
#include "printk.h"
#include "stdlib.h"
#include "string.h"

page_directory_t  kernel_page_dir;
page_directory_t *current_directory = 0;

/* Page fault handling */
__attribute__((interrupt)) void page_fault_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    (void)frame;
    disable_intr();

    uint64_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));

    int      present  = !(error_code & 0x1); // Page does not exist
    uint64_t rw       = error_code & 0x2;    // Read-only page is written
    uint64_t us       = error_code & 0x4;    // User mode writes to kernel page
    uint64_t reserved = error_code & 0x8;    // Write CPU reserved bits
    uint64_t id       = error_code & 0x10;   // Caused by instruction fetch

    if (present)
        panic("PAGE_FAULT-Present-Address: 0x%016llx", faulting_address);
    else if (rw)
        panic("PAGE_FAULT-ReadOnly-Address: 0x%016llx", faulting_address);
    else if (us)
        panic("PAGE_FAULT-UserMode-Address: 0x%016llx", faulting_address);
    else if (reserved)
        panic("PAGE_FAULT-Reserved-Address: 0x%016llx", faulting_address);
    else if (id)
        panic("PAGE_FAULT-DecodeAddress-Address: 0x%016llx", faulting_address);
}

/* Determine whether the page table entry maps a huge page */
static int is_huge_page(page_table_entry_t *entry)
{
    return (((uint64_t)entry->value) & PTE_HUGE) != 0;
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

/* Iteratively copy memory page tables using an explicit stack */
void copy_page_table_iterative(page_table_t *source_table, page_table_t *new_table, int level)
{
    struct StackFrame {
            page_table_t *source_table;
            page_table_t *new_table;
            int           level;
            int           i;
    } stack[32];
    int top      = -1;
    stack[++top] = (struct StackFrame) {source_table, new_table, level, 0};
    while (top >= 0) {
        struct StackFrame frame = stack[top--];
        if (frame.level == 0) {
            for (int j = 0; j < 512; j++) frame.new_table->entries[j].value = frame.source_table->entries[j].value;
            continue;
        }
        for (; frame.i < 512; frame.i++) {
            int i = frame.i;
            if (frame.source_table->entries[i].value == 0) {
                frame.new_table->entries[i].value = 0;
                continue;
            }
            page_table_t *source_next_level   = (page_table_t *)phys_to_virt(frame.source_table->entries[i].value & 0xfffffffffffff000);
            page_table_t *new_next_level      = page_table_create(&(frame.new_table->entries[i]));
            frame.new_table->entries[i].value = (uint64_t)new_next_level | (frame.source_table->entries[i].value & 0xfff);
            frame.i++;
            stack[++top] = frame;
            stack[++top] = (struct StackFrame) {
                .source_table = source_next_level,
                .new_table    = new_next_level,
                .level        = frame.level - 1,
                .i            = 0,
            };
            break;
        }
    }
}

/* Iteratively free memory page tables using an explicit stack */
void free_page_table_iterative(page_table_t *table, int level)
{
    void *phys_addr;
    struct StackFrame {
            page_table_t *table;
            int           level;
            int           i;
    } stack[32];
    int top      = -1;
    stack[++top] = (struct StackFrame) {table, level, 0};
    while (top >= 0) {
        struct StackFrame frame = stack[top--];
        while (frame.i < 512) {
            page_table_entry_t *entry = &frame.table->entries[frame.i];
            if (entry->value == 0 || is_huge_page(entry)) {
                frame.i++;
                continue;
            }
            if (frame.level == 1) {
                if ((entry->value & PTE_PRESENT) && (entry->value & PTE_WRITEABLE) && (entry->value & PTE_USER)) {
                    free_frame(entry->value & 0x000fffffffff000);
                }
                frame.i++;
                continue;
            }
            page_table_t *child_table = (page_table_t *)phys_to_virt(entry->value & 0x000fffffffff000);
            stack[++top]              = (struct StackFrame) {frame.table, frame.level, frame.i + 1};
            stack[++top]              = (struct StackFrame) {child_table, frame.level - 1, 0};
            break;
        }
        if (frame.i >= 512) {
            phys_addr = virt_to_phys((uint64_t)frame.table);
            free_frame((uint64_t)phys_addr & 0x000fffffffff000);
        }
    }
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
    copy_page_table_iterative(src->table, new_directory->table, 3);
    return new_directory;
}

/* Free a page directory */
void free_directory(page_directory_t *dir)
{
    free_page_table_iterative(dir->table, 3);
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

    l1_table->entries[l1_index].value = (frame & 0xfffffffffffff000) | flags;
    flush_tlb(addr);
}

/* Switch the page directory of the current process */
void switch_page_directory(page_directory_t *dir)
{
    current_directory            = dir;
    page_table_t *physical_table = virt_to_phys((uint64_t)dir->table);
    __asm__ volatile("mov %0, %%cr3" ::"r"(physical_table));
}

/* Map a continuous section of physical memory to the virtual address space */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags) // NOLINT
{
    for (uint64_t i = 0; i < length; i += 0x1000) page_map_to(directory, (uint64_t)phys_to_virt(frame + i), frame + i, flags);
}

/* Mapping random portions of non-contiguous physical memory into the virtual address space */
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
