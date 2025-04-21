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
#include "limine.h"
#include "printk.h"

__attribute__((used, section(".limine_requests"))) static volatile struct limine_hhdm_request hhdm_request = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

uint64_t physical_memory_offset = 0;

/* Initialize the upper half memory mapping */
void init_hhdm(void)
{
    physical_memory_offset = hhdm_request.response->offset;
}

/* Get physical memory offset */
uint64_t get_physical_memory_offset(void)
{
    return physical_memory_offset;
}

/* Convert physical memory to virtual memory */
void *phys_to_virt(uint64_t phys_addr)
{
    return (void *)(phys_addr + physical_memory_offset);
}

/* Convert virtual memory to physical memory */
void *virt_to_phys(uint64_t virt_addr)
{
    return (void *)(virt_addr - physical_memory_offset);
}
