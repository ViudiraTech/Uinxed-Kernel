/*
 *
 *      page.c
 *      Memory pages
 *
 *      2025/2/16 By XIAOYI12
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <debug.h>
#include <frame.h>
#include <heap.h>
#include <hhdm.h>
#include <interrupt.h>
#include <page.h>
#include <printk.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
#if defined(__x86_64__) || defined(_M_X64)
    uint64_t cr3_val = page_directory_phys;
    __asm__ volatile("mfence\n\t"
                     "mov %0, %%cr3\n\t"
                     "mov %%cr0, %%rax\n\t"
                     "orl $0x80000000, %%eax\n\t"
                     "mov %%rax, %%cr0\n\t"
                     "jmp 1f\n\t"
                     "1:\n\t"
                     "mov %%cr3, %%rax\n\t"
                     "mov %%rax, %%cr3\n\t"
                     :
                     : "r"(cr3_val)
                     : "rax", "memory");
#else
    uint32_t cr3_val = (uint32_t)page_directory_phys;
    __asm__ volatile("mfence\n\t"
                     "mov %0, %%cr3\n\t"
                     "mov %%cr0, %%eax\n\t"
                     "orl $0x80000000, %%eax\n\t"
                     "mov %%eax, %%cr0\n\t"
                     "jmp 1f\n\t"
                     "1:\n\t"
                     "mov %%cr3, %%eax\n\t"
                     "mov %%eax, %%cr3\n\t"
                     :
                     : "r"(cr3_val)
                     : "eax", "memory");
#endif
}

/* Clear all entries in a memory page table */
void page_table_clear(page_table_t *table)
{
    for (int i = 0; i < 512; i++) table->entries[i].value = 0;
}

/* Create a memory page table */
page_table_t *page_table_create(page_table_entry_t *entry)
{
    if (!entry->value) {
        uint64_t frame      = alloc_frames(1);
        entry->value        = frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
        page_table_t *table = (page_table_t *)phys_to_virt(entry->value & PAGE_4K_MASK);
        page_table_clear(table);
        return table;
    }
    page_table_t *table = (page_table_t *)phys_to_virt(entry->value & PAGE_4K_MASK);
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
    if (!level) {
        for (int i = 0; i < 512; i++) new_table->entries[i].value = source_table->entries[i].value;
        return;
    }
    for (int i = 0; i < 512; i++) {
        if (!source_table->entries[i].value) {
            new_table->entries[i].value = 0;
            continue;
        }
        page_table_t *source_next_level = (page_table_t *)phys_to_virt(source_table->entries[i].value & PAGE_4K_MASK);
        page_table_t *new_next_level    = page_table_create(&(new_table->entries[i]));
        new_table->entries[i].value     = (uint64_t)new_next_level | (source_table->entries[i].value & 0xfff);
        copy_page_table_recursive(source_next_level, new_next_level, level - 1);
    }
}

/* Recursively free memory page tables */
void free_page_table_recursive(page_table_t *table, int level)
{
    uint64_t virtual_address  = (uint64_t)table;
    uint64_t physical_address = (uint64_t)virt_to_phys(virtual_address);

    if (!level) {
        free_frame(physical_address & PAGE_4K_MASK);
        return;
    }
    for (int i = 0; i < 512; i++) {
        page_table_entry_t *entry = &table->entries[i];
        if (entry->value == 0 || is_huge_page(entry)) continue;
        if (level == 1) {
            if (entry->value & PTE_PRESENT && entry->value & PTE_WRITEABLE && entry->value & PTE_USER) {
                free_frame(entry->value & PAGE_4K_MASK);
            }
        } else {
            free_page_table_recursive(phys_to_virt(entry->value & PAGE_4K_MASK), level - 1);
        }
    }
    free_frame(physical_address & PAGE_4K_MASK);
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

/* Maps a virtual address to a physical frame using 4KB pages */
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

    l1_table->entries[l1_index].value = (frame & PAGE_4K_MASK) | flags;
    flush_tlb(addr);
}

/* Maps a virtual address to a physical frame using 2MB huge pages */
void page_map_to_2M(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
    uint64_t l4_index = (addr >> 39) & 0x1FF;
    uint64_t l3_index = (addr >> 30) & 0x1FF;
    uint64_t l2_index = (addr >> 21) & 0x1FF;

    page_table_t *l4_table = directory->table;
    page_table_t *l3_table = page_table_create(&l4_table->entries[l4_index]);
    page_table_t *l2_table = page_table_create(&l3_table->entries[l3_index]);

    l2_table->entries[l2_index].value = (frame & PAGE_2M_MASK) | flags | PTE_HUGE;
    flush_tlb(addr);
}

/* Maps a virtual address to a physical frame using 1GB huge pages */
void page_map_to_1G(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
    uint64_t l4_index = (addr >> 39) & 0x1FF;
    uint64_t l3_index = (addr >> 30) & 0x1FF;

    page_table_t *l4_table = directory->table;
    page_table_t *l3_table = page_table_create(&l4_table->entries[l4_index]);

    l3_table->entries[l3_index].value = (frame & PAGE_1G_MASK) | flags | PTE_HUGE;
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
void page_map_range(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t length, uint64_t flags)
{
    for (uint64_t i = 0; i < length; i += 0x1000) page_map_to(directory, (uint64_t)addr + i, frame + i, flags);
}

/* Maps a contiguous physical memory range to virtual memory */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags)
{
    for (uint64_t i = 0; i < length; i += 0x1000) page_map_to(directory, (uint64_t)phys_to_virt(frame + i), frame + i, flags);
}

/* Maps random non-contiguous physical pages to the virtual address range using 4K page */
void page_map_range_to_random_4K(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    uint64_t frame = 0;
    for (uint64_t i = 0; i < length; i += PAGE_4K_SIZE) {
        frame = alloc_frames(1);
        if (frame) page_map_to(directory, addr + i, frame, flags);
    }
}

/* Maps random non-contiguous physical pages to the virtual address range using 2M page */
void page_map_range_to_random_2M(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    /* Check align */
    uint64_t aligned_addr   = ALIGN_DOWN(addr, PAGE_2M_SIZE);
    uint64_t end_addr       = ALIGN_UP(addr + length, PAGE_2M_SIZE);
    uint64_t aligned_length = end_addr - aligned_addr;
    uint64_t blocks         = aligned_length / PAGE_2M_SIZE;

    for (uint64_t i = 0; i < blocks; i++) {
        uint64_t block_addr = aligned_addr + i * PAGE_2M_SIZE;

        /* Try 2M */
        uint64_t frame_2m = alloc_frames_2M(1);
        if (frame_2m) {
            page_map_to_2M(directory, block_addr, frame_2m, flags);
        } else {
            /* Fallback to 4K */
            page_map_range_to_random_4K(directory, block_addr, PAGE_2M_SIZE, flags);
        }
    }
}

/* Maps random non-contiguous physical pages to the virtual address range using 1G page */
void page_map_range_to_random_1G(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    /* Check align */
    uint64_t aligned_addr   = ALIGN_DOWN(addr, PAGE_1G_SIZE);
    uint64_t end_addr       = ALIGN_UP(addr + length, PAGE_1G_SIZE);
    uint64_t aligned_length = end_addr - aligned_addr;
    uint64_t blocks         = aligned_length / PAGE_1G_SIZE;

    for (uint64_t i = 0; i < blocks; i++) {
        uint64_t block_addr = aligned_addr + i * PAGE_1G_SIZE;

        /* Try 1G */
        uint64_t frame_1g = alloc_frames_1G(1);
        if (frame_1g) {
            page_map_to_1G(directory, block_addr, frame_1g, flags);
        } else {
            /* Fallback to 2M */
            page_map_range_to_random_2M(directory, block_addr, PAGE_1G_SIZE, flags);
        }
    }
}

/* Helper function to map unaligned regions using 2M and 4K pages */
static void map_unaligned_region(page_directory_t *directory, uint64_t start_addr, uint64_t end_addr, uint64_t flags)
{
    /* Try 2M pages for aligned sub-regions */
    const uint64_t aligned_2m_start = ALIGN_UP(start_addr, PAGE_2M_SIZE);
    const uint64_t aligned_2m_end   = ALIGN_DOWN(end_addr, PAGE_2M_SIZE);

    /* Map aligned middle region with 2M pages */
    if (aligned_2m_start < aligned_2m_end) {
        page_map_range_to_random_2M(directory, aligned_2m_start, aligned_2m_end - aligned_2m_start, flags);
    }

    /* Map leading unaligned region with 4K pages */
    if (start_addr < aligned_2m_start) {
        const uint64_t lead_end = MIN(aligned_2m_start, end_addr);
        page_map_range_to_random_4K(directory, start_addr, lead_end - start_addr, flags);
    }

    /* Map trailing unaligned region with 4K pages */
    if (aligned_2m_end < end_addr) page_map_range_to_random_4K(directory, aligned_2m_end, end_addr - aligned_2m_end, flags);
}

/* Intelligently maps random non-contiguous physical pages to the virtual address range */
void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    const uint64_t start_addr = addr;
    const uint64_t end_addr   = addr + length;

    /* Try to map 1G-aligned regions with 1G pages */
    const uint64_t aligned_1g_start = ALIGN_UP(start_addr, PAGE_1G_SIZE);
    const uint64_t aligned_1g_end   = ALIGN_DOWN(end_addr, PAGE_1G_SIZE);

    if (aligned_1g_start < aligned_1g_end) {
        /* We have a fully 1G-aligned region in the middle */
        page_map_range_to_random_1G(directory, aligned_1g_start, aligned_1g_end - aligned_1g_start, flags);
    }

    /* Process remaining regions with 2M and 4K pages */
    uint64_t current_addr = start_addr;

    /* Handle unaligned region before 1G section (if any) */
    if (current_addr < aligned_1g_start) {
        const uint64_t chunk_end = MIN(aligned_1g_start, end_addr);
        map_unaligned_region(directory, current_addr, chunk_end, flags);
        current_addr = chunk_end;
    }

    /* Handle unaligned region after 1G section (if any) */
    if (current_addr < end_addr) map_unaligned_region(directory, current_addr, end_addr, flags);
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
