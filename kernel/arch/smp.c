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
 * CPUID leaves 0x1f/0x0b describe topology by giving bit shifts in the
 * x2APIC id.  The shifts are uniform across the machine, so the BSP can
 * decode every Limine-provided APIC id before the APs start.
 */
static int smp_topology_shifts(uint8_t *smt_shift, uint8_t *core_shift)
{
    uint32_t max_leaf, ebx, ecx, edx;
    uint32_t leaf;

    cpuid_safe(0, 0, &max_leaf, &ebx, &ecx, &edx);
    leaf = max_leaf >= 0x1f ? 0x1f : (max_leaf >= 0x0b ? 0x0b : 0);
    if (!leaf) return 0;

    *smt_shift  = 0;
    *core_shift = 0;
    for (uint32_t level = 0; level < 8; level++) {
        uint32_t eax;
        cpuid_safe(leaf, level, &eax, &ebx, &ecx, &edx);
        if (!ebx) break;
        uint32_t type  = (ecx >> 8) & 0xffU;
        uint8_t  shift = (uint8_t)(eax & 0x1fU);
        if (type == 1)
            *smt_shift = shift;
        else if (type == 2)
            *core_shift = shift;
    }
    if (!*core_shift) *core_shift = *smt_shift;
    return *core_shift || *smt_shift;
}

/* Return a mask containing the requested number of low-order bits. */
static uint32_t smp_topology_mask(uint8_t bits)
{
    if (!bits) return 0;
    if (bits >= 32) return UINT32_MAX;
    return (1U << bits) - 1U;
}

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

    /*
     * Flush for the generation observed at entry, then re-check: an
     * initiator may have bumped the generation while we were flushing.
     * Publishing ack only for a generation whose flush we completed keeps
     * "ack >= G" a truthful statement about OUR translation cache state,
     * so no later generation can ever satisfy an earlier waiter for us.
     */
    uint64_t generation;
    do {
        generation = __atomic_load_n(&tlb_shootdown_generation, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&tlb_shootdown_ack[cpu_id], __ATOMIC_ACQUIRE) >= generation) {
            /*
             * Nothing to flush for this generation: the NMI is a late or
             * duplicated shootdown delivery (flush_tlb_all() re-arms the NMI
             * past a deadline, and the original may already have been
             * processed and acked).  This is expected, not fatal - report it
             * as handled so the ISR returns instead of panicking.  Before
             * smp_ready the early return above already panics on a real NMI.
             */
            return 1;
        }
        flush_local_tlb_all();
        __atomic_store_n(&tlb_shootdown_ack[cpu_id], generation, __ATOMIC_RELEASE);
    } while (__atomic_load_n(&tlb_shootdown_generation, __ATOMIC_RELAXED) != generation);

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
    /*
     * Keep interrupts masked until the compiler-generated interrupt epilogue
     * executes IRETQ.  Re-enabling them here allowed a timer/reschedule IPI to
     * nest while this handler still owned its interrupt frame (and, for user
     * returns, before irq_leave_gs() had restored GS), corrupting return state
     * under heavy cross-CPU TLB traffic.
     */
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

    /*
     * x86 NMIs are not queued: an NMI that arrives while another NMI is
     * still being delivered or handled is silently dropped.  Waiting for a
     * per-CPU ack alone is therefore unsound - a LATER generation's flush
     * would satisfy THIS waiter while the target never flushed our pages,
     * letting it keep stale translations to freed and reused frames.
     * Re-arm the NMI whenever a target stays silent past the deadline so
     * every drop is repaired instead of masked.
     */
    const uint64_t resend_period = 200000ULL; /* ~80 us at 2.5 GHz TSC */

    for (size_t i = 0; i < cpu_count; i++) {
        if (i == self) continue;
        send_ipi(cpus[i].lapic_id, IPI_NMI | APIC_ICR_PHYSICAL);
    }

    for (size_t i = 0; i < cpu_count; i++) {
        if (i == self) continue;
        uint64_t deadline = rdtsc() + resend_period;
        while (__atomic_load_n(&tlb_shootdown_ack[i], __ATOMIC_ACQUIRE) < generation) {
            if (rdtsc() > deadline) {
                /*
                 * NMI delivery can be dropped under virtualization.  Re-arm
                 * the NMI AND ping the fixed-vector handler, which performs
                 * the same full local flush before acking the current
                 * generation - a truthful ack by construction, so a lost NMI
                 * is repaired instead of masked.
                 */
                send_ipi(cpus[i].lapic_id, IPI_NMI | APIC_ICR_PHYSICAL);
                send_ipi(cpus[i].lapic_id, IPI_TLB_SHOOTDOWN | APIC_ICR_PHYSICAL);
                deadline = rdtsc() + resend_period;
            }
            __asm__ volatile("pause");
        }
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

/* Return the processor descriptor for a valid CPU ID. */
const cpu_processor_t *get_cpu_processor(uint32_t cpu_id)
{
    return cpus && cpu_id < cpu_count ? &cpus[cpu_id] : NULL;
}

/* Check whether two CPUs belong to the same physical core. */
int cpu_topology_same_core(uint32_t first, uint32_t second)
{
    const cpu_processor_t *a = get_cpu_processor(first);
    const cpu_processor_t *b = get_cpu_processor(second);
    return a && b && a->package_id == b->package_id && a->core_id == b->core_id;
}

/* Check whether two CPUs belong to the same physical package. */
int cpu_topology_same_package(uint32_t first, uint32_t second)
{
    const cpu_processor_t *a = get_cpu_processor(first);
    const cpu_processor_t *b = get_cpu_processor(second);
    return a && b && a->package_id == b->package_id;
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
    cpu->tss->ist[1]     = ALIGN_DOWN(((uint64_t)cpu->nmi_stack) + sizeof(tss_stack_t), 16);

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
    __atomic_add_fetch(&ap_ready_count, 1, __ATOMIC_RELEASE);
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

    /* aligned_alloc() does not clear memory; never expose stale per-CPU state. */
    memset(cpus, 0, sizeof(cpu_processor_t) * cpu_count);
    plogk("smp: Found %d CPUs.\n", cpu_count);

    /*
     * The Limine MP response identifies the BSP by LAPIC ID but does not
     * promise that its entry is first.  The scheduler deliberately reserves
     * logical CPU 0 for the boot task and the global tick, so build our
     * logical topology with the BSP first instead of inheriting firmware
     * enumeration order.  Scan the complete response before applying the
     * configured CPU limit so the BSP cannot be truncated out.
     */
    uint64_t bsp_index = smp->cpu_count;
    for (uint64_t i = 0; i < smp->cpu_count; i++) {
        if (smp->cpus[i]->lapic_id == smp->bsp_lapic_id) {
            bsp_index = i;
            break;
        }
    }
    if (bsp_index == smp->cpu_count) panic("smp: BSP is missing from the Limine CPU list.");

    uint8_t smt_shift = 0, core_shift = 0;
    int     topology_valid = smp_topology_shifts(&smt_shift, &core_shift);

    /* Initialize the per-CPU state of every processor, with special handling for the BSP */
    uint64_t next_ap_index = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        uint64_t source_index;
        if (i == 0) {
            source_index = bsp_index;
        } else {
            while (next_ap_index == bsp_index) next_ap_index++;
            source_index = next_ap_index++;
        }
        struct limine_smp_info *cpu = smp->cpus[source_index];
        cpus[i].id                  = i;
        cpus[i].lapic_id            = cpu->lapic_id;
        if (topology_valid) {
            uint32_t smt_mask  = smp_topology_mask(smt_shift);
            uint32_t core_bits = core_shift > smt_shift ? core_shift - smt_shift : 0;
            uint32_t core_mask = smp_topology_mask((uint8_t)core_bits);
            cpus[i].thread_id  = (uint32_t)cpu->lapic_id & smt_mask;
            cpus[i].core_id    = smt_shift >= 32 ? 0 : ((uint32_t)cpu->lapic_id >> smt_shift) & core_mask;
            cpus[i].package_id = core_shift >= 32 ? 0 : (uint32_t)cpu->lapic_id >> core_shift;
        } else {
            /* Conservative fallback: no fake SMT sharing, one package. */
            cpus[i].thread_id  = 0;
            cpus[i].core_id    = (uint32_t)cpu->lapic_id;
            cpus[i].package_id = 0;
        }
        cpus[i].capacity           = 1024;
        cpus[i].syscall.user_rsp   = 0;
        cpus[i].syscall.kernel_rsp = 0;
        cpus[i].syscall.current    = NULL;
        cpus[i].syscall.cpu_id     = i;
        cpus[i].syscall.reserved   = 0;
        cpus[i].fpu.fpu_live       = NULL;
        cpus[i].fpu.fpu_kernel_cnt = 0;
        cpus[i].fpu.fpu_irq_saved  = 0;
        /* Allocate kernel stack for each CPU */
        cpus[i].kernel_stack = malloc(sizeof(kernel_stack_t)); // 64 KiB stack
        if (!cpus[i].kernel_stack) { panic("smp: failed to allocate kernel stack for CPU %u", i); }

        /* Special handling for BSP */
        if (i == 0) {
            cpus[i].gdt       = &gdt0;
            cpus[i].tss_stack = &tss_stack;
            cpus[i].nmi_stack = &nmi_stack;
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
        if (!cpus[i].gdt) { panic("smp: failed to allocate GDT for CPU %u", i); }
        memset(cpus[i].gdt, 0, sizeof(gdt_t)); // Clear dirty data
        cpus[i].tss_stack = malloc(sizeof(tss_stack_t));
        if (!cpus[i].tss_stack) { panic("smp: failed to allocate TSS stack for CPU %u", i); }
        cpus[i].nmi_stack = malloc(sizeof(tss_stack_t));
        if (!cpus[i].nmi_stack) { panic("smp: failed to allocate NMI stack for CPU %u", i); }
        cpus[i].tss = (tss_t *)aligned_alloc(16, ALIGN_UP(sizeof(tss_t), 16));
        if (!cpus[i].tss) { panic("smp: failed to allocate TSS for CPU %u", i); }
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
    while (__atomic_load_n(&ap_ready_count, __ATOMIC_ACQUIRE) < cpu_count - 1) __asm__ volatile("pause");
    if (cpu_support_rdtscp()) __atomic_store_n(&smp_tsc_aux_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&smp_ready, 1, __ATOMIC_RELEASE);
    for (size_t i = 0; i < cpu_count; i++) plogk("smp: CPU %03u: tss_stack = %p, nmi_stack = %p, kernel_stack = %p\n", cpus[i].id, cpus[i].tss_stack, cpus[i].nmi_stack, cpus[i].kernel_stack);
    plogk("smp: All APs are up, total %llu CPUs.\n", cpu_count);
}
