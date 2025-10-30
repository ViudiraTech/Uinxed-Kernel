/*
 *
 *      linker.ld
 *      Linker script written for kernel format
 *
 *      2024/6/23 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(executable_entry)

PHDRS
{
    limine_requests PT_LOAD    FLAGS(4);
    text            PT_LOAD    FLAGS(5);
    rodata          PT_LOAD    FLAGS(4);
    data            PT_LOAD    FLAGS(6);
    dynamic         PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
    . = ALIGN(CONSTANT(MAXPAGESIZE));
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
    .dynamic : {
        *(.dynamic)
    } :rodata :dynamic

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : {
        *(.data .data.*)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
