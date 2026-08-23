/*
 *
 *      inthandle.c
 *      Interrupt handler
 *
 *      2024/8/1 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smp.h>
#include <kernel/debug/debug.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <syscall/syscall.h>

void page_fault_entry(void);

uint64_t nmi_spurious_count;

#define USER_CS 0x1B

/* Check whether the interrupt came from user mode */
static inline int is_user_mode(interrupt_frame_t *frame)
{
    return (frame->cs & 3) == 3;
}

/* Deliver a signal for a user-mode exception */
static inline int user_exception(interrupt_frame_t *frame, int sig, int code, const char *name, const char *msg)
{
    if (is_user_mode(frame)) {
        process_t *proc = process_current();
        if (proc) {
            siginfo_t info = {0};
            info.si_signo  = sig;
            info.si_code   = code;
            info.si_addr   = (void *)frame->rip;

            task_t *fault_task = current_task();
            if (msg) plogk("%s (pid=%llu): %s\n", name, fault_task ? fault_task->pid : proc->task->pid, msg);
            signal_send_thread(fault_task ? fault_task : proc->task, sig, &info);

            syscall_frame_t sigframe = {0};
            sigframe.rip             = frame->rip;
            sigframe.cs              = frame->cs;
            sigframe.rflags          = frame->rflags;
            sigframe.rsp             = frame->rsp;
            sigframe.ss              = frame->ss;

            int ret = signal_deliver_if_pending(&sigframe);
            if (ret == 1) task_exit();

            frame->rip    = sigframe.rip;
            frame->rflags = sigframe.rflags;
            frame->rsp    = sigframe.rsp;

            /*
             * Propagate signal number (rdi) and optional siginfo
             * (rsi, rdx) to the saved GPR area below the interrupt
             * frame. The __attribute__((interrupt)) epilogue will
             * restore these values, so the user signal handler
             * receives the correct arguments.
             *
             * GCC's interrupt prologue saves the GPRs in this order
             * below the iret frame (confirmed by disassembly):
             *   r15..r8, rdi(rbp-72), rsi(rbp-80), rbx(rbp-88),
             *   rcx(rbp-96), rdx(rbp-104), rax(rbp-112)
             * with frame == rbp+8, so:
             *   rdi -> frame-80, rsi -> frame-88, rdx -> frame-112
             */
            __asm__ volatile("movq %[rdi], -0x50(%[fp])\n" // saved_rdi = sig
                             "movq %[rsi], -0x58(%[fp])\n" // saved_rsi = siginfo ptr
                             "movq %[rdx], -0x70(%[fp])\n" // saved_rdx = old_mask ptr
                             :
                             : [fp] "r"(frame), [rdi] "r"(sigframe.rdi), [rsi] "r"(sigframe.rsi), [rdx] "r"(sigframe.rdx)
                             : "memory");
        }
        return 1;
    }
    return 0;
}

/*
 * A user #GP usually carries no fault address.  The old one-line report only
 * named the pid, which made an alignment fault in a shared library
 * indistinguishable from a bad selector or a corrupted return frame.  Keep a
 * compact, one-shot crash record with enough information to resolve the RIP
 * against the executable/shared object and inspect the faulting instruction.
 * This is emitted only when a process is about to receive SIGSEGV, so it does
 * not reintroduce the high-volume Weston diagnostics.
 */
static void user_gp_report(interrupt_frame_t *frame, uint64_t error_code)
{
    process_t *proc = process_current();
    task_t    *task = current_task();
    if (!proc || !task || !frame) return;

    uintptr_t  map_start = 0;
    uintptr_t  map_end   = 0;
    uint64_t   file_off  = 0;
    vm_flags_t map_flags = 0;
    int        map_type  = -1;
    char       map_name[VFS_NAME_MAX + 1];
    strcpy(map_name, "[anonymous]");

    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (frame->rip < vma->start || frame->rip >= vma->end) continue;
        map_start = vma->start;
        map_end   = vma->end;
        map_flags = vma->flags;
        map_type  = (int)vma->type;
        file_off  = vma->vm_pgoff * 4096ULL + (frame->rip - vma->start);
        if (vma->vm_file && vma->vm_file->name) {
            strncpy(map_name, vma->vm_file->name, sizeof(map_name) - 1);
            map_name[sizeof(map_name) - 1] = '\0';
        }
        break;
    }
    spin_unlock(&proc->mmap_lock);

    uint8_t code[16] = {0};
    size_t  code_len = 0;
    while (code_len < sizeof(code) && !copy_from_user(&code[code_len], (const void *)(frame->rip + code_len), 1)) code_len++;

    uint64_t stack[4]  = {0};
    size_t   stack_len = 0;
    while (stack_len < 4 && !copy_from_user(&stack[stack_len], (const void *)(frame->rsp + stack_len * sizeof(uint64_t)), sizeof(uint64_t))) stack_len++;

    plogk("[exception] #GP pid=%llu tgid=%llu comm=%s process=%s\n", (unsigned long long)task->pid, (unsigned long long)task->tgid, task->name, proc->name);
    plogk("[exception] exe=%s rip=%p rsp=%p error=0x%llx cs=0x%llx ss=0x%llx rflags=0x%llx\n", proc->exe_path[0] ? proc->exe_path : "[unknown]", (void *)frame->rip, (void *)frame->rsp,
          (unsigned long long)error_code, (unsigned long long)frame->cs, (unsigned long long)frame->ss, (unsigned long long)frame->rflags);
    plogk("[exception] gp-source external=%u table=%u selector-index=%llu fsbase=%p fpu-init=%u fpu-active=%u\n", (unsigned)(error_code & 1U), (unsigned)((error_code >> 1) & 3U),
          (unsigned long long)(error_code >> 3), (void *)task->thread.fs_base, (unsigned)task->thread.fpu_initialized, (unsigned)task->thread.fpu_active);
    if (map_start) {
        plogk("[exception] vma=%p-%p %c%c%c%c type=%d file=%s file-offset=0x%llx\n", (void *)map_start, (void *)map_end, (map_flags & VM_READ) ? 'r' : '-', (map_flags & VM_WRITE) ? 'w' : '-',
              (map_flags & VM_EXEC) ? 'x' : '-', (map_flags & VM_SHARED) ? 's' : 'p', map_type, map_name, (unsigned long long)file_off);
    } else {
        plogk("[exception] rip-vma=[unmapped]\n");
    }
    plogk("[exception] code[%zu]=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", code_len, code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
          code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);
    plogk("[exception] stack[%zu]=%p %p %p %p\n", stack_len, (void *)stack[0], (void *)stack[1], (void *)stack[2], (void *)stack[3]);
}

/* Divide error (#DE) */
INTERRUPT_BEGIN static void ISR_0_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGFPE, FPE_INTDIV, "#DE", "Floating point exception")) panic("Kernel exception: #DE");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Debug exception (#DB) */
INTERRUPT_BEGIN static void ISR_1_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    panic("Kernel exception: #DB");
}
INTERRUPT_END

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

    /*
     * Not a TLB-shootdown NMI: an unknown hardware NMI (LINT1 assertion,
     * NMI button, watchdog).  Mirror Linux's default_do_nmi() - log it and
     * continue, a real NMI must not be fatal.  plogk() cannot run here (it
     * would spin forever against a lock the interrupted context holds), so
     * park the message and let the timer tick drain it through the normal
     * log, reaching every enabled console just like any other message.
     * Also count it, visible through /proc/interrupts.
     */
    (void)__atomic_add_fetch(&nmi_spurious_count, 1, __ATOMIC_RELAXED);
    char msg[NMI_LOG_MSG_SIZE];
    int  n = snprintf(msg, sizeof(msg), "NMI received for unknown reason on CPU %u (RIP %p)\n", get_current_cpu_id(), (void *)frame->rip);
    if (n > 0) nmi_log_message(msg, (size_t)n);
    irq_leave_gs_no_preempt(frame);
}
INTERRUPT_END

/* Breakpoint (#BP) */
INTERRUPT_BEGIN static void ISR_3_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGTRAP, TRAP_BRKPT, "#BP", "Breakpoint trap")) panic("Kernel breakpoint exception: BP");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Overflow (#OF) */
INTERRUPT_BEGIN static void ISR_4_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGFPE, FPE_INTOVF, "#OF", "Integer overflow")) panic("Kernel exception: #OF");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Bound range exceeded (#BR) */
INTERRUPT_BEGIN static void ISR_5_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGSEGV, SEGV_ACCERR, "#BR", "Bound range exceeded")) panic("Kernel exception: #BR");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Invalid opcode (#UD) */
INTERRUPT_BEGIN static void ISR_6_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGILL, ILL_ILLOPC, "#UD", "Invalid opcode")) panic("Kernel exception: #UD");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Device not available (#NM) */
INTERRUPT_BEGIN static void ISR_7_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGFPE, FPE_FLTINV, "#NM", "Device not available")) panic("Kernel exception: #NM");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Double fault (#DF) */
INTERRUPT_BEGIN static void ISR_8_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    irq_enter_gs(frame);
    (void)frame;
    (void)error_code;
    carry_error_code = 1;
    panic("Kernel exception: #DF");
}
INTERRUPT_END

/* Coprocessor segment overrun */
INTERRUPT_BEGIN static void ISR_9_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    panic("Kernel exception: Coprocessor Segment Overrun");
}
INTERRUPT_END

/* Invalid TSS (#TS) */
INTERRUPT_BEGIN static void ISR_10_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    irq_enter_gs(frame);
    (void)frame;
    (void)error_code;
    carry_error_code = 1;
    panic("Kernel exception: #TS");
}
INTERRUPT_END

/* Segment not present (#NP) */
INTERRUPT_BEGIN static void ISR_11_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    irq_enter_gs(frame);
    (void)frame;
    (void)error_code;
    carry_error_code = 1;
    panic("Kernel exception: #NP");
}
INTERRUPT_END

/* Stack segment fault (#SS) */
INTERRUPT_BEGIN static void ISR_12_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    irq_enter_gs(frame);
    (void)frame;
    (void)error_code;
    carry_error_code = 1;
    panic("Kernel exception: #SS");
}
INTERRUPT_END

/* General protection fault (#GP) */
INTERRUPT_BEGIN static void ISR_13_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    irq_enter_gs(frame);
    carry_error_code = 1;
    if (is_user_mode(frame)) user_gp_report(frame, error_code);
    if (!user_exception(frame, SIGSEGV, SEGV_ACCERR, "#GP", NULL)) panic("Kernel exception: #GP rip=%p cs=0x%llx error=0x%llx", (void *)frame->rip, frame->cs, error_code);
    irq_leave_gs(frame);
}
INTERRUPT_END

/* ISR 14 (#PF) is handled by the paging subsystem */

/* x87 floating point error (#MF) */
INTERRUPT_BEGIN static void ISR_16_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGFPE, FPE_FLTINV, "#MF", "x87 FPU error")) panic("Kernel exception: #MF");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Alignment check (#AC) */
INTERRUPT_BEGIN static void ISR_17_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    irq_enter_gs(frame);
    (void)error_code;
    if (!user_exception(frame, SIGBUS, BUS_ADRALN, "#AC", "Alignment check")) panic("Kernel exception: #AC");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Machine check (#MC) */
INTERRUPT_BEGIN static void ISR_18_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    panic("Kernel exception: #MC");
}
INTERRUPT_END

/* SIMD floating point exception (#XM) */
INTERRUPT_BEGIN static void ISR_19_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    if (!user_exception(frame, SIGFPE, FPE_FLTINV, "#XM", "SIMD floating point exception")) panic("Kernel exception: #XM");
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Register ISR interrupt processing */
void isr_registe_handle(void)
{
    register_interrupt_handler(ISR_0, (void *)ISR_0_handle, 0, 0x8e);
    register_interrupt_handler(ISR_1, (void *)ISR_1_handle, 0, 0x8e);
    register_interrupt_handler(ISR_2, (void *)ISR_2_handle, 2, 0x8e);

    /* User processes may execute INT3; expose the breakpoint gate at DPL=3. */

    register_interrupt_handler(ISR_3, (void *)ISR_3_handle, 0, 0xee);
    register_interrupt_handler(ISR_4, (void *)ISR_4_handle, 0, 0x8e);
    register_interrupt_handler(ISR_5, (void *)ISR_5_handle, 0, 0x8e);
    register_interrupt_handler(ISR_6, (void *)ISR_6_handle, 0, 0x8e);
    register_interrupt_handler(ISR_7, (void *)ISR_7_handle, 0, 0x8e);

    /*
     * #DF gets its own IST stack: a double fault by definition fires while
     * delivering another exception, often with the current kernel stack
     * unusable.  Without IST, pushing the #DF frame faults again and the
     * machine triple-faults before any diagnostics are printed.
     */

    register_interrupt_handler(ISR_8, (void *)ISR_8_handle, 1, 0x8e);
    register_interrupt_handler(ISR_9, (void *)ISR_9_handle, 0, 0x8e);
    register_interrupt_handler(ISR_10, (void *)ISR_10_handle, 0, 0x8e);
    register_interrupt_handler(ISR_11, (void *)ISR_11_handle, 0, 0x8e);
    register_interrupt_handler(ISR_12, (void *)ISR_12_handle, 0, 0x8e);
    register_interrupt_handler(ISR_13, (void *)ISR_13_handle, 0, 0x8e);
    register_interrupt_handler(ISR_14, (void *)page_fault_entry, 0, 0x8e);

    /* ISR 15 CPU reserved */

    register_interrupt_handler(ISR_16, (void *)ISR_16_handle, 0, 0x8e);
    register_interrupt_handler(ISR_17, (void *)ISR_17_handle, 0, 0x8e);
    register_interrupt_handler(ISR_18, (void *)ISR_18_handle, 0, 0x8e);
    register_interrupt_handler(ISR_19, (void *)ISR_19_handle, 0, 0x8e);

    plogk("isr: All ISR handlers are registered.\n");
}
