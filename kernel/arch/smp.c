/*
 *
 *      smp.c
 *      Symmetric multi-processing
 *
 *      2025/7/6 By W9pi3cZ1
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/cpuid.h>
#include <arch/fpu.h>
#include <arch/gdt.h>
#include <arch/smp.h>
#include <arch/tss.h>
#include <boot/limine.h>
#include <drivers/firmware/apic.h>
#include <kernel/debug/debug.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <process/sched.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/syscall.h>

static cpu_processor_t *cpus;
static size_t           cpu_count = 0;

static volatile uint64_t  ap_ready_count = 0;
static volatile uint64_t  tlb_shootdown_generation;
static volatile uint64_t *tlb_shootdown_ack;
static volatile uint32_t  smp_ready;
static volatile uint32_t  smp_tsc_aux_ready;
spinlock_t                ap_start_lock = {0};
static spinlock_t         tlb_shootdown_lock;

/*
 * CR3 reloads preserve global translations under CR4.PGE.  Explicit
 * flush_tlb_all() requests are stronger: toggling PGE invalidates both
 * global and non-global entries on this logical CPU.
 */
static inline void flush_local_tlb_all(void)
{
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 7)) {
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        return;
    }
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* Handle an NMI on this CPU, flushing the local TLB if a shootdown is pending */
int smp_handle_nmi(void)
{
    if (!__atomic_load_n(&smp_ready, __ATOMIC_ACQUIRE) || !tlb_shootdown_ack) return 0;
    uint32_t cpu_id = get_current_cpu_id();
    if (cpu_id >= cpu_count) return 0;
    uint64_t generation = __atomic_load_n(&tlb_shootdown_generation, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&tlb_shootdown_ack[cpu_id], __ATOMIC_ACQUIRE) >= generation) return 0;

    flush_local_tlb_all();
    __atomic_store_n(&tlb_shootdown_ack[cpu_id], generation, __ATOMIC_RELEASE);
    return 1;
}

void ipi_reschedule_handle_frame(syscall_frame_t *frame) __attribute__((used, noinline));

/* Reschedule and deliver signals with the complete interrupted register set. */
void ipi_reschedule_handle_frame(syscall_frame_t *frame)
{
    disable_intr();
    send_eoi();
    sched_ipi_reschedule();
    if ((frame->cs & 3U) == 3U) (void)signal_deliver_if_pending(frame);
}

/*
 * Compiler-generated interrupt prologues save only registers selected by the
 * optimizer.  Signals need every GPR for sigreturn, so use one fixed frame.
 */
__asm__(".text\n"
        ".global ipi_reschedule_entry\n"
        ".type ipi_reschedule_entry, @function\n"
        "ipi_reschedule_entry:\n"
        "cld\n"
        "testb $3, 8(%rsp)\n"
        "jz 1f\n"
        "swapgs\n"
        "1:\n"
        "pushq %rax\n"
        "pushq %rbx\n"
        "pushq %rcx\n"
        "pushq %rdx\n"
        "pushq %rbp\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        "pushq %r8\n"
        "pushq %r9\n"
        "pushq %r10\n"
        "pushq %r11\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "movq %rsp, %r12\n"
        "movq %r12, %rdi\n"
        "andq $-16, %rsp\n"
        "call ipi_reschedule_handle_frame\n"
        "movq %r12, %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %r11\n"
        "popq %r10\n"
        "popq %r9\n"
        "popq %r8\n"
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rbp\n"
        "popq %rdx\n"
        "popq %rcx\n"
        "popq %rbx\n"
        "popq %rax\n"
        "testb $3, 8(%rsp)\n"
        "jz 2f\n"
        "cli\n"
        "swapgs\n"
        "2:\n"
        "iretq\n"
        ".size ipi_reschedule_entry, .-ipi_reschedule_entry\n");

void ipi_reschedule_entry(void);

/* Downtime Request */
INTERRUPT_BEGIN static void ipi_halt_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    disable_intr();
    lapic_timer_stop();
    send_eoi();
    while (1) __asm__ volatile("hlt");
}
INTERRUPT_END

/* TLB flush request */
INTERRUPT_BEGIN static void ipi_tlb_shootdown_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    disable_intr();
    flush_local_tlb_all();
    uint32_t cpu_id     = get_current_cpu_id();
    uint64_t generation = __atomic_load_n(&tlb_shootdown_generation, __ATOMIC_ACQUIRE);
    if (tlb_shootdown_ack && cpu_id < cpu_count) __atomic_store_n(&tlb_shootdown_ack[cpu_id], generation, __ATOMIC_RELEASE);
    send_eoi();
    enable_intr();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Emergency Error Broadcast */
INTERRUPT_BEGIN static void ipi_panic_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    disable_intr();
    send_eoi();
    while (1) __asm__ volatile("hlt");
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

/* Send an IPI to the current CPU */
void send_ipi_self(uint8_t vector)
{
    uint32_t cpu_id = get_current_cpu_id();
    vector |= IPI_FIXED | APIC_ICR_PHYSICAL;
    if (cpu_id < cpu_count) send_ipi(cpus[cpu_id].lapic_id, vector);
}

/* Flush TLBs of all CPUs */
void flush_tlb_all(void)
{
    flush_local_tlb_all();
    if (!__atomic_load_n(&smp_ready, __ATOMIC_ACQUIRE) || cpu_count < 2 || !tlb_shootdown_ack) return;

    uint64_t irq_flags  = spin_lock_irqsave(&tlb_shootdown_lock);
    uint32_t self       = get_current_cpu_id();
    uint64_t generation = __atomic_add_fetch(&tlb_shootdown_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&tlb_shootdown_ack[self], generation, __ATOMIC_RELEASE);
    for (size_t i = 0; i < cpu_count; i++)
        if (i != self) send_ipi(cpus[i].lapic_id, IPI_NMI | APIC_ICR_PHYSICAL);
    for (size_t i = 0; i < cpu_count; i++) {
        if (i == self) continue;
        while (__atomic_load_n(&tlb_shootdown_ack[i], __ATOMIC_ACQUIRE) < generation) __asm__ volatile("pause");
    }
    spin_unlock_irqrestore(&tlb_shootdown_lock, irq_flags);
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
    /*
     * LAPIC ID reads are MMIO in xAPIC mode and particularly expensive under
     * emulation.  IA32_TSC_AUX is core-local and RDTSCP returns it directly,
     * making current_task()/process_current() cheap enough for syscall hot
     * paths.  Keep the LAPIC lookup as the early-boot/unsupported fallback.
     */
    if (__atomic_load_n(&smp_tsc_aux_ready, __ATOMIC_ACQUIRE)) {
        uint32_t cpu_id;
        (void)rdtscp(&cpu_id);
        if (cpu_id < cpu_count) return cpu_id;
    }
    uint64_t current_lapic_id = lapic_id();
    for (size_t i = 0; i < cpu_count; i++)
        if (cpus[i].lapic_id == current_lapic_id) return i;
    return 0; // Default to CPU 0 if not found
}

/* Get the per-CPU state for the processor executing this code. */
cpu_processor_t *get_current_cpu(void)
{
    if (!cpus) return NULL;
    uint32_t cpu_id = get_current_cpu_id();
    return cpu_id < cpu_count ? &cpus[cpu_id] : NULL;
}

/* Initialize the TSS for the AP */
static void ap_init_tss(cpu_processor_t *cpu)
{
    compiler_barrier();
    uint64_t address     = (uint64_t)(cpu->tss);
    uint64_t low_base    = (((address & 0xffffff)) << 16);
    uint64_t mid_base    = (((((address >> 24)) & 0xff)) << 56);
    uint64_t high_base   = (address >> 32);
    uint64_t access_byte = (((uint64_t)(0x89)) << 40);
    uint64_t limit       = (uint64_t)(sizeof(tss_t) - 1);

    cpu->gdt->entries[7] = (((low_base | mid_base) | limit) | access_byte);
    cpu->gdt->entries[8] = high_base;
    cpu->tss->ist[0]     = ALIGN_DOWN(((uint64_t)cpu->tss_stack) + sizeof(tss_stack_t), 16);

    /* Set kernel stack */
    pointer_cast_t cast;
    cast.ptr                = cpu->kernel_stack;
    cpu->tss->rsp[0]        = ALIGN_DOWN((uint64_t)cast.val + sizeof(kernel_stack_t), 16);
    cpu->syscall.kernel_rsp = cpu->tss->rsp[0];

    __asm__ volatile("ltr %w[offset]" ::[offset] "rm"((uint16_t)0x38) : "memory");
}

/* Initialize the GDT for the AP */
static void ap_init_gdt(cpu_processor_t *cpu)
{
    cpu->gdt->entries[0] = 0x0000000000000000; // NULL descriptor
    cpu->gdt->entries[1] = 0x00a09a0000000000; // Kernel code segment
    cpu->gdt->entries[2] = 0x00c0920000000000; // Kernel data segment
    cpu->gdt->entries[3] = 0x00a0fa0000000000; // User code segment
    cpu->gdt->entries[4] = 0x00c0f20000000000; // User data segment
    cpu->gdt->entries[5] = 0x00c0f20000000000; // SYSRET user data segment
    cpu->gdt->entries[6] = 0x00a0fa0000000000; // SYSRET user code segment

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

/*
 * Point this CPU's GS base at its per-CPU window so kernel code runs with
 * %gs -> per-CPU.  KernelGSBase holds the (initially absent) user GS, which
 * swapgs exchanges back on return to user mode.
 */
static void cpu_gs_install(cpu_processor_t *cpu)
{
    wrmsr(0xC0000101, (uint64_t)&cpu->syscall); // GS base = per-CPU window
    set_user_gs_base(0);                        // KernelGSBase: no user GS yet
}

/* Multi-core boot entry */
void ap_entry(struct limine_smp_info *info)
{
    fpu_init();
    cpu_enable_nx();

    /* Load the page table */
    page_directory_t *krnl_pagedir = get_kernel_pagedir();
    pointer_cast_t    cast;
    cast.ptr = krnl_pagedir->table;
    cast.ptr = virt_to_phys(cast.val);
    enable_paging(cast.val);

    cast.val             = info->extra_argument;
    cpu_processor_t *cpu = (cpu_processor_t *)cast.ptr;

    if (cpu_support_rdtscp()) wrmsr(0xC0000103, cpu->id); // IA32_TSC_AUX

    /* Initializing the GDT */
    ap_init_gdt(cpu);

    /* Initializing the IDT */
    __asm__ volatile("lidt %0" ::"m"(idt_pointer) : "memory");

    /* SYSCALL MSRs are core-local. */
    syscall_init_cpu();

    /* Establish the kernel-mode GS base for this AP. */
    cpu_gs_install(cpu);

    /* Initializing Local APIC */
    local_apic_init();

    spin_lock(&ap_start_lock);
    ap_ready_count++;
    spin_unlock(&ap_start_lock);

    sched_ap_online(cpu->id);

    /* Enter the AP idle loop; interrupts (timer, IPI) wake it for scheduling */
    sched_ap_start(cpu->id);
}

/* Initializing Symmetric Multi-Processing */
void smp_init(void)
{
    struct limine_smp_response *smp = smp_request.response;

    if (!smp) {
        plogk("smp: No SMP response.\n");
        return;
    }

    cpu_count         = (!CPU_MAX_COUNT) ? smp->cpu_count : (smp->cpu_count > CPU_MAX_COUNT ? CPU_MAX_COUNT : smp->cpu_count);
    cpus              = (cpu_processor_t *)aligned_alloc(16, sizeof(cpu_processor_t) * cpu_count);
    tlb_shootdown_ack = calloc(cpu_count, sizeof(*tlb_shootdown_ack));
    if (!cpus || !tlb_shootdown_ack) panic("smp: Cannot allocate CPU state.");
    plogk("smp: Found %d CPUs.\n", cpu_count);

    /* Initialize the per-CPU state of every processor, with special handling for the BSP */
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct limine_smp_info *cpu = smp->cpus[i];
        cpus[i].id                  = i;
        cpus[i].lapic_id            = cpu->lapic_id;
        cpus[i].syscall.user_rsp    = 0;
        cpus[i].syscall.kernel_rsp  = 0;
        cpus[i].syscall.current     = NULL;
        cpus[i].fpu.fpu_live        = NULL;
        cpus[i].fpu.fpu_kernel_cnt  = 0;
        cpus[i].fpu.fpu_irq_saved   = 0;
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
            if (cpu_support_rdtscp()) wrmsr(0xC0000103, cpus[i].id); // IA32_TSC_AUX

            /* Establish the kernel-mode GS base for the BSP before any scheduler or syscall code runs (sched_init() writes gs:16). */
            cpu_gs_install(&cpus[i]);
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
    register_interrupt_handler(IPI_RESCHEDULE, (void *)ipi_reschedule_entry, 0, 0x8e);
    register_interrupt_handler(IPI_HALT, (void *)ipi_halt_handler, 0, 0x8e);
    register_interrupt_handler(IPI_TLB_SHOOTDOWN, (void *)ipi_tlb_shootdown_handler, 0, 0x8e);
    register_interrupt_handler(IPI_PANIC, (void *)ipi_panic_handler, 0, 0x8e);
    plogk("smp: IPI handlers registered.\n");

    /* Wait for all APs to be ready */
    while (ap_ready_count < cpu_count - 1) __asm__ volatile("pause");
    if (cpu_support_rdtscp()) __atomic_store_n(&smp_tsc_aux_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&smp_ready, 1, __ATOMIC_RELEASE);
    for (size_t i = 0; i < cpu_count; i++) plogk("smp: CPU %03u: tss_stack = %p, kernel_stack = %p\n", cpus[i].id, cpus[i].tss_stack, cpus[i].kernel_stack);
    plogk("smp: All APs are up, total %llu CPUs.\n", cpu_count);
}
