#ifndef INCLUDE_BOOT_H_
#define INCLUDE_BOOT_H_

#include "types.h"

#define KERNEL_LMA_BASE 0x100000 // 必须对齐到4K
#define VMA2LMA(x) ((((uintptr_t)(x))-((uintptr_t)__kernel_start))+KERNEL_LMA_BASE)

/*
 * 开分页前的内存布局，地址和kernel.ld里的LMA对应
 * 先假设高地址只有一段，这样只用mem_upper就够了。未来需要能处理mmap_addr
 *  +-----------+ <- 1M + mem_upper * 1K
 *  |           |
 *  ~           ~
 *  ~           ~
 *  |           |
 *  +-----------+
 *  |  kernel   |
 *  +-----------+ <- 1M (KERNEL_LMA_BASE)
 *  | BIOS used |
 *  +-----------+ <- mem_lower * 1K, BIOS的EBDA起点在这里
 *  |           |
 *  ~           ~
 *  ~           ~
 *  |           |
 *  +-----------+ <- 0
 *
 * 开分页后，把[0,2G)的地址空间划分给用户态，把[2G,4G)的地址空间划分给内核态
 * 内核态最终的内存布局
 * +--------------+ <- 4G
 * |  page table  |
 * +--------------+ <- 4G - 8M
 * |    stack     |
 * +--------------+ <- KERNEL_STACK_BASE (初始program_break_end)
 * | frame buffer | 需要全局映射的一些页
 * |              | 从上往下
 * ~              ~ <- program_break_end
 *
 * ~              ~ <- program_break
*  |              | ^ 从下往上
 * |     heap     | 内核堆
 * +--------------+ <- 初始 program_break
 * |    kernel    |
 * +--------------+ <- __kernel_start (2G)
 *
 * ELF文件
 * +------------------+ <- __kernel_end (LMA)
 * |    page table    | 不占用从2G开始连续的地址空间，而是在最高处
 * |    boot stack    | 特别注意：这一段 VMA=LMA，应当仅限boot.c能操作
 * +------------------+ <- __frame_infos_end (VMA)
 * |                  |  这一段是可以回收的物理页以及VMA地址空间
 * |                  |  内存少的时候，这有很大一段可以回收
 * |                  | <- 初始 program_break
 * ~                  ~
 * ~                  ~
 * |                  | 用frame_infos来管理物理内存的分配
 * |    frame infos   | 不预留而是运行起来动态分配就套娃了
 * +------------------+
 * | .text .data .bss |
 * +------------------+ <- __kernel_start (VMA)
 */

extern char __kernel_start[];
extern char __frame_infos_end[];

#define BOOT_CMDLINE_SIZE 4096

struct memory_region {
	uintptr_t start;
	uintptr_t end;
};

struct boot_info {
	uint32_t mem_lower;
	uint32_t mem_upper;
	const char *cmdline;
	uint8_t framebuffer_type;
	uintptr_t framebuffer_addr;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint8_t framebuffer_bpp;
	struct memory_region initrd;
	struct memory_region symtab;
	struct memory_region strtab;
	struct memory_region acpi;
	uintptr_t rsdt;
};

extern struct boot_info boot_info;

extern uint32_t kernel_directory[1024] __attribute__((aligned(4096)));

#endif // INCLUDE_BOOT_H_
