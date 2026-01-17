/*
 *
 *      smp.c
 *      Symmetric multi-processing
 *
 *      2025/7/6 By W9pi3cZ1
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <apic.h>
#include <common.h>
#include <debug.h>
#include <eis.h>
#include <frame.h>
#include <gdt.h>
#include <heap.h>
#include <hhdm.h>
#include <interrupt.h>
#include <limine.h>
#include <page.h>
#include <printk.h>
#include <smp.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uinxed.h>

static cpu_processor_t *cpus;
static size_t           cpu_count = 0;

static volatile uint64_t ap_ready_count = 0;
spinlock_t               ap_start_lock  = {0};

/* Rescheduling Requests */
INTERRUPT_BEGIN static void ipi_reschedule_handler(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    /* TODO: Handle rescheduling */
    send_eoi();
    enable_intr();
}
INTERRUPT_END

/* Downtime Request */
INTERRUPT_BEGIN static void ipi_halt_handler(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    /* TODO: Handle halting */
    send_eoi();
    enable_intr();
}
INTERRUPT_END

/* TLB flush request */
INTERRUPT_BEGIN static void ipi_tlb_shootdown_handler(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    /* TODO: Handle TLB shootdown */
    send_eoi();
    enable_intr();
}
INTERRUPT_END

/* Emergency Error Broadcast */
INTERRUPT_BEGIN static void ipi_panic_handler(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    /* TODO: Handle panic */
    send_eoi();
    enable_intr();
}
INTERRUPT_END

/* Send an IPI to all CPUs */
void send_ipi_all(uint8_t vector)
{
    vector |= IPI_FIXED | APIC_ICR_PHYSICAL;
    for (size_t i = 0; i < cpu_count; i++)
        if (cpus[i].id != get_current_cpu_id()) send_ipi(cpus[i].lapic_id, vector);
}

/* Send an IPI to the specified CPU */
void send_ipi_cpu(uint32_t cpu_id, uint8_t vector)
{
    vector |= IPI_FIXED | APIC_ICR_PHYSICAL;
    if (cpu_id < cpu_count && cpu_id != get_current_cpu_id()) send_ipi(cpus[cpu_id].lapic_id, vector);
}

/* Flush TLBs of all CPUs */
void flush_tlb_all(void)
{
    send_ipi_all(IPI_TLB_SHOOTDOWN);
}

/* Flushing TLB by address range */
void flush_tlb_range(uint64_t start, uint64_t end)
{
    for (uint64_t addr = start; addr < end; addr += PAGE_4K_SIZE) flush_tlb(addr);
}

/* Get the number of CPUs */
uint32_t get_cpu_count(void)
{
    return cpu_count;
}

/* Get the ID of the current CPU */
uint32_t get_current_cpu_id(void)
{
    for (size_t i = 0; i < cpu_count; i++)
        if (cpus[i].lapic_id == lapic_id()) return i;
    return 0; // Default to CPU 0 if not found
}

/* Initialize the TSS for the AP  */
void ap_init_tss(cpu_processor_t *cpu)
{
    compiler_barrier();
    uint64_t address     = (uint64_t)(cpu->tss);
    uint64_t low_base    = (((address & 0xffffff)) << 16);
    uint64_t mid_base    = (((((address >> 24)) & 0xff)) << 56);
    uint64_t high_base   = (address >> 32);
    uint64_t access_byte = (((uint64_t)(0x89)) << 40);
    uint64_t limit       = (uint64_t)(sizeof(tss_t) - 1);

    cpu->gdt->entries[5] = (((low_base | mid_base) | limit) | access_byte);
    cpu->gdt->entries[6] = high_base;
    cpu->tss->ist[0]     = ALIGN_DOWN(((uint64_t)cpu->tss_stack) + sizeof(tss_stack_t), 16);

    /* Set kernel stack */
    pointer_cast_t cast;
    cast.ptr         = cpu->kernel_stack;
    cpu->tss->rsp[0] = ALIGN_DOWN((uint64_t)cast.val + sizeof(kernel_stack_t), 16);

    __asm__ volatile("ltr %w[offset]" ::[offset] "rm"((uint16_t)0x28) : "memory");
}

/* Initialize the GDT for the AP */
void ap_init_gdt(cpu_processor_t *cpu)
{
    cpu->gdt->entries[0] = 0x0000000000000000; // NULL descriptor
    cpu->gdt->entries[1] = 0x00a09a0000000000; // Kernel code segment
    cpu->gdt->entries[2] = 0x00c0920000000000; // Kernel data segment
    cpu->gdt->entries[3] = 0x00c0f20000000000; // User code segment
    cpu->gdt->entries[4] = 0x00a0fa0000000000; // User data segment

    cpu->gdt->pointer = ((gdt_register_t) {
        .size = (uint16_t)(sizeof(gdt_entries_t) - 1),
        .ptr  = (gdt_entries_t *)&cpu->gdt->entries,
    });

    __asm__ volatile("lgdt %[ptr]; push %[cseg]; lea 1f(%%rip), %%rax; push %%rax; lretq;"
                     "1:"
                     "mov %[dseg], %%ds;"
                     "mov %[dseg], %%fs;"
                     "mov %[dseg], %%gs;"
                     "mov %[dseg], %%es;"
                     "mov %[dseg], %%ss;" ::[ptr] "m"(cpu->gdt->pointer),
                     [cseg] "rm"((uint64_t)0x8), [dseg] "rm"((uint64_t)0x10)
                     : "memory");
    ap_init_tss(cpu);
}

/* Multi-core boot entry */
void ap_entry(struct limine_smp_info *info)
{
    init_fpu();
    init_sse();
    init_avx();

    /* load page table */
    page_directory_t *krnl_pagedir = get_kernel_pagedir();
    pointer_cast_t    cast;
    cast.ptr = krnl_pagedir->table;
    cast.ptr = virt_to_phys(cast.val);
    enable_paging(cast.val);

    cast.val             = info->extra_argument;
    cpu_processor_t *cpu = (cpu_processor_t *)cast.ptr;

    /* Initializing the GDT */
    ap_init_gdt(cpu);

    /* Initializing the IDT */
    __asm__ volatile("lidt %0" ::"m"(idt_pointer) : "memory");

    /* Initializing Local APIC */
    local_apic_init();

    uint64_t flags = spin_lock(&ap_start_lock);
    ap_ready_count++;
    spin_unlock(&ap_start_lock, flags);

    /* TODO: Implement the scheduler loop */
    enable_intr();
    while (1) __asm__ volatile("hlt");

    /* Shouldn't reach here */
    panic("AP %d scheduler exited.", cpu->id);
}

/* Initializing Symmetric Multi-Processing */
void smp_init(void)
{
    struct limine_smp_response *smp = smp_request.response;

    if (!smp) {
        plogk("SMP: No SMP response.\n");
        return;
    }

    cpu_count = smp->cpu_count;
    cpus      = (cpu_processor_t *)aligned_alloc(16, sizeof(cpu_processor_t) * cpu_count);
    plogk("smp: Found %d CPUs.\n", cpu_count);

    /* Init BootStrap Processor */
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct limine_smp_info *cpu = smp->cpus[i];
        cpus[i].id                  = i;
        cpus[i].lapic_id            = cpu->lapic_id;
        /* Allocate kernel stack for each CPU */
        cpus[i].kernel_stack = malloc(sizeof(kernel_stack_t)); // 64 KiB stack

        /* Special handling for BSP */
        if (cpu->lapic_id == smp->bsp_lapic_id) {
            cpus[i].gdt       = &gdt0;
            cpus[i].tss_stack = &tss_stack;
            cpus[i].tss       = &tss0;

            pointer_cast_t cast;
            cast.ptr = cpus[i].kernel_stack;
            set_kernel_stack(ALIGN_DOWN((uint64_t)cast.val + sizeof(kernel_stack_t), 16ULL));
            continue;
        }
        cpus[i].gdt = (gdt_t *)aligned_alloc(16, ALIGN_UP(sizeof(gdt_t), 16));
        memset(cpus[i].gdt, 0, sizeof(gdt_t)); // Clear dirty data
        cpus[i].tss_stack = malloc(sizeof(tss_stack_t));
        cpus[i].tss       = (tss_t *)aligned_alloc(16, ALIGN_UP(sizeof(tss_t), 16));
        memset(cpus[i].tss, 0, sizeof(tss_t)); // Clear dirty data

        /* Configure the AP entry point */
        cpu->extra_argument = (uint64_t)&cpus[i];
        cpu->goto_address   = (limine_goto_address)ap_entry;
    }

    /* Register IPI handler */
    register_interrupt_handler(IPI_RESCHEDULE, (void *)ipi_reschedule_handler, 0, 0x8e);
    register_interrupt_handler(IPI_HALT, (void *)ipi_halt_handler, 0, 0x8e);
    register_interrupt_handler(IPI_TLB_SHOOTDOWN, (void *)ipi_tlb_shootdown_handler, 0, 0x8e);
    register_interrupt_handler(IPI_PANIC, (void *)ipi_panic_handler, 0, 0x8e);
    plogk("smp: IPI handlers registered.\n");

    /* Wait for all APs to be ready */
    while (ap_ready_count < cpu_count - 1) __asm__ volatile("pause");
    for (size_t i = 0; i < cpu_count; i++)
        plogk("smp: CPU %03u: tss_stack = %p, kernel_stack = %p\n", cpus[i].id, cpus[i].tss_stack, cpus[i].kernel_stack);
    plogk("smp: All APs are up, total %llu CPUs.\n", cpu_count);
}
