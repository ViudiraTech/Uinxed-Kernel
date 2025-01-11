/*
 *
 *		kernel.ld
 *		针对 kernel 格式所写的链接脚本
 *
 *		2024/6/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

ENTRY(start)
SECTIONS
{
	/* 段起始位置 */
	. = 0x80000000;
	__kernel_start = .;
	.text : AT(0x100000) /* boot.h KERNEL_LMA_BASE */
	{
		*(.multiboot)
		*(.text .text.*)
		. = ALIGN(4096); /* 没有这个会报 LOAD segment with RWX permissions */
	}
	.rodata :
	{
		*(.rodata .rodata.*)
		. = ALIGN(4096); /* 未来完善了，可以在页表标记为只读 */
	}
	.data :
	{
		*(.data .data.*)
		*(.bss .bss.*)
	}
	.frame_infos : /* 确保预留的空间在连续区域的最后 */
	{
		__frame_info_before = .;
		. += 8;
		frame_infos = .;
		/* 最多预留4G/4K*8 = 8M，前后各放一个额外的 */
		. += 0x800000;
		__frame_info_after = .;
		. += 8;
		. = ALIGN(4096);
	}
	__frame_infos_end = .;

	/* 强制设置VMA和LMA */
	.reserved 0x100000+(__frame_infos_end-__kernel_start) : AT(0x100000 + (__frame_infos_end-__kernel_start))
	{
		/* 预留boot时内核用的栈，需要和KERNEL_STACK_SIZE保持一致 */
		. = ALIGN(4096);
		. += 32768;
		boot_stack_top = .;
		/* 页表需要4K对齐 */
		. = ALIGN(4096);
		boot_page_tables = .;
		/* 只需要预留内核态的页表 512*4K = 2M */
		. += 0x200000;
	}
	. = ALIGN(4096);
	__kernel_end = .;
	/DISCARD/ : {
		*(.comment);
		*(.eh_frame);
		*(.note.*);
	}
}
