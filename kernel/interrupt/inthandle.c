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
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <syscall/syscall.h>

void     page_fault_entry(void);
void     exception_0_entry(void);
void     exception_1_entry(void);
void     exception_3_entry(void);
void     exception_4_entry(void);
void     exception_5_entry(void);
void     exception_6_entry(void);
void     exception_7_entry(void);
void     exception_10_entry(void);
void     exception_11_entry(void);
void     exception_12_entry(void);
void     exception_13_entry(void);
void     exception_16_entry(void);
void     exception_17_entry(void);
void     exception_19_entry(void);
uint64_t nmi_spurious_count;

_Static_assert(offsetof(syscall_frame_t, rip) == 15 * sizeof(uint64_t), "bad fixed exception GPR layout");
_Static_assert(offsetof(syscall_frame_t, cs) == 16 * sizeof(uint64_t), "bad fixed exception iret layout");

typedef struct exception_error_frame {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
        uint64_t error_code;
        uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) exception_error_frame_t;

_Static_assert(offsetof(exception_error_frame_t, error_code) == 15 * sizeof(uint64_t), "bad fixed error-code layout");
_Static_assert(offsetof(exception_error_frame_t, rip) == 16 * sizeof(uint64_t), "bad fixed error-code iret layout");

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

static bool fixed_exception_signal(uint32_t vector, int *signal, int *code, const char **name, const char **message)
{
    switch (vector) {
        case 0 : *signal = SIGFPE; *code = FPE_INTDIV; *name = "#DE"; *message = "divide error"; return true;
        case 1 : *signal = SIGTRAP; *code = TRAP_TRACE; *name = "#DB"; *message = "debug trap"; return true;
        case 3 : *signal = SIGTRAP; *code = TRAP_BRKPT; *name = "#BP"; *message = "breakpoint"; return true;
        case 4 : *signal = SIGFPE; *code = FPE_INTOVF; *name = "#OF"; *message = "integer overflow"; return true;
        case 5 : *signal = SIGSEGV; *code = SEGV_ACCERR; *name = "#BR"; *message = "bound range exceeded"; return true;
        case 6 : *signal = SIGILL; *code = ILL_ILLOPC; *name = "#UD"; *message = "invalid opcode"; return true;
        case 7 : *signal = SIGILL; *code = ILL_COPROC; *name = "#NM"; *message = "device not available"; return true;
        case 10 : *signal = SIGSEGV; *code = SEGV_ACCERR; *name = "#TS"; *message = "invalid TSS"; return true;
        case 11 : *signal = SIGSEGV; *code = SEGV_ACCERR; *name = "#NP"; *message = "segment not present"; return true;
        case 12 : *signal = SIGSEGV; *code = SEGV_ACCERR; *name = "#SS"; *message = "stack-segment fault"; return true;
        case 13 : *signal = SIGSEGV; *code = SEGV_ACCERR; *name = "#GP"; *message = "general protection fault"; return true;
        case 16 : *signal = SIGFPE; *code = FPE_FLTINV; *name = "#MF"; *message = "x87 floating-point exception"; return true;
        case 17 : *signal = SIGBUS; *code = BUS_ADRALN; *name = "#AC"; *message = "alignment check"; return true;
        case 19 : *signal = SIGFPE; *code = FPE_FLTINV; *name = "#XM"; *message = "SIMD floating-point exception"; return true;
        default : return false;
    }
}

void fixed_exception_handle_frame(exception_error_frame_t *frame, uint32_t vector) __attribute__((used, noinline));

void fixed_exception_handle_frame(exception_error_frame_t *frame, uint32_t vector)
{
    int         signal = SIGSEGV;
    int         code = SEGV_ACCERR;
    const char *name = "#??";
    const char *message = "unknown exception";

    disable_intr();
    (void)fixed_exception_signal(vector, &signal, &code, &name, &message);
    if ((frame->cs & 3U) != 3U) {
        carry_error_code = vector == 10 || vector == 11 || vector == 12 || vector == 13 || vector == 17;
        panic("Kernel exception: %s vector=%u rip=%p cs=0x%llx error=0x%llx", name, vector, (void *)frame->rip, (unsigned long long)frame->cs, (unsigned long long)frame->error_code);
    }

    process_t *proc = process_current();
    task_t    *task = current_task();
    if (!proc || !task) task_exit();

    if (vector == 13) {
        interrupt_frame_t report = {
            .rip = frame->rip, .cs = frame->cs, .rflags = frame->rflags, .rsp = frame->rsp, .ss = frame->ss,
        };
        user_gp_report(&report, frame->error_code);
    } else {
        plogk("[exception] %s pid=%llu rip=%p: %s\n", name, (unsigned long long)task->pid, (void *)frame->rip, message);
    }

    siginfo_t info = {0};
    info.si_signo  = signal;
    info.si_code   = code;
    info.si_addr   = (void *)frame->rip;

    /*
     * Returning to a faulting instruction with its synchronous signal
     * blocked/ignored spins forever in the exception path.  INT3 is a trap
     * and has already advanced RIP, so it alone may safely remain blocked.
     */
    if (vector != 3 && signal_is_blocked_or_ignored(proc, signal)) process_exit_group(-signal);
    signal_send_thread(task, signal, &info);

    syscall_frame_t sigframe;
    memcpy(&sigframe, frame, offsetof(syscall_frame_t, rip));
    sigframe.rip    = frame->rip;
    sigframe.cs     = frame->cs;
    sigframe.rflags = frame->rflags;
    sigframe.rsp    = frame->rsp;
    sigframe.ss     = frame->ss;
    if (signal_deliver_if_pending(&sigframe) == 1) task_exit();

    memcpy(frame, &sigframe, offsetof(syscall_frame_t, rip));
    frame->rip    = sigframe.rip;
    frame->cs     = sigframe.cs;
    frame->rflags = sigframe.rflags;
    frame->rsp    = sigframe.rsp;
    frame->ss     = sigframe.ss;
    sched_maybe_preempt();
}

/*
 * Every signal-capable exception uses one explicit full-register frame.
 * For vectors without a CPU error code the stub first pushes a synthetic
 * zero, making the C layout and restore path identical for all entries.
 */
__asm__(".text\n"
        ".macro FIXED_EXCEPTION name, vector, has_error\n"
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
        "call fixed_exception_handle_frame\n"
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
        "FIXED_EXCEPTION exception_0_entry, 0, 0\n"
        "FIXED_EXCEPTION exception_1_entry, 1, 0\n"
        "FIXED_EXCEPTION exception_3_entry, 3, 0\n"
        "FIXED_EXCEPTION exception_4_entry, 4, 0\n"
        "FIXED_EXCEPTION exception_5_entry, 5, 0\n"
        "FIXED_EXCEPTION exception_6_entry, 6, 0\n"
        "FIXED_EXCEPTION exception_7_entry, 7, 0\n"
        "FIXED_EXCEPTION exception_10_entry, 10, 1\n"
        "FIXED_EXCEPTION exception_11_entry, 11, 1\n"
        "FIXED_EXCEPTION exception_12_entry, 12, 1\n"
        "FIXED_EXCEPTION exception_13_entry, 13, 1\n"
        "FIXED_EXCEPTION exception_16_entry, 16, 0\n"
        "FIXED_EXCEPTION exception_17_entry, 17, 1\n"
        "FIXED_EXCEPTION exception_19_entry, 19, 0\n");

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

/* ISR 14 (#PF) is handled by the paging subsystem */

/* Machine check (#MC) */
INTERRUPT_BEGIN static void ISR_18_handle(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    panic("Kernel exception: #MC");
}
INTERRUPT_END

/* Register ISR interrupt processing */
void isr_registe_handle(void)
{
    register_interrupt_handler(ISR_0, (void *)exception_0_entry, 0, 0x8e);
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
    register_interrupt_handler(ISR_14, (void *)page_fault_entry, 0, 0x8e);

    /* ISR 15 CPU reserved */

    register_interrupt_handler(ISR_16, (void *)exception_16_entry, 0, 0x8e);
    register_interrupt_handler(ISR_17, (void *)exception_17_entry, 0, 0x8e);
    register_interrupt_handler(ISR_18, (void *)ISR_18_handle, 0, 0x8e);
    register_interrupt_handler(ISR_19, (void *)exception_19_entry, 0, 0x8e);

    plogk("isr: All ISR handlers are registered.\n");
}
