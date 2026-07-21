/*
 *
 *      init.ld
 *      Linker script for userspace init program
 *
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text : {
        *(.text .text.*)
    }
    . = ALIGN(16);
    .rodata : {
        *(.rodata .rodata.*)
    }
    . = ALIGN(16);
    .data : {
        *(.data .data.*)
    }
    . = ALIGN(16);
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
}
