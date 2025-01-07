/*
 *
 *		page.c
 *		内核内存页操作
 *
 *		2024/12/7 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "memory.h"
#include "boot.h"
#include "string.h"
#include "printk.h"
#include "idt.h"
#include "common.h"
#include "debug.h"
#include "fifo.h"
#include "sched.h"

uintptr_t align_down(uintptr_t addr, uintptr_t size) {
	return addr-addr%size;
}

uintptr_t align_up(uintptr_t addr, uintptr_t size) {
	return align_down(addr+size-1, size);
}

uint32_t kh_usage_memory_byte = 0;

void page_map(uintptr_t vaddr, uintptr_t entry)
{
	uintptr_t index = vaddr >> HUGE_PAGE_SHIFT;
	uintptr_t *page_directory = (uintptr_t *)CURRENT_PD_BASE;
	uintptr_t *page_table = (uintptr_t *)CURRENT_PT_BASE;

	if (!(page_directory[index] & PT_P)) {
		uintptr_t flags = entry & (PT_W|PT_U);
		uintptr_t paddr = frame_alloc(1);
		if (!(paddr & FRAMEINFO_NONNULL))
			panic(P002);
		page_directory[index] = PT_ADDRESS(paddr)|PT_P|flags;
		uintptr_t base = index << 10;
		for (uintptr_t i=base; i<base+1024; ++i)
			page_table[i] = 0;
		}
	if (!(page_table[vaddr>>PAGE_SHIFT] & PT_P))
		page_table[vaddr>>PAGE_SHIFT] = entry|PT_P;
}

void page_alloc(uintptr_t vaddr, uintptr_t flags)
{
	uintptr_t paddr = frame_alloc(1);
	if (!(paddr & FRAMEINFO_NONNULL))
		panic(P001);
	page_map(vaddr, PT_ADDRESS(paddr)|flags);
}

void *page_map_kernel_range(uintptr_t start, uintptr_t end, uintptr_t flags) {
	uintptr_t aligned_start = align_down(start, PAGE_SIZE);
	uintptr_t aligned_end = align_up(end, PAGE_SIZE);
	uintptr_t size = aligned_end-aligned_start;
	if (program_break + size > program_break_end)
		panic(P001);

	program_break_end -= size;

	for (uintptr_t offset=0; offset<size; offset+=PAGE_SIZE)
		page_map(program_break_end+offset, (aligned_start+offset)|flags);

	return (void *)(program_break_end + (start - aligned_start));
}

static void scratch_page_map(uintptr_t vaddr, uintptr_t entry)
{
	uintptr_t index = vaddr >> HUGE_PAGE_SHIFT;
	uintptr_t *page_directory = (uintptr_t *)SCRATCH_PD_BASE;
	uintptr_t *page_table = (uintptr_t *)SCRATCH_PT_BASE;

	if (!(page_directory[index] & PT_P)) {
		uintptr_t flags = entry & (PT_W|PT_U);
		uintptr_t paddr = frame_alloc(1);
		if (!(paddr & FRAMEINFO_NONNULL))
			panic(P002);
		page_directory[index] = PT_ADDRESS(paddr)|PT_P|flags;
		uintptr_t base = index << 10;
		for (uintptr_t i=base; i<base+1024; ++i)
			page_table[i] = 0;
		}
	if (!(page_table[vaddr>>PAGE_SHIFT] & PT_P))
		page_table[vaddr>>PAGE_SHIFT] = entry|PT_P;
}

static void scratch_page_alloc(uintptr_t vaddr, uintptr_t flags)
{
	uintptr_t paddr = frame_alloc(1);
	if (!(paddr & FRAMEINFO_NONNULL))
		panic(P001);
	scratch_page_map(vaddr, PT_ADDRESS(paddr)|flags);
}

/* 返回内核使用的内存量 */
uint32_t get_kernel_memory_usage(void)
{
	return kh_usage_memory_byte;
}

/* 页错误处理 */
__attribute__((interrupt))
void page_fault(struct interrupt_frame *frame, uintptr_t error_code)
{
	uint32_t faulting_address;
	__asm__ __volatile__("mov %%cr2, %0" : "=r" (faulting_address));

	char s[50];
	int present = !(error_code & 0x1);		// 页不存在
	int rw = error_code & 0x2;				// 只读页被写入
	int us = error_code & 0x4;				// 用户态写入内核页
	int reserved = error_code & 0x8;		// 写入CPU保留位
	int id = error_code & 0x10;				// 由取指引起

	if (present) {
		sprintf(s, "%s 0x%08X", P003, faulting_address);
		panic(s);
	} else if (rw) {
		sprintf(s, "%s 0x%08X", P004, faulting_address);
		panic(s);
	} else if (us) {
		sprintf(s, "%s 0x%08X", P005, faulting_address);
		panic(s);
	} else if (reserved) {
		sprintf(s, "%s 0x%08X", P006, faulting_address);
		panic(s);
	} else if (id) {
		sprintf(s, "%s 0x%08X", P007, faulting_address);
		panic(s);
	}
	krn_halt();
}

/* 新建页目录 */
uintptr_t create_directory(void)
{
	uintptr_t paddr = frame_alloc(1);
	if (!(paddr & FRAMEINFO_NONNULL))
		panic(P001);

	uintptr_t *current_pd = (uintptr_t *)CURRENT_PD_BASE;
	current_pd[1022] = PT_ADDRESS(paddr)|PT_P|PT_W;

	/* 自指页表 */
	uintptr_t *scratch_pd = (uintptr_t *)SCRATCH_PD_BASE;
	scratch_pd[1023] = PT_ADDRESS(paddr)|PT_P|PT_W;

	uintptr_t kernel_base=(uintptr_t)__kernel_start;

	/* 所有page_directory共用的页表 */
	for (uintptr_t addr=kernel_base; addr<KERNEL_STACK_BASE; addr+=HUGE_PAGE_SIZE)
		scratch_pd[addr>>HUGE_PAGE_SHIFT] = current_pd[addr>>HUGE_PAGE_SHIFT];

	for (uintptr_t addr=KERNEL_STACK_TOP-KERNEL_STACK_SIZE; addr<KERNEL_STACK_TOP; addr+=PAGE_SIZE)
		scratch_page_alloc(addr, PT_W);

	return PT_ADDRESS(paddr);
}

/* 初始化内存分页 */
void init_page(void)
{
	init_frame();
	uintptr_t *page_table = (uintptr_t *)CURRENT_PT_BASE;

	/* 用户态地址空间 */
	for (uintptr_t addr=0; addr<(uintptr_t)__kernel_start; addr+=HUGE_PAGE_SIZE)
		kernel_directory[addr>>HUGE_PAGE_SHIFT] = 0;

	/* 预留而没用到的内核态地址空间 */
	for (uintptr_t addr=program_break; addr<KERNEL_STACK_BASE; addr+=PAGE_SIZE)
		page_table[addr >> PAGE_SHIFT] = 0;

	/* 直接清空TLB，免得一页页改了 */
	__asm__("mov %0, %%cr3" : : "r"(VMA2LMA(kernel_directory)));
}
