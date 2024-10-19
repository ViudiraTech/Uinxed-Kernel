/*
 *
 *		kheap.c
 *		内核内存分配器
 *
 *		2024/6/30 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "memory.h"
#include "printk.h"

header_t *head = NULL, *tail = NULL; // 内存块链表
extern page_directory_t *current_directory;
extern uint32_t end;
uint32_t placement_address = (uint32_t) & end;
void *program_break, *program_break_end;

/* 开启分页机制后的内核栈 */
char kern_stack[STACK_SIZE] __attribute__ ((aligned(16)));

/* 栈顶 */
uint32_t kern_stack_top = ((uint32_t)kern_stack + STACK_SIZE);

/* 分配内存，同时返回物理地址 */
uint32_t kmalloc_i_ap(uint32_t size, uint32_t *phys)
{
	if ((placement_address & 0x00000FFF)) {
		placement_address &= 0xFFFFF000;
		placement_address += 0x1000;
	}
	if (phys) *phys = placement_address;
	uint32_t tmp = placement_address;
	placement_address += size;
	return tmp;
}

/* 分配内存并可选择对齐和返回物理地址 */
static uint32_t kmalloc_int(size_t sz, uint32_t align, uint32_t *phys)
{
	if (program_break) {
		/* 有内存堆 */
		void *addr = alloc(sz); // 直接malloc，align丢掉了
		if (phys) {
			/* 需要物理地址，先找到对应页 */
			page_t *page = get_page((uint32_t) addr, 0, current_directory);
			*phys = page->frame * 0x1000 + ((uint32_t) addr & 0x00000FFF);
		}
		return (uint32_t) addr;
	}
	if (align == 1 && (placement_address & 0x00000FFF)) {
		placement_address &= 0xFFFFF000;
		placement_address += 0x1000;
	}
	if (phys) *phys	= placement_address;
	uint32_t tmp	= placement_address;
	placement_address += sz;
	return tmp;
}

/* 分配对齐的内存 */
uint32_t kmalloc_a(uint32_t size)
{
	return kmalloc_int(size, 1, 0);
}

/* 分配内存并返回物理地址 */
uint32_t kmalloc_p(uint32_t size, uint32_t *phys)
{
	return kmalloc_int(size, 0, phys);
}

/* 分配对齐的内存并返回物理地址 */
uint32_t kmalloc_ap(uint32_t size, uint32_t *phys)
{
	return kmalloc_int(size, 1, phys);
}

/* 分配内存，不返回物理地址 */
uint32_t kmalloc(uint32_t size)
{
	return kmalloc_int(size, 0, 0);
}

/* 改变进程的堆栈大小 */
void *ksbrk(int incr)
{
	if (program_break == 0 || program_break + incr >= program_break_end) return (void *) -1;

	void *prev_break = program_break;
	program_break += incr;
	return prev_break;
}

/* 寻找一个符合条件的指定大小的空闲内存块 */
static header_t *get_free_block(size_t size)
{
	header_t *curr = head;
	while (curr) {
		if (curr->s.is_free && curr->s.size >= size) return curr;
		curr = curr->s.next;
	}
	return NULL;
}

/* 尝试在现有的内存块中找到足够大的空闲块 */
void *alloc(size_t size)
{
	uint32_t total_size;
	void *block;
	header_t *header;
	if (!size) return NULL;
	header = get_free_block(size);
	if (header) {
		header->s.is_free = 0;
		return (void *) (header + 1);
	}
	total_size = sizeof(header_t) + size;
	block = ksbrk(total_size);
	if (block == (void *) -1) return NULL;
	header = block;
	header->s.size = size;
	header->s.is_free = 0;
	header->s.next = NULL;
	if (!head)	head = header;
	if (tail)	tail->s.next = header;
	tail = header;
	return (void *) (header + 1);
}

/* 将内存块标记为空闲并尝试将其与相邻的空闲块合并 */
void kfree(void *block)
{
	header_t *header, *tmp;
	if (!block) return;
	header = (header_t *) block - 1;
	if ((char *) block + header->s.size == program_break) {
		if (head == tail) head = tail = NULL;
		else {
			tmp = head;
			while (tmp) {
				if (tmp->s.next == tail) {
					tmp->s.next = NULL;
					tail = tmp;
				}
				tmp = tmp->s.next;
			}
		}
		ksbrk(0 - sizeof(header_t) - header->s.size);
		return;
	}
	header->s.is_free = 1;
}
void *krealloc(void *block, size_t size) {
  header_t *header, *tmp;
  if (!block)
    return;
  header = (header_t *)block - 1;
  void *new_block = (void *)kmalloc(size);
  if (!new_block)
    return NULL;
  memcpy(new_block, block, header->s.size);
  kfree(block);
  return new_block;
}