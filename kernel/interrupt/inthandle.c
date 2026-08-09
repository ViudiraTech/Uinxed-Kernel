/*
 *
 *      inthandle.c
 *      Interrupt handler
 *
 *      2024/8/1 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smp.h>
#include <kernel/debug.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <syscall/syscall.h>

void page_fault_entry(void);

#define USER_CS 0x1B

static inline int is_user_mode(interrupt_frame_t *frame)
{
    return (frame->cs & 3) == 3;
}

static inline int user_exception(interrupt_frame_t *frame, int sig, int code, const char *name, const char *msg)
{
    if (is_user_mode(frame)) {
        process_t *proc = process_current();
        if (proc) {
            siginfo_t info = {0};
            info.si_signo  = sig;
            info.si_code   = code;
            info.si_addr   = (void *)frame->rip;

            if (msg) plogk("%s (pid=%llu): %s\n", name, proc->task->pid, msg);
            signal_send_thread(proc->task, sig, &info);

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
    while (stack_len < 4 && !copy_from_user(&stack[stack_len], (const void *)(frame->rsp + stack_len * sizeof(uint64_t)), sizeof(uint64_t)))
        stack_len++;

    plogk("[exception] #GP pid=%llu tgid=%llu comm=%s process=%s\n", (unsigned long long)task->pid, (unsigned long long)task->tgid, task->name,
          proc->name);
    plogk("[exception] exe=%s rip=%p rsp=%p error=0x%llx cs=0x%llx ss=0x%llx rflags=0x%llx\n", proc->exe_path[0] ? proc->exe_path : "[unknown]",
          (void *)frame->rip, (void *)frame->rsp, (unsigned long long)error_code, (unsigned long long)frame->cs, (unsigned long long)frame->ss,
          (unsigned long long)frame->rflags);
    plogk("[exception] gp-source external=%u table=%u selector-index=%llu fsbase=%p fpu-init=%u fpu-active=%u\n", (unsigned)(error_code & 1U),
          (unsigned)((error_code >> 1) & 3U), (unsigned long long)(error_code >> 3), (void *)task->thread.fs_base,
          (unsigned)task->thread.fpu_initialized, (unsigned)task->thread.fpu_active);
    if (map_start) {
        plogk("[exception] vma=%p-%p %c%c%c%c type=%d file=%s file-offset=0x%llx\n", (void *)map_start, (void *)map_end,
              (map_flags & VM_READ) ? 'r' : '-', (map_flags & VM_WRITE) ? 'w' : '-', (map_flags & VM_EXEC) ? 'x' : '-',
              (map_flags & VM_SHARED) ? 's' : 'p', map_type, map_name, (unsigned long long)file_off);
    } else {
        plogk("[exception] rip-vma=[unmapped]\n");
    }
    plogk("[exception] code[%zu]=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", code_len, code[0], code[1],
          code[2], code[3], code[4], code[5], code[6], code[7], code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);
    plogk("[exception] stack[%zu]=%p %p %p %p\n", stack_len, (void *)stack[0], (void *)stack[1], (void *)stack[2], (void *)stack[3]);
}

INTERRUPT_BEGIN static void ISR_0_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGFPE, FPE_INTDIV, "#DE", "Floating point exception")) return;
    panic("Kernel exception: #DE");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_1_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #DB");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_2_handle(interrupt_frame_t *frame)
{
    (void)frame;
    if (smp_handle_nmi()) return;
    panic("Kernel fatal error: NMI");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_3_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGTRAP, TRAP_BRKPT, "#BP", "Breakpoint trap")) return;
    panic("Kernel breakpoint exception: BP");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_4_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGFPE, FPE_INTOVF, "#OF", "Integer overflow")) return;
    panic("Kernel exception: #OF");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_5_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGSEGV, SEGV_ACCERR, "#BR", "Bound range exceeded")) return;
    panic("Kernel exception: #BR");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_6_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGILL, ILL_ILLOPC, "#UD", "Invalid opcode")) return;
    panic("Kernel exception: #UD");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_7_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGFPE, FPE_FLTINV, "#NM", "Device not available")) return;
    panic("Kernel exception: #NM");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_8_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    (void)frame;
    (void)error_code;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #DF");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_9_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: Coprocessor Segment Overrun");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_10_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    (void)frame;
    (void)error_code;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #TS");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_11_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    (void)frame;
    (void)error_code;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #NP");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_12_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    (void)frame;
    (void)error_code;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #SS");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_13_handle(interrupt_frame_t *frame, uint64_t error_code)
{
    carry_error_code = 1; // carry error code
    if (is_user_mode(frame)) user_gp_report(frame, error_code);
    if (user_exception(frame, SIGSEGV, SEGV_ACCERR, "#GP", NULL)) return;
    panic("Kernel exception: #GP rip=%p cs=0x%llx error=0x%llx", (void *)frame->rip, frame->cs, error_code);
}
INTERRUPT_END

/* ISR 14 will be define by pagine program */

/* ISR 15 CPU reserved */

INTERRUPT_BEGIN static void ISR_16_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGFPE, FPE_FLTINV, "#MF", "x87 FPU error")) return;
    panic("Kernel exception: #MF");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_17_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGBUS, BUS_ADRALN, "#AC", "Alignment check")) return;
    panic("Kernel exception: #AC");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_18_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #MC");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_19_handle(interrupt_frame_t *frame)
{
    if (user_exception(frame, SIGFPE, FPE_FLTINV, "#XM", "SIMD floating point exception")) return;
    panic("Kernel exception: #XM");
}
INTERRUPT_END

/* Register ISR interrupt processing */
void isr_registe_handle(void)
{
    register_interrupt_handler(ISR_0, (void *)ISR_0_handle, 0, 0x8e);
    register_interrupt_handler(ISR_1, (void *)ISR_1_handle, 0, 0x8e);
    register_interrupt_handler(ISR_2, (void *)ISR_2_handle, 0, 0x8e);
    register_interrupt_handler(ISR_3, (void *)ISR_3_handle, 0, 0x8e);
    register_interrupt_handler(ISR_4, (void *)ISR_4_handle, 0, 0x8e);
    register_interrupt_handler(ISR_5, (void *)ISR_5_handle, 0, 0x8e);
    register_interrupt_handler(ISR_6, (void *)ISR_6_handle, 0, 0x8e);
    register_interrupt_handler(ISR_7, (void *)ISR_7_handle, 0, 0x8e);
    register_interrupt_handler(ISR_8, (void *)ISR_8_handle, 0, 0x8e);
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
