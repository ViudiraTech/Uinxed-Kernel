/*
 *
 *      inthandle.c
 *      Interrupt handler
 *
 *      2024/8/1 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/exception_entry.h>
#include <arch/fpu.h>
#include <arch/idt.h>
#include <arch/smp.h>
#include <kernel/debug/debug.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <process/process.h>
#include <process/sched.h>
#include <syscall/syscall.h>

/* Reserved DR6 bits, which read back set in the value the register returns. */
#define DR6_RESERVED 0xffff0ff0ULL

uint64_t nmi_spurious_count;

/* Interrupt/exception entry handler for vector 0. */
void exception_0_entry(void);

/* Interrupt/exception entry handler for vector 1. */
void exception_1_entry(void);

/* Interrupt/exception entry handler for vector 3. */
void exception_3_entry(void);

/* Interrupt/exception entry handler for vector 4. */
void exception_4_entry(void);

/* Interrupt/exception entry handler for vector 5. */
void exception_5_entry(void);

/* Interrupt/exception entry handler for vector 6. */
void exception_6_entry(void);

/* Interrupt/exception entry handler for vector 7. */
void exception_7_entry(void);

/* Interrupt/exception entry handler for vector 10. */
void exception_10_entry(void);

/* Interrupt/exception entry handler for vector 11. */
void exception_11_entry(void);

/* Interrupt/exception entry handler for vector 12. */
void exception_12_entry(void);

/* Interrupt/exception entry handler for vector 13. */
void exception_13_entry(void);

/* Interrupt/exception entry handler for vector 16. */
void exception_16_entry(void);

/* Interrupt/exception entry handler for vector 17. */
void exception_17_entry(void);

/* Interrupt/exception entry handler for vector 19. */
void exception_19_entry(void);

/* Page-fault entry, generated alongside the fixed-exception entries. */
void page_fault_entry(void);

/* Entry handlers for the reserved vectors 15 and 20-31. */
void reserved_15_entry(void);
void reserved_20_entry(void);
void reserved_21_entry(void);
void reserved_22_entry(void);
void reserved_23_entry(void);
void reserved_24_entry(void);
void reserved_25_entry(void);
void reserved_26_entry(void);
void reserved_27_entry(void);
void reserved_28_entry(void);
void reserved_29_entry(void);
void reserved_30_entry(void);
void reserved_31_entry(void);

_Static_assert(offsetof(syscall_frame_t, rip) == 15 * sizeof(uint64_t), "bad fixed exception GPR layout");
_Static_assert(offsetof(syscall_frame_t, cs) == 16 * sizeof(uint64_t), "bad fixed exception iret layout");

/* Signal, si_code, si_addr source and console text for one fixed exception vector. */
typedef struct {
        int         signal;
        int         code;   // si_code, or 0 for the vectors whose code comes from hardware state
        bool        use_ip; // si_addr carries the trapping instruction pointer
        const char *name;
        const char *message;
} exception_signal_t;

/*
 * Signal and si_code for each fixed exception vector; the vectors with
 * dedicated handlers (#PF, #DF, NMI, #MC, reserved) are absent.  #NM is
 * unreachable: no instruction raises it while CR0.TS and CR0.EM are clear.
 */
static const exception_signal_t exception_signals[] = {
    [ISR_0]  = {SIGFPE,  FPE_INTDIV, true,  "#DE", "divide error"                 },
    [ISR_1]  = {SIGTRAP, 0,          true,  "#DB", "debug trap"                   },
    [ISR_3]  = {SIGTRAP, SI_KERNEL,  false, "#BP", "breakpoint"                   },
    [ISR_4]  = {SIGSEGV, SI_KERNEL,  false, "#OF", "integer overflow"             },
    [ISR_5]  = {SIGSEGV, SI_KERNEL,  false, "#BR", "bound range exceeded"         },
    [ISR_6]  = {SIGILL,  ILL_ILLOPN, false, "#UD", "invalid opcode"               },
    [ISR_7]  = {SIGILL,  ILL_COPROC, false, "#NM", "device not available"         },
    [ISR_10] = {SIGSEGV, SI_KERNEL,  false, "#TS", "invalid TSS"                  },
    [ISR_11] = {SIGBUS,  SI_KERNEL,  false, "#NP", "segment not present"          },
    [ISR_12] = {SIGBUS,  SI_KERNEL,  false, "#SS", "stack-segment fault"          },
    [ISR_13] = {SIGSEGV, SI_KERNEL,  false, "#GP", "general protection fault"     },
    [ISR_16] = {SIGFPE,  0,          true,  "#MF", "x87 floating-point exception" },
    [ISR_17] = {SIGBUS,  BUS_ADRALN, false, "#AC", "alignment check"              },
    [ISR_19] = {SIGFPE,  0,          true,  "#XM", "SIMD floating-point exception"},
};

/* si_code for #DB, from the DR6 bits that name the breakpoint which fired; the B0-B3 bits stay set, so single step is tested first. */
static int exception_debug_code(void)
{
    uint64_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    dr6 ^= DR6_RESERVED; // drop the reserved bits so an untouched DR6 reads as zero

    if (dr6 & 0x4000) return TRAP_TRACE;  // BS: single step
    if (dr6 & 0x000f) return TRAP_HWBKPT; // B0-B3: hardware breakpoint
    return TRAP_BRKPT;                    // ICEBP
}

/* Fill `out` for a fixed exception vector, completing the si_codes that come from hardware state.  An unknown vector leaves `out` at its defaults. */
static void fixed_exception_signal(uint32_t vector, exception_signal_t *out)
{
    if (vector >= sizeof(exception_signals) / sizeof(exception_signals[0])) return;
    *out = exception_signals[vector];

    if (vector == ISR_1) {
        out->code = exception_debug_code();
    } else if (vector == ISR_16 || vector == ISR_19) {
        out->code = fpu_exception_code(vector);
        if (out->code == 0) out->signal = 0; // spurious floating-point exception
    }
}

/* Fixed exception handle frame. */
__attribute__((used)) void fixed_exception_handle_frame(exception_frame_t *frame, uint32_t vector)
{
    exception_signal_t sig = {.name = "#??", .message = "unknown exception"};

    disable_intr();
    fixed_exception_signal(vector, &sig);
    if (!user_mode(frame)) {
        carry_error_code = vector == ISR_10 || vector == ISR_11 || vector == ISR_12 || vector == ISR_13 || vector == ISR_17;
        panic("Kernel exception: %s", sig.name);
    }
    if (!sig.signal) return; // no signal to deliver

    /* INT3 is the trap whose RIP has already advanced; the rest may re-execute. */
    exception_deliver_signal(frame, sig.signal, sig.code, sig.use_ip ? frame->rip : 0, sig.message, vector == ISR_3);
}

/* Copy an exception frame into the signal-delivery frame, leaving error_code alone. */
static void exception_frame_to_syscall(syscall_frame_t *dst, const exception_frame_t *src)
{
    memcpy(dst, src, offsetof(syscall_frame_t, rip));
    dst->rip    = src->rip;
    dst->cs     = src->cs;
    dst->rflags = src->rflags;
    dst->rsp    = src->rsp;
    dst->ss     = src->ss;
}

/* Write a delivered signal frame back into the exception frame. */
static void syscall_frame_to_exception(exception_frame_t *dst, const syscall_frame_t *src)
{
    memcpy(dst, src, offsetof(syscall_frame_t, rip));
    dst->rip    = src->rip;
    dst->cs     = src->cs;
    dst->rflags = src->rflags;
    dst->rsp    = src->rsp;
    dst->ss     = src->ss;
}

/* Deliver a synchronous signal for a user-mode exception.  The console line is rate limited and emitted only when the signal has no handler. */
void exception_deliver_signal(exception_frame_t *frame, int sig, int code, uintptr_t addr, const char *message, bool trap)
{
    static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);

    process_t *proc = process_current();
    task_t    *task = current_task();
    if (!proc || !task) panic("Exception %s with no process context", message);

    siginfo_t info = {0};
    info.si_signo  = sig;
    info.si_code   = code;
    info.si_addr   = (void *)addr;

    /* A blocked or ignored signal would re-execute the fault forever; forcing delivery keeps an installed handler reachable, and a trap has already advanced RIP. */
    if (!trap) signal_force_delivery(proc, sig);
    if (signal_is_unhandled(proc, sig) && ratelimit_allow(&ratelimit))
        plogk("%s[%llu]: %s at %p ip %p sp %p error 0x%llx\n", task->name, task->pid, message, (void *)addr, (void *)frame->rip, (void *)frame->rsp, frame->error_code);

    (void)signal_send_thread(task, sig, &info);

    syscall_frame_t sigframe;
    exception_frame_to_syscall(&sigframe, frame);
    if (signal_deliver_if_pending(&sigframe) == 1) task_exit();
    syscall_frame_to_exception(frame, &sigframe);

    sched_maybe_preempt();
}

/*
 * Every signal-capable exception uses one explicit full-register frame.
 * For vectors without a CPU error code the stub first pushes a synthetic
 * zero, making the C layout and restore path identical for all entries.
 */
__asm__(".text\n"
        ".macro EXCEPTION_ENTRY name, vector, has_error, chandler\n"
        ".global \\name\n"
        ".type \\name, @function\n"
        "\\name:\n"
        ".if \\has_error == 0\n"
        "pushq $0\n"
        ".endif\n"
        "cld\n"
        "testb $3, 16(%rsp)\n"
        "jz 991f\n"
        "swapgs\n"
        "991:\n"
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
        "movl $\\vector, %esi\n"
        "andq $-16, %rsp\n"
        "call \\chandler\n"
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
        "addq $8, %rsp\n"
        "testb $3, 8(%rsp)\n"
        "jz 992f\n"
        "cli\n"
        "swapgs\n"
        "992:\n"
        "iretq\n"
        ".size \\name, .-\\name\n"
        ".endm\n"
        "EXCEPTION_ENTRY exception_0_entry, 0, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_1_entry, 1, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_3_entry, 3, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_4_entry, 4, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_5_entry, 5, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_6_entry, 6, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_7_entry, 7, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_10_entry, 10, 1, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_11_entry, 11, 1, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_12_entry, 12, 1, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_13_entry, 13, 1, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_16_entry, 16, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_17_entry, 17, 1, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY exception_19_entry, 19, 0, fixed_exception_handle_frame\n"
        "EXCEPTION_ENTRY page_fault_entry, 14, 1, page_fault_handle_frame\n"

        /* #CP (21), #VC (29) and #SX (30) push an error code; the stub synthesises a zero for the rest. */
        "EXCEPTION_ENTRY reserved_15_entry, 15, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_20_entry, 20, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_21_entry, 21, 1, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_22_entry, 22, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_23_entry, 23, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_24_entry, 24, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_25_entry, 25, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_26_entry, 26, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_27_entry, 27, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_28_entry, 28, 0, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_29_entry, 29, 1, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_30_entry, 30, 1, exception_reserved_panic\n"
        "EXCEPTION_ENTRY reserved_31_entry, 31, 0, exception_reserved_panic\n");

/* An interrupt from user mode runs with the user's GS; swap to the per-CPU base on entry and back before returning (frame->cs tells which case). */
void irq_enter_gs(interrupt_frame_t *frame)
{
    if (user_mode(frame)) __asm__ volatile("swapgs" ::: "memory");
}

/* Leave: cli closes the scheduling/swapgs->iretq window; iretq restores the saved IF. */
void irq_leave_gs(interrupt_frame_t *frame)
{
    if (user_mode(frame)) {
        /*
         * Device IRQs can wake a task on this CPU without sending an IPI.
         * Honor that wake before restoring userspace so an input/compositor
         * waiter is not stranded until the next periodic timer interrupt.
         */
        disable_intr();
        sched_maybe_preempt();
        __asm__ volatile("swapgs" ::: "memory");
    }
}

/* Leave an NMI/other non-preemptible entry without entering the scheduler. */
void irq_leave_gs_no_preempt(interrupt_frame_t *frame)
{
    if (user_mode(frame)) __asm__ volatile("swapgs" ::: "memory");
}

/* Non-maskable interrupt (#NMI) */
INTERRUPT_BEGIN static void ISR_2_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);

    /* NMI handlers must not be interrupted by ordinary IRQs. */
    disable_intr();
    if (smp_handle_nmi()) {
        irq_leave_gs_no_preempt(frame);
        return;
    }

    /* Unknown hardware NMI: parked for the timer tick to drain, since plogk() cannot run here. */
    (void)__atomic_add_fetch(&nmi_spurious_count, 1, __ATOMIC_RELAXED);
    char msg[NMI_LOG_MSG_SIZE];
    int  n = snprintf(msg, sizeof(msg), "NMI received for unknown reason on CPU %u (RIP %p)\n", get_current_cpu_id(), (void *)frame->rip);
    if (n > 0) nmi_log_message(msg, (size_t)n);
    irq_leave_gs_no_preempt(frame);
}
INTERRUPT_END

/* Double fault (#DF) */
INTERRUPT_BEGIN __attribute__((noreturn)) static void ISR_8_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    irq_enter_gs(frame);
    (void)frame;
    (void)error_code;
    carry_error_code = 1;
    panic("Kernel exception: #DF");
}
INTERRUPT_END

/* Coprocessor segment overrun */
INTERRUPT_BEGIN __attribute__((noreturn)) static void ISR_9_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    panic("Kernel exception: Coprocessor Segment Overrun");
}
INTERRUPT_END

/* Machine check (#MC) */
INTERRUPT_BEGIN __attribute__((noreturn)) static void ISR_18_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    panic("Kernel exception: #MC");
}
INTERRUPT_END

/* Reserved-vector entry: no recovery path, and the frame is never inspected. */
__attribute__((used, noreturn)) void exception_reserved_panic(exception_frame_t *frame, uint32_t vector)
{
    (void)frame;
    disable_intr();
    carry_error_code = vector == 21 || vector == 29 || vector == 30; // the reserved vectors that push an error code
    panic("Reserved exception vector %u", vector);
}

/* Register ISR interrupt processing */
void isr_registe_handle(void)
{
    register_interrupt_handler(ISR_0, (void *)exception_0_entry, 0, 0x8e);

    /* #DB stays on IST 0: exception_deliver_signal() may preempt from its user-mode path. */
    register_interrupt_handler(ISR_1, (void *)exception_1_entry, 0, 0x8e);
    register_interrupt_handler(ISR_2, (void *)ISR_2_handle, 2, 0x8e);

    /* User processes may execute INT3; expose the breakpoint gate at DPL=3. */
    register_interrupt_handler(ISR_3, (void *)exception_3_entry, 0, 0xee);
    register_interrupt_handler(ISR_4, (void *)exception_4_entry, 0, 0x8e);
    register_interrupt_handler(ISR_5, (void *)exception_5_entry, 0, 0x8e);
    register_interrupt_handler(ISR_6, (void *)exception_6_entry, 0, 0x8e);
    register_interrupt_handler(ISR_7, (void *)exception_7_entry, 0, 0x8e);

    /*
     * #DF gets its own IST stack: a double fault by definition fires while
     * delivering another exception, often with the current kernel stack
     * unusable.  Without IST, pushing the #DF frame faults again and the
     * machine triple-faults before any diagnostics are printed.
     */

    register_interrupt_handler(ISR_8, (void *)ISR_8_handle, 1, 0x8e);
    register_interrupt_handler(ISR_9, (void *)ISR_9_handle, 0, 0x8e);
    register_interrupt_handler(ISR_10, (void *)exception_10_entry, 0, 0x8e);
    register_interrupt_handler(ISR_11, (void *)exception_11_entry, 0, 0x8e);
    register_interrupt_handler(ISR_12, (void *)exception_12_entry, 0, 0x8e);
    register_interrupt_handler(ISR_13, (void *)exception_13_entry, 0, 0x8e);

    /*
     * #PF stays on IST 0: exception_deliver_signal() calls
     * sched_maybe_preempt(), which must not switch tasks while running on an
     * IST stack.
     */
    register_interrupt_handler(ISR_14, (void *)page_fault_entry, 0, 0x8e);
    register_interrupt_handler(ISR_15, (void *)reserved_15_entry, 0, 0x8e);

    register_interrupt_handler(ISR_16, (void *)exception_16_entry, 0, 0x8e);
    register_interrupt_handler(ISR_17, (void *)exception_17_entry, 0, 0x8e);

    /* #MC stays on IST 0: the handler only panics, and #DF already has its own stack. */
    register_interrupt_handler(ISR_18, (void *)ISR_18_handle, 0, 0x8e);
    register_interrupt_handler(ISR_19, (void *)exception_19_entry, 0, 0x8e);

    /* Vectors 20-31 are reserved by the CPU: entering one is unrecoverable. */
    register_interrupt_handler(20, (void *)reserved_20_entry, 0, 0x8e);
    register_interrupt_handler(21, (void *)reserved_21_entry, 0, 0x8e);
    register_interrupt_handler(22, (void *)reserved_22_entry, 0, 0x8e);
    register_interrupt_handler(23, (void *)reserved_23_entry, 0, 0x8e);
    register_interrupt_handler(24, (void *)reserved_24_entry, 0, 0x8e);
    register_interrupt_handler(25, (void *)reserved_25_entry, 0, 0x8e);
    register_interrupt_handler(26, (void *)reserved_26_entry, 0, 0x8e);
    register_interrupt_handler(27, (void *)reserved_27_entry, 0, 0x8e);
    register_interrupt_handler(28, (void *)reserved_28_entry, 0, 0x8e);
    register_interrupt_handler(29, (void *)reserved_29_entry, 0, 0x8e);
    register_interrupt_handler(30, (void *)reserved_30_entry, 0, 0x8e);
    register_interrupt_handler(31, (void *)reserved_31_entry, 0, 0x8e);

    plogk("isr: All ISR handlers are registered.\n");
}
