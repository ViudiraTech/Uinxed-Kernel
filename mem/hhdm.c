/*
 *
 *      hhdm.c
 *      Upper half memory map
 *
 *      2025/2/16 By XIAOYI12
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cpuid.h>
#include <hhdm.h>
#include <limine.h>
#include <page.h>
#include <page_walker.h>
#include <printk.h>
#include <uinxed.h>

/* Get physical memory offset */
uint64_t get_physical_memory_offset(void)
{
    return hhdm_request.response->offset;
}

/* Convert physical memory to HHDM virtual memory */
void *phys_to_virt(uint64_t phys_addr)
{
    pointer_cast_t virt_addr;
    if (phys_addr >= (1ULL << get_cpu_phys_bits())) { // Check if physical address is valid
        plogk("Warning: Physical address 0x%016llx exceeds physical address space\n", phys_addr);
    }
    virt_addr.val = phys_addr + hhdm_request.response->offset;
    return virt_addr.ptr;
}

/* Convert HHDM virtual memory to physical memory */
void *virt_to_phys(uint64_t virt_addr)
{
    pointer_cast_t phys_addr;
    if (virt_addr < hhdm_request.response->offset) { // Check if virtual address is in HHDM region
        plogk("Warning: Virtual address 0x%016llx is not in HHDM region.\n", virt_addr);
    }
    phys_addr.val = virt_addr - hhdm_request.response->offset;
    return phys_addr.ptr;
}

/* Convert any virtual memory to physical memory */
void *virt_any_to_phys(uint64_t addr)
{
    pointer_cast_t phys_addr;

    /* Try to walk the page tables first */
    phys_addr.val = walk_page_tables(get_kernel_pagedir(), addr);
    if (phys_addr.val) return phys_addr.ptr;

    /* May be in HHDM region */
    uint64_t hhdm_base = hhdm_request.response->offset;
    if (addr >= hhdm_base) {
        phys_addr.val = addr - hhdm_base;
        if (phys_addr.val < (1ULL << get_cpu_phys_bits())) return phys_addr.ptr; // Check if physical address is valid
    }

    /* Not mapped */
    plogk("Warning: Virtual address 0x%016llx is not mapped to any physical address.\n", addr);
    return 0;
}
