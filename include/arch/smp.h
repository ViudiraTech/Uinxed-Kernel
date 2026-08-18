/*
 *
 *      smp.h
 *      Symmetric multi-processing header file
 *
 *      2025/7/6 By W9pi3cZ1
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SMP_H_
#define INCLUDE_SMP_H_

#include <arch/common.h>
#include <arch/gdt.h>
#include <arch/tss.h>
#include <boot/limine.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define KERNEL_STACK_SIZE 0x10000 // 64 KiB

#ifndef CPU_MAX_COUNT
#    define CPU_MAX_COUNT 0
#endif

typedef uint8_t kernel_stack_t[KERNEL_STACK_SIZE];

struct task;

#define SYSCALL_CPU_USER_RSP_OFFSET   0
#define SYSCALL_CPU_KERNEL_RSP_OFFSET 8
#define SYSCALL_CPU_CURRENT_OFFSET    16

/*
 * GS-relative per-CPU window.  In kernel mode %gs points here, so the fields
 * are addressed as %gs:0, %gs:8 and %gs:16 (the syscall prologue uses the first
 * two to switch stacks; current_task() is a single %gs:16 load).
 */
typedef struct {
        uint64_t     user_rsp;   // 0x00 - user stack saved by syscall entry
        uint64_t     kernel_rsp; // 0x08 - per-CPU kernel stack
        struct task *current;    // 0x10 - task running on this CPU
} syscall_cpu_state_t;

_Static_assert(offsetof(syscall_cpu_state_t, user_rsp) == SYSCALL_CPU_USER_RSP_OFFSET, "syscall user RSP offset");
_Static_assert(offsetof(syscall_cpu_state_t, kernel_rsp) == SYSCALL_CPU_KERNEL_RSP_OFFSET, "syscall kernel RSP offset");
_Static_assert(offsetof(syscall_cpu_state_t, current) == SYSCALL_CPU_CURRENT_OFFSET, "syscall current offset");

/* Read the current task from the GS-relative per-CPU window (one load) */
static inline struct task *percpu_gs_current(void)
{
    struct task *t;
    __asm__("movq %%gs:%c1, %0" : "=r"(t) : "i"(SYSCALL_CPU_CURRENT_OFFSET) : "memory");
    return t;
}

/* Store the current task into the GS-relative per-CPU window */
static inline void percpu_gs_set_current(struct task *t)
{
    __asm__("movq %0, %%gs:%c1" : : "r"(t), "i"(SYSCALL_CPU_CURRENT_OFFSET) : "memory");
}

/*
 * Park the current task's user GS base in KERNEL_GS_BASE.  In kernel mode the
 * hidden GS base is the per-CPU window, so the user GS lives in KERNEL_GS_BASE
 * and the return-to-user swapgs restores it.
 */
static inline void set_user_gs_base(uint64_t user_gs_base)
{
    wrmsr(0xC0000102, user_gs_base);
}

/* Per-CPU floating-point state (see <arch/fpu.h>) */
typedef struct {
        struct task *fpu_live;       // task whose FPU state is currently loaded on this CPU
        uint8_t      fpu_kernel_cnt; // nested kernel_fpu_begin() depth
        uint8_t      fpu_irq_saved;  // RFLAGS.IF of the outermost kernel_fpu_begin()
} fpu_percpu_t;

typedef struct cpu_processor {
        uint64_t            id;
        uint64_t            lapic_id;
        gdt_t              *gdt;
        tss_stack_t        *tss_stack;
        tss_t              *tss;
        kernel_stack_t     *kernel_stack;
        syscall_cpu_state_t syscall;
        fpu_percpu_t        fpu;
} cpu_processor_t;

/* Send an IPI to all CPUs */
void send_ipi_all(uint8_t vector);

/* Send an IPI to the specified CPU */
void send_ipi_cpu(uint32_t cpu_id, uint8_t vector);

/* Flush TLBs of all CPUs */
void flush_tlb_all(void);

/* Flushing TLB by address range */
void flush_tlb_range(uint64_t start, uint64_t end);

/* Handle an NMI used for a pending TLB shootdown. */
int smp_handle_nmi(void);

/* Get the number of CPUs */
uint32_t get_cpu_count(void);

/* Get the ID of the current CPU */
uint32_t get_current_cpu_id(void);

/* Get the current CPU's per-CPU state. */
cpu_processor_t *get_current_cpu(void);

/* Multi-core boot entry */
void ap_entry(struct limine_smp_info *info);

/* Initializing Symmetric Multi-Processing */
void smp_init(void);

#endif // INCLUDE_SMP_H_
