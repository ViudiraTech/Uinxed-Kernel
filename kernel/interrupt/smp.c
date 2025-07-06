/*
 *
 *      smp.c
 *      Symmetric Multi-Processing (SMP) support
 *
 *      2025/7/6 By W9pi3cZ1
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "smp.h"
#include "apic.h"
#include "common.h"
#include "debug.h"
#include "gdt.h"
#include "idt.h"
#include "limine.h"
#include "page.h"
#include "printk.h"
#include "stddef.h"
#include "stdlib.h"
#include "string.h"
#include "uinxed.h"
#include <stdint.h>

static cpu_processor cpus[256];
static size_t cpu_count = 0;

void send_ipi_all(uint8_t vector)
{
    for (size_t i = 0; i < cpu_count; i++) {
        if (cpus[i].id != get_current_cpu_id()) { send_ipi(cpus[i].lapic_id, vector | IPI_FIXED | APIC_ICR_PHYSICAL); }
    }
}

void send_ipi_cpu(uint32_t cpu_id, uint8_t vector)
{
    if (cpu_id < cpu_count && cpu_id != get_current_cpu_id()) {
        send_ipi(cpus[cpu_id].lapic_id, vector | IPI_FIXED | APIC_ICR_PHYSICAL);
    }
}

void flush_tlb_all(void)
{
    send_ipi_all(IPI_TLB_SHOOTDOWN);
}

void flush_tlb_range(uint64_t start, uint64_t end)
{
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) { flush_tlb(addr); }
}

__attribute__((interrupt)) void ipi_reschedule_handler(interrupt_frame_t *frame)
{
    (void)frame;
    // TODO: Handle rescheduling
    send_eoi();
}

__attribute__((interrupt)) void ipi_halt_handler(interrupt_frame_t *frame)
{
    (void)frame;
    // TODO: Handle halting
    send_eoi();
}

__attribute__((interrupt)) void ipi_tlb_shootdown_handler(interrupt_frame_t *frame)
{
    (void)frame;
    // TODO: Handle TLB shootdown
    send_eoi();
}

__attribute__((interrupt)) void ipi_panic_handler(interrupt_frame_t *frame)
{
    (void)frame;
    // TODO: Handle panic
    send_eoi();
}

extern struct gdt_register gdt_pointer;
extern struct idt_register idt_pointer;
extern tss_t tss0;
extern tss_stack_t tss_stack;
extern gdt_entries_t gdt_entries;

static volatile uint64_t ap_ready_count = 0;
spinlock_t ap_start_lock                = {0};

void ap_entry(struct limine_smp_info *info)
{
    // Init for APs (But now it's looks so messy)
    // TODO: Refactor this code to be cleaner and more modular
    spin_lock(&ap_start_lock);
    cpu_processor *cpu  = (cpu_processor *)info->extra_argument;
    uint32_t logical_id = cpu->id;
    plogk("AP_entry(early): AP %d (LAPIC ID %d)\n", logical_id, info->lapic_id);

    __asm__ volatile("lgdt %[ptr]; push %[cseg]; lea 1f, %%rax; push %%rax; lretq;"
                     "1:"
                     "mov %[dseg], %%ds;"
                     "mov %[dseg], %%fs;"
                     "mov %[dseg], %%gs;"
                     "mov %[dseg], %%es;"
                     "mov %[dseg], %%ss;" ::[ptr] "m"(gdt_pointer),
                     [cseg] "rm"((uint64_t)0x8), [dseg] "rm"((uint64_t)0x10)
                     : "memory");

    uint64_t address     = ((uint64_t)(&tss0));
    uint64_t low_base    = (((address & 0xffffff)) << 16);
    uint64_t mid_base    = (((((address >> 24)) & 0xff)) << 56);
    uint64_t high_base   = (address >> 32);
    uint64_t access_byte = (((uint64_t)(0x89)) << 40);
    uint64_t limit       = (uint64_t)(sizeof(tss_t) - 1);

    gdt_entries[5] = (((low_base | mid_base) | limit) | access_byte);
    gdt_entries[6] = high_base;
    tss0.ist[0]    = ((uint64_t)&tss_stack) + sizeof(tss_stack_t);

    __asm__ volatile("ltr %w[offset]" ::[offset] "rm"((uint16_t)0x28) : "memory");

    __asm__ volatile("lidt %0" ::"m"(idt_pointer) : "memory");

    // Initialize the TSS stack
    uint64_t stack_top = ALIGN_DOWN(cpu->stack + 0x10000, 16);
    set_kernel_stack(stack_top);

    plogk("AP_entry: AP %d (LAPIC ID %d) is up\n", logical_id, info->lapic_id);

    uint32_t spurious = lapic_read(LAPIC_REG_SPURIOUS);
    lapic_write(LAPIC_REG_SPURIOUS, spurious | (1 << 8) | 0xFF);

    plogk("AP %d LAPIC enabled (Spurious: 0x%x)\n", logical_id, lapic_read(LAPIC_REG_SPURIOUS));

    ap_ready_count++;
    spin_unlock(&ap_start_lock);

    // TODO: Implement the scheduler loop
    while (1) {
        __asm__ volatile("hlt");
        // Dead loop
    }

    // Shouldn't reach here
    panic("AP %d scheduler exited", logical_id);
}

void smp_init(void)
{
    struct limine_smp_response *smp = smp_request.response;

    if (!smp) {
        plogk("SMP: No SMP response\n");
        return;
    }

    plogk("SMP: Found %d CPUs\n", smp->cpu_count);
    cpu_count = smp->cpu_count;

    // Init BootStrap Processor
    for (uint32_t i = 0; i < smp->cpu_count; i++) {
        struct limine_smp_info *cpu = smp->cpus[i];
        cpus[i].id                  = i;
        cpus[i].lapic_id            = cpu->lapic_id;

        // Allocate 16 KiB for each CPU stack
        cpus[i].stack = (uint64_t)malloc(0x10000);

        // Special handling for BSP
        if (cpu->lapic_id == smp->bsp_lapic_id) {
            plogk("SMP: BSP is CPU %d (LAPIC ID: %d)\n", i, cpu->lapic_id);
            set_kernel_stack(ALIGN_DOWN(cpus[i].stack + 0x10000, 16));
            continue;
        } else {
            plogk("SMP: Starting AP %d (LAPIC ID: %d)\n", i, smp->cpus[i]->lapic_id);
            // Configure the AP entry point
            cpu->extra_argument = (uint64_t)&cpus[i];
            cpu->goto_address   = (limine_goto_address)ap_entry;

            send_ipi(cpu->lapic_id, APIC_ICR_STARTUP | 0x08); // 0x8000
            send_ipi(cpu->lapic_id, APIC_ICR_STARTUP | 0x08);
        }
    }
    // Register IPI handler
    register_interrupt_handler(IPI_RESCHEDULE, ipi_reschedule_handler, 0, 0x8e);
    register_interrupt_handler(IPI_HALT, ipi_halt_handler, 0, 0x8e);
    register_interrupt_handler(IPI_TLB_SHOOTDOWN, ipi_tlb_shootdown_handler, 0, 0x8e);
    register_interrupt_handler(IPI_PANIC, ipi_panic_handler, 0, 0x8e);
    // Wait for all APs to be ready
    while (ap_ready_count < cpu_count - 1) { __asm__ volatile("pause"); }
    plogk("SMP: All APs are up, total %d CPUs\n", cpu_count);
}

uint32_t get_cpu_count(void)
{
    return cpu_count;
}

uint32_t get_current_cpu_id(void)
{
    uint64_t cpu_lapic_id = lapic_id();
    for (size_t i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == cpu_lapic_id) { return i; }
    }
    return 0; // Default to CPU 0 if not found
}