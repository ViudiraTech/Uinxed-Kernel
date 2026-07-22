/*
 *
 *      tss.c
 *      Task state segment
 *
 *      2026/7/21 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/gdt.h>
#include <arch/tss.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>

/* Task state segment definition */
tss_stack_t tss_stack;
tss_t       tss0;

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
