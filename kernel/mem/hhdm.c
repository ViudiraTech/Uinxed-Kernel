/*
 *
 *		hhdm.c
 *		高半区内存映射
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "limine.h"
#include "hhdm.h"
#include "printk.h"

__attribute__((used, section(".limine_requests")))
static __volatile__ struct limine_hhdm_request hhdm_request = {
	.id = LIMINE_HHDM_REQUEST,
	.revision = 0
};

uint64_t physical_memory_offset = 0;

/* 初始化高半区内存映射 */
void init_hhdm(void)
{
	physical_memory_offset = hhdm_request.response->offset;
	plogk("HHDM: Initialized high half direct map with offset: 0x%016x\n", physical_memory_offset);
}

/* 获取物理内存偏移量 */
uint64_t get_physical_memory_offset(void)\
{
	return physical_memory_offset;
}

/* 物理内存转虚拟内存 */
void *phys_to_virt(uint64_t phys_addr)
{
	return (void *)(phys_addr + physical_memory_offset);
}

/* 虚拟内存转物理内存 */
void *virt_to_phys(uint64_t virt_addr)
{
	return (void *)(virt_addr - physical_memory_offset);
}
