/* 
 * kernel.ld -- 针对 kernel 格式所写的链接脚本（基于 GPL-3.0 开源协议）
 * Copyright © 2020 ViudiraTech，保留所有权利。
 * 源于 小严awa 撰写于 2024-6-23.
 */

ENTRY(start)
SECTIONS
{
	/* 段起始位置 */
	. = 0x100000;

	.text :
	{
		*(.text)
		. = ALIGN(4096);
	}

	.data :
	{
		*(.data)
		*(.rodata)
		. = ALIGN(4096);
	}

	.bss :
	{
		*(.bss)
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
