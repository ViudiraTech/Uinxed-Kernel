/*
 *
 *      idt.c
 *      Interrupt Descriptor
 *
 *      2024/6/27 By Rainy101112
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "idt.h"
#include "interrupt.h"
#include "printk.h"
#include "stdint.h"

struct idt_register idt_pointer;
struct idt_entry idt_entries[256];

/* Initialize the interrupt descriptor table */
void init_idt(void)
{
    idt_pointer.size = (uint16_t)sizeof(idt_entries) - 1;
    idt_pointer.ptr  = &idt_entries;

    __asm__ volatile("lidt %0" ::"m"(idt_pointer) : "memory");
    plogk("IDT: IDT initialized at 0x%016x (limit = 0x%04x)\n", idt_entries, idt_pointer.size);
    plogk("IDT: Loaded IDTR with base = 0x%016x, limit = %d\n", idt_pointer.ptr, idt_pointer.size + 1);

    for (int i = 0; i < 256; i++) register_interrupt_handler(i, empty_handle[i], 0, 0x8e);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)

/* Register an interrupt handler */
void register_interrupt_handler(uint16_t vector, void *handler, uint8_t ist, uint8_t flags)
{
    // NOLINTEND(bugprone-easily-swappable-parameters)
    uint64_t addr                  = (uint64_t)handler;
    idt_entries[vector].offset_low = (uint16_t)addr;
    idt_entries[vector].ist        = ist;
    idt_entries[vector].flags      = flags;
    idt_entries[vector].selector   = 0x08;
    idt_entries[vector].offset_mid = (uint16_t)(addr >> 16);
    idt_entries[vector].offset_hi  = (uint32_t)(addr >> 32);
}
