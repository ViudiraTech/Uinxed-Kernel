/*
 *
 *      interrupt.h
 *      Interrupt related header files
 *
 *      2024/8/1 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_INTERRUPT_H_
#define INCLUDE_INTERRUPT_H_

#include <arch/idt.h>
#include <libs/std/stdint.h>

#if defined(__clang__)
#    define INTERRUPT_BEGIN _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wexcessive-regsave\"") __attribute__((interrupt, target("general-regs-only")))
#    define INTERRUPT_END   _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#    define INTERRUPT_BEGIN __attribute__((interrupt, target("general-regs-only")))
#    define INTERRUPT_END
#else
#    error "Unknown compiler"
#endif

/* Scheduler return hook; kept as a declaration here to avoid a header cycle. */
void sched_maybe_preempt(void);

/* An interrupt from user mode runs with the user's GS; swap to the per-CPU base on entry and back before returning (frame->cs tells which case). */
static inline void irq_enter_gs(interrupt_frame_t *frame)
{
    if ((frame->cs & 3) == 3) __asm__ volatile("swapgs" ::: "memory");
}

/* Leave: cli closes the scheduling/swapgs->iretq window; iretq restores the saved IF. */
static inline void irq_leave_gs(interrupt_frame_t *frame)
{
    if ((frame->cs & 3) == 3) {
        /*
         * Device IRQs can wake a task on this CPU without sending an IPI.
         * Honor that wake before restoring userspace so an input/compositor
         * waiter is not stranded until the next periodic timer interrupt.
         */
        __asm__ volatile("cli" ::: "memory");
        sched_maybe_preempt();
        __asm__ volatile("swapgs" ::: "memory");
    }
}

/* Leave an NMI/other non-preemptible entry without entering the scheduler. */
static inline void irq_leave_gs_no_preempt(interrupt_frame_t *frame)
{
    if ((frame->cs & 3) == 3) __asm__ volatile("swapgs" ::: "memory");
}

/* Empty function handling */
extern void (*empty_handle[256])(interrupt_frame_t *frame);

/* Spurious NMIs that arrived with no pending TLB shootdown */
extern uint64_t nmi_spurious_count;

/* Register ISR interrupt processing */
void isr_registe_handle(void);

#endif // INCLUDE_INTERRUPT_H_
