/*
 *
 *		linker.ld
 *		针对内核格式所写的链接脚本
 *
 *		2024/6/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

PHDRS
{
	limine_requests	PT_LOAD;
	text			PT_LOAD;
	rodata			PT_LOAD;
	data			PT_LOAD;
}

SECTIONS
{
	. = 0xffffffff80000000;
	.limine_requests : {
		KEEP(*(.limine_requests_start))
		KEEP(*(.limine_requests))
		KEEP(*(.limine_requests_end))
	} :limine_requests

	. = ALIGN(CONSTANT(MAXPAGESIZE));
	.text : {
		*(.text .text.*)
	} :text

	. = ALIGN(CONSTANT(MAXPAGESIZE));
	.rodata : {
		*(.rodata .rodata.*)
	} :rodata

	. = ALIGN(CONSTANT(MAXPAGESIZE));
	.data : {
		*(.data .data.*)
	} :data

	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	} :data

	/DISCARD/ : {
		*(.eh_frame*)
		*(.note .note.*)
	}
}
