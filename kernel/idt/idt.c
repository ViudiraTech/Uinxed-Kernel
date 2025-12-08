/*
 *
 *      idt.c
 *      Interrupt descriptor
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <interrupt.h>
#include <printk.h>
#include <stdint.h>

idt_register_t idt_pointer;
idt_entry_t    idt_entries[256];

/* Initialize the interrupt descriptor table */
void init_idt(void)
{
    idt_pointer.size = (uint16_t)sizeof(idt_entries) - 1;
    idt_pointer.ptr  = &idt_entries;

    __asm__ volatile("lidt %0" ::"m"(idt_pointer) : "memory");
    plogk("idt: IDT initialized at %p (limit = 0x%04x)\n", idt_entries, idt_pointer.size);
    plogk("idt: Loaded IDTR with base = %p, limit = %hu\n", idt_pointer.ptr, idt_pointer.size + 1);

    for (int i = 0; i < 256; i++) register_interrupt_handler(i, (void *)empty_handle[i], 0, 0x8e);
    plogk("idt: Empty handler functions for interrupt vectors 0-255 registered.\n");
}

/* Register an interrupt handler */
void register_interrupt_handler(uint16_t vector, void *handler, uint8_t ist, uint8_t flags)
{
    uint64_t addr                  = (uint64_t)handler;
    idt_entries[vector].offset_low = (uint16_t)addr;
    idt_entries[vector].ist        = ist;
    idt_entries[vector].flags      = flags;
    idt_entries[vector].selector   = 0x08;
    idt_entries[vector].offset_mid = (uint16_t)(addr >> 16);
    idt_entries[vector].offset_hi  = (uint32_t)(addr >> 32);
}
