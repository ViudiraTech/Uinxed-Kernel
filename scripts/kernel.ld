/*
 *
 *		kernel.ld
 *		针对 kernel 格式所写的链接脚本
 *
 *		2024/6/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

ENTRY(start)
SECTIONS
{
	/* 段起始位置 */
	. = 0x100000;

	.text :
	{
		*(.multiboot)
		*(.text .text.*)
		. = ALIGN(4096);
	}
	.data :
	{
		*(.data .data.*)
		*(.rodata .rodata.*)
		. = ALIGN(4096);
	}
	.bss :
	{
		*(.bss .bss.*)
		. = ALIGN(4096);
	}
	.stab :
	{
		*(.stab)
		. = ALIGN(4096);
	}
	.stabstr :
	{
		*(.stabstr)
		. = ALIGN(4096);
	}
	/DISCARD/ : { *(.comment) *(.eh_frame) }
	end = .; _end = .; __end = .;
}
