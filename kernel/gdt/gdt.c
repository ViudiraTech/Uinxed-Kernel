/*
 *
 *      gdt.c
 *      Global descriptor
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <gdt.h>
#include <printk.h>
#include <stdint.h>

/* Global Descriptor Table Definition */
gdt_t gdt0;

/* TSS */
tss_stack_t tss_stack;
tss_t       tss0;

/* Initialize the global descriptor table */
void init_gdt(void)
{
    gdt0.entries[0] = 0x0000000000000000; // NULL descriptor
    gdt0.entries[1] = 0x00a09a0000000000; // Kernel code segment
    gdt0.entries[2] = 0x00c0920000000000; // Kernel data segment
    gdt0.entries[3] = 0x00c0f20000000000; // User code segment
    gdt0.entries[4] = 0x00a0fa0000000000; // User data segment

    gdt0.pointer = ((gdt_register_t) {.size = (uint16_t)(sizeof(gdt_entries_t) - 1), .ptr = &gdt0.entries});

    __asm__ volatile("lgdt %[ptr]; push %[cseg]; lea 1f(%%rip), %%rax; push %%rax; lretq;"
                     "1:"
                     "mov %[dseg], %%ds;"
                     "mov %[dseg], %%fs;"
                     "mov %[dseg], %%gs;"
                     "mov %[dseg], %%es;"
                     "mov %[dseg], %%ss;" ::[ptr] "m"(gdt0.pointer),
                     [cseg] "rm"((uint64_t)0x8), [dseg] "rm"((uint64_t)0x10)
                     : "memory");

    plogk("gdt: CS reloaded with 0x%04x, DS/ES/FS/GS/SS = 0x%04x\n", 0x8, 0x10);
    plogk("gdt: GDT initialized at %p (6 entries)\n", &gdt0.entries);
    tss_init();
}

/* Initialize TSS */
void tss_init(void)
{
    uint64_t address     = ((uint64_t)(&tss0));
    uint64_t low_base    = (((address & 0xffffff)) << 16);
    uint64_t mid_base    = (((((address >> 24)) & 0xff)) << 56);
    uint64_t high_base   = (address >> 32);
    uint64_t access_byte = (((uint64_t)(0x89)) << 40);
    uint64_t limit       = (uint64_t)(sizeof(tss_t) - 1);

    gdt0.entries[5] = (((low_base | mid_base) | limit) | access_byte);
    gdt0.entries[6] = high_base;
    tss0.ist[0]     = ((uint64_t)&tss_stack) + sizeof(tss_stack_t);

    plogk("tss: TSS descriptor configured (address = %p, limit = 0x%04x)\n", &tss0, sizeof(tss_t) - 1);
    plogk("tss: IST0 stack = %p\n", tss0.ist[0]);
    __asm__ volatile("ltr %w[offset]" ::[offset] "rm"((uint16_t)0x28) : "memory");
    plogk("tss: TR register loaded with selector 0x%04x\n", 0x28);
}

/* Setting up the kernel stack */
void set_kernel_stack(uint64_t rsp)
{
    tss0.rsp[0] = rsp;
}
