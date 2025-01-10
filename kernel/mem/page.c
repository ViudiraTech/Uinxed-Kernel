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
#include "string.h"
#include "printk.h"
#include "idt.h"
#include "common.h"
#include "debug.h"
#include "fifo.h"
#include "sched.h"

page_directory_t *kernel_directory	= 0;	// 内核用页目录
page_directory_t *current_directory	= 0;	// 当前页目录

uint32_t *frames;
uint32_t nframes;

struct FIFO8 *fifo;
uint32_t kh_usage_memory_byte = 0;

/* 标记一个帧为已使用 */
static void set_frame(uint32_t frame_addr)
{
	uint32_t frame = frame_addr / PAGE_SIZE;
	uint32_t idx = INDEX_FROM_BIT(frame);
	uint32_t off = OFFSET_FROM_BIT(frame);
	frames[idx] |= (0x1U << off);
}

/* 将一个帧标记为未使用状态 */
static void clear_frame(uint32_t frame_addr)
{
	uint32_t frame = frame_addr / PAGE_SIZE;
	uint32_t idx = INDEX_FROM_BIT(frame);
	uint32_t off = OFFSET_FROM_BIT(frame);
	frames[idx] &= ~(0x1U << off);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

/* 将帧位图中对应帧的位清除为0 */
static uint32_t test_frame(uint32_t frame_addr)
{
	uint32_t frame = frame_addr / PAGE_SIZE;
	uint32_t idx = INDEX_FROM_BIT(frame);
	uint32_t off = OFFSET_FROM_BIT(frame);
	return (frames[idx] & (0x1U << off));
}

#pragma GCC diagnostic pop

/* 清除帧位图中的特定位 */
uint32_t first_frame(void)
{
	for (size_t i = 0; i < INDEX_FROM_BIT(0xFFFFFFFF / PAGE_SIZE); i++) {
		if (frames[i] != 0xffffffff) {
			for (int j = 0; j < 32; j++) {
				uint32_t toTest = 0x1U << j;
				if (!(frames[i] & toTest)) {
					return i * 4 * 8 + j;
				}
			}
		}
	}
	return (uint32_t) - 1;
}

/* 分配一个帧给一个页表项 */
void alloc_frame(page_t *page, int is_kernel, int is_writable)
{
	if (page->present) {
		return;
	}
	else {
		uint32_t idx = first_frame();
		if (idx == (uint32_t) - 1) {
			panic(P002);
		}
		set_frame(idx * PAGE_SIZE);
		memset(page,0,4);
		page->present = 1;				// 现在这个页存在了
		page->rw = is_writable ? 1 : 0;	// 是否可写由is_writable决定
		page->user = is_kernel ? 0 : 1;	// 是否为用户态由is_kernel决定
		page->frame = idx;
	}
}

/* 手动分配特定帧给页表项 */
void alloc_frame_line(page_t *page, uint32_t line,int is_kernel, int is_writable)
{
	set_frame(line);
	memset(page, 0, 4);

	page->present = 1;				// 现在这个页存在了
	page->rw = is_writable ? 1 : 0;	// 是否可写由is_writable决定
	page->user = is_kernel ? 0 : 1;	// 是否为用户态由is_kernel决定
	page->frame = line / PAGE_SIZE;
}

/* 释放页表项所占用的帧 */
void free_frame(page_t *page)
{
	uint32_t frame = page->frame;
	if (!frame) return;
	else {
		page->present = 0;
		clear_frame(frame);
		page->frame = 0x0;
	}
}

/* 切换当前进程的页目录 */
void switch_page_directory(page_directory_t *dir)
{
	current_directory = dir;
	__asm__ __volatile__("mov %0, %%cr3" : : "r"(&dir->table_phy));
}

/* 获取给定虚拟地址对应的页表项 */
page_t *get_page(uint32_t address, int make, page_directory_t *dir)
{
	address /= PAGE_SIZE;
	uint32_t table_idx = address / 1024;

	if (dir->tables[table_idx]){
		page_t *pgg = &dir->tables[table_idx]->pages[address % 1024];
		return pgg;
	}else if (make) {
		uint32_t tmp;
		tmp = (uint32_t )(dir->tables[table_idx] = (page_table_t*)kmalloc(sizeof(page_table_t)));
		memset((void *)dir->tables[table_idx], 0, PAGE_SIZE);
		dir->table_phy[table_idx] = tmp | 0x7;
		page_t *pgg = &dir->tables[table_idx]->pages[address % 1024];
		return pgg;
	} else return 0;
}

/* 将页目录放入FIFO中 */
void put_directory(page_directory_t *dir)
{
	fifo8_put(fifo, (uint8_t)(intptr_t)dir);
}

/* 释放一个页目录 */
void free_pages(void)
{
	disable_scheduler();
	int ii;
	do {
		ii = fifo8_get(fifo);
		if (ii == -1 || ii == 0) {
			break;
		}
		page_directory_t *dir = (page_directory_t *)ii;
		for (int i = 0; i < 1024; i++) {
			page_table_t *table = dir->tables[i];
			if (table == NULL) continue;
			for (int j = 0; j < 1024; j++) {
				page_t page = table->pages[i];
				free_frame(&page);
			}
			kfree(table);
		}
		kfree(dir);
	} while (1);
	enable_scheduler();
}

/* 初始化一个用于存储空闲页目录的FIFO */
void setup_free_page(void)
{
	fifo = kmalloc(sizeof(struct FIFO));
	uint8_t *buf = kmalloc(sizeof(uint32_t) * MAX_FREE_QUEUE);
	fifo8_init(fifo,sizeof(uint32_t) * MAX_FREE_QUEUE, buf);
}

/* 返回内核使用的内存量 */
uint32_t get_kernel_memory_usage(void)
{
	return kh_usage_memory_byte;
}

/* 页错误处理 */
static void page_fault(pt_regs *regs)
{
	disable_intr();
	uint32_t faulting_address;
	__asm__ __volatile__("mov %%cr2, %0" : "=r" (faulting_address));

	char s[50];
	int present = !(regs->err_code & 0x1);		// 页不存在
	int rw = regs->err_code & 0x2;				// 只读页被写入
	int us = regs->err_code & 0x4;				// 用户态写入内核页
	int reserved = regs->err_code & 0x8;		// 写入CPU保留位
	int id = regs->err_code & 0x10;				// 由取指引起

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

/* 克隆一个页面表 */
static page_table_t *clone_table(page_table_t *src, uint32_t *physAddr)
{
	page_table_t *table = (page_table_t *)kmalloc(sizeof(page_table_t));
	*physAddr = (uint32_t )table;
	memset(table, 0, sizeof(page_directory_t));

	int i;
	for (i = 0; i < 1024; i++) {
		if (!src->pages[i].frame)
			continue;
		alloc_frame(&table->pages[i], 0, 0);
		if (src->pages[i].present) table->pages[i].present = 1;
		if (src->pages[i].rw) table->pages[i].rw = 1;
		if (src->pages[i].user) table->pages[i].user = 1;
		if (src->pages[i].accessed)table->pages[i].accessed = 1;
		if (src->pages[i].dirty) table->pages[i].dirty = 1;
		copy_page_physical(src->pages[i].frame * 0x1000, table->pages[i].frame * 0x1000);
	}
	return table;
}

/* 克隆页目录 */
page_directory_t *clone_directory(page_directory_t *src)
{
	page_directory_t *dir = (page_directory_t *)kmalloc(sizeof(page_directory_t));
	memset(dir, 0, sizeof(page_directory_t));

	int i;
	for (i = 0; i < 1024; i++) {
		if (!src->tables[i])
			continue;
		if (kernel_directory->tables[i] == src->tables[i]) {
			dir->tables[i] = src->tables[i];
			dir->table_phy[i] = src->table_phy[i];
		} else {
			uint32_t phys;
			dir->tables[i] = clone_table((page_table_t *)src->tables[i], &phys);
			dir->table_phy[i] = phys | 0x07;
		}
	}
	return dir;
}

/* 打开分页机制 */
static void open_page(void)
{
	uint32_t cr0;
	__asm__ __volatile__("mov %%cr0, %0" : "=b"(cr0));
	cr0 |= 0x80000000;
	__asm__ __volatile__("mov %0, %%cr0" : : "b"(cr0));
}

/* 初始化内存分页 */
void init_page(multiboot_t *multiboot)
{
	print_busy("Initializing memory paging...\r"); // 提示用户正在初始化内存分页，并回到行首等待覆盖

	uint32_t mem_end_page = 0xFFFFFFFF; // 4GB Page
	nframes = mem_end_page / PAGE_SIZE;

	frames = (uint32_t *)kmalloc(INDEX_FROM_BIT(nframes));
	memset(frames, 0, INDEX_FROM_BIT(nframes));

	kernel_directory = (page_directory_t *)kmalloc(sizeof(page_directory_t));
	memset(kernel_directory, 0, sizeof(page_directory_t));

	int i = 0;
	while (i < (int)program_break_end) {
		/*
         * 内核部分对ring3而言不可读不可写
         * 无偏移页表映射
         * 因为刚开始分配, 所以内核线性地址与物理地址对应
         */
		page_t *p = get_page(i, 1, kernel_directory);
		alloc_frame(p, 1, 1);
		i += PAGE_SIZE;
	}
	uint32_t j = multiboot->framebuffer_addr,
                 size = multiboot->framebuffer_height * multiboot->framebuffer_width*multiboot->framebuffer_bpp;
	while (j <= multiboot->framebuffer_addr + size) { // VBE显存缓冲区映射
		alloc_frame_line(get_page(j,1,kernel_directory),j,0,1);
		j += 0x1000;
	}
	register_interrupt_handler(0xe, &page_fault);
	switch_page_directory(kernel_directory);
	open_page();
	print_succ("Memory paging initialized successfully | Memory size: "); // 提示用户已经完成初始化内存分页
	printk("%dMiB\n", (glb_mboot_ptr->mem_upper + glb_mboot_ptr->mem_lower) / 1024 + 1);
}
