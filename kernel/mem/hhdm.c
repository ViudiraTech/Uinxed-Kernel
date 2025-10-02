/*
 *
 *      hhdm.c
 *      Upper half memory map
 *
 *      2025/2/16 By XIAOYI12
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "hhdm.h"
#include "cpuid.h"
#include "limine.h"
#include "page.h"
#include "printk.h"
#include "uinxed.h"

/* Get physical memory offset */
uint64_t get_physical_memory_offset(void)
{
    return hhdm_request.response->offset;
}

/* Convert physical memory to HHDM virtual memory */
void *phys_to_virt(uint64_t phys_addr)
{
    pointer_cast_t virt_addr;
    if (phys_addr & hhdm_request.response->offset) plogk("Unsafe! 0x%016llx in phys_to_virt.\n", phys_addr);

    /* Avoid overflow */
    virt_addr.val = phys_addr | hhdm_request.response->offset;
    return virt_addr.ptr;
}

/* Convert HHDM virtual memory to physical memory */
void *virt_to_phys(uint64_t virt_addr)
{
    pointer_cast_t phys_addr;
    if (!(virt_addr & hhdm_request.response->offset)) plogk("Unsafe! 0x%016llx in virt_to_phys.\n", virt_addr);

    /* Avoid overflow */
    phys_addr.val = virt_addr & ~(hhdm_request.response->offset);
    return phys_addr.ptr;
}

/* Convert any virtual memory to physical memory */
void *virt_any_to_phys(uint64_t addr)
{
    pointer_cast_t phys_addr;
    phys_addr.val = addr - hhdm_request.response->offset;
    if (phys_addr.val >> get_cpu_phys_bits()) {
        /* Non-HHDM virtual memory */
        phys_addr.val = walk_page_tables(get_kernel_pagedir(), addr);
        return phys_addr.ptr;
    } else {
        /* HHDM virtual memory */
        return phys_addr.ptr;
    }
}