/*
 *
 *      debug.c
 *      Kernel debug
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/smbios.h>
#include <arch/smp.h>
#include <boot/limine.h>
#include <drivers/firmware/apic.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/tty/tty.h>
#include <kernel/debug/debug.h>
#include <kernel/debug/symbols.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stdarg.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>

int                 carry_error_code  = 0;
static volatile int panic_in_progress = 0;

/*
 * A frame pointer is trusted only when it lives inside a kernel stack we can
 * name: the current task's stack, its process stack, or this CPU's IST
 * (where #DF handlers run).  Walking beyond that used to follow corrupted
 * links into arbitrary memory and fault inside the panic path itself.
 */
static bool frame_pointer_plausible(uintptr_t fp)
{
    if (fp <= 0x1000) return false;

    task_t *task = current_task();
    if (task) {
        if (task->process && task->process->kernel_stack) {
            uintptr_t base = (uintptr_t)task->process->kernel_stack;
            if (fp >= base && fp < base + PROCESS_KERNEL_STACK) return true;
        }
        if (task->kernel_stack) {
            uintptr_t base = (uintptr_t)task->kernel_stack;
            if (fp >= base && fp < base + TASK_KERNEL_STACK) return true;
        }
    }

    cpu_processor_t *cpu = get_current_cpu();
    if (cpu && cpu->tss_stack) {
        uintptr_t base = (uintptr_t)cpu->tss_stack;
        if (fp >= base && fp < base + sizeof(tss_stack_t)) return true;
    }

    /*
     * Fallback: the current stack grows down, so live frame pointers live in
     * the window above the current RSP.  This covers the early-boot stack
     * (boot_task.kernel_stack is only a single marker byte, not the real
     * stack the boot path runs on) and any stack not described by a
     * task/CPU descriptor - without it, a panic during early boot reports an
     * empty backtrace.
     */
    uintptr_t sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    if (fp >= sp && fp < sp + 0x20000) return true;
    return false;
}

/*
 * Fallback unwinder: when the frame-pointer chain is unusable - most
 * commonly because the panic ran in an interrupt/exception handler that
 * interrupted user mode (RBP is then a user pointer, outside every kernel
 * stack) - scan the current kernel stack for 8-byte values that land in the
 * kernel text window and print them as candidate return addresses.
 */
static void stack_scan(uintptr_t sp)
{
    uintptr_t base = 0, top = 0;
    task_t   *task = current_task();
    if (task) {
        if (task->process && task->process->kernel_stack) {
            base = (uintptr_t)task->process->kernel_stack;
            top  = base + PROCESS_KERNEL_STACK;
        } else if (task->kernel_stack) {
            base = (uintptr_t)task->kernel_stack;
            top  = base + TASK_KERNEL_STACK;
        }
    }
    if (!top) {
        cpu_processor_t *cpu = get_current_cpu();
        if (cpu && cpu->tss_stack) {
            base = (uintptr_t)cpu->tss_stack;
            top  = base + sizeof(tss_stack_t);
        }
    }
    if (!top) {
        /* Early boot / unknown stack: scan the window above the current RSP (see frame_pointer_plausible). */
        base = sp;
        top  = sp + 0x20000;
    }
    if (!top || sp < base || sp >= top) return;

    uintptr_t current_address = kernel_address_request.response->virtual_base;
    uintptr_t text_end        = kernel_text_end(); // real code end from the ELF symtab
    int       printed         = 0;

    plogk("Stack scan (frame pointer chain unavailable):\n");
    for (uintptr_t p = sp; p + 8 <= top && printed < 32; p += 8) {
        uintptr_t val = *(uintptr_t *)p;
        if (val < current_address || val >= text_end) continue;
        sym_info_t sym_info = get_symbol_info(kernel_file_request.response->kernel_file->address, val);
        if (sym_info.name)
            plogk("  [<0x%016zx>] `%s`+0x%lx/0x%lx\n", val, sym_info.name, val - current_address, sym_info.size);
        else
            plogk("  [<0x%016zx>] unknown\n", val);
        printed++;
    }
}

/* Dump stack */
void dump_stack(void)
{
    uintptr_t current_address = kernel_address_request.response->virtual_base;

    typedef union rbp_node {
            uintptr_t       inner;
            union rbp_node *next;
    } rbp_node_t;

    rbp_node_t *rbp;
    uintptr_t   rip;
    __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));

    plogk("Call Trace:\n");
    plogk(" <TASK>\n");

    int frame_count = 0;
    for (int i = 0; i < 16; ++i) {
        if ((uintptr_t)rbp <= 0x1000 || !frame_pointer_plausible((uintptr_t)rbp)) break;
        if (!frame_pointer_plausible((uintptr_t)(rbp + 1))) break;
        if (carry_error_code && frame_count == 3) {
            uintptr_t next = (uintptr_t)rbp->next;
            if (next <= 0x1000 || !frame_pointer_plausible(next)) break;
            rbp = rbp->next;
            ++frame_count;
            continue;
        }

        rip = *(uintptr_t *)(rbp + 1);

        if (!rip) break;
        if (rip >= KERNEL_BASE_ADDRESS) {
            sym_info_t sym_info = get_symbol_info(kernel_file_request.response->kernel_file->address, rip);
            if (sym_info.name) {
                plogk("  [<0x%016zx>] `%s`+0x%lx/0x%lx\n", rip, sym_info.name, rip - current_address, sym_info.size);
            } else {
                plogk("  [<0x%016zx>] %s\n", rip, "unknown");
            }
        } else {
            plogk("  [<0x%016zx>] %s\n", rip, "unknown");
            break;
        }
        {
            uintptr_t next = (uintptr_t)rbp->next;
            if (next <= 0x1000 || next <= (uintptr_t)rbp || !frame_pointer_plausible(next)) break;
            rbp = rbp->next;
        }
        ++frame_count;
    }

    /*
     * The RBP chain is untrustworthy when it yielded almost nothing - panic
     * from an ISR that interrupted user mode is the classic case.  Fall back
     * to a stack scan so the report always contains at least a hint of where
     * the fault happened.
     */
    if (frame_count < 2) {
        uintptr_t sp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
        stack_scan(sp);
    }
    plogk(" </TASK>\n");
}

/* Kernel panic */
void panic(const char *format, ...)
{
    /*
     * Re-entry guard: a panic that faults (or a second CPU panicking on the
     * same console) must not recurse.  The first entrant owns the report;
     * everyone else - including this CPU after a nested fault - parks.
     */
    if (__atomic_exchange_n(&panic_in_progress, 1, __ATOMIC_ACQ_REL)) {
        disable_intr();
        for (;;) __asm__ volatile("hlt");
    }

    /* Freeze the other CPUs so they cannot wander into more faults. */
    disable_intr();
    if (get_cpu_count() > 1) send_ipi_all(IPI_PANIC);

    /*
     * A panic commonly starts in an exception or IRQ frame.  Re-enabling
     * interrupts here allowed a timer/device IRQ to nest into the diagnostic
     * path, recurse through locks or scheduling, and turn the original fault
     * into a misleading #DF/triple fault before it could be printed.
     */

    uint64_t    current_address = kernel_address_request.response->virtual_base;
    const char *sys_vendor      = smbios_sys_manufacturer();
    const char *sys_product     = smbios_sys_product_name();
    const char *bios_version    = smbios_bios_version();
    const char *bios_date       = smbios_bios_release_date();

    static char buff[1024];
    va_list     args;
    int         i;

    va_start(args, format);
    i = vsnprintf(buff, sizeof(buff), format, args);
    va_end(args);

    if (i < 0 || (size_t)i >= sizeof(buff)) buff[sizeof(buff) - 1] = '\0';

    plogk("\n");
    plogk("Kernel panic - not syncing: %s\n", buff);
    task_t     *panic_task = current_task();
    int         panic_pid  = panic_task ? (int)panic_task->pid : -1;
    const char *panic_comm = panic_task ? panic_task->name : "unknown";
    plogk("CPU: %d PID: %d Comm: %s Not tainted.\n", get_current_cpu_id(), panic_pid, panic_comm);
    plogk("Hardware name: %s %s, BIOS %s %s\n", sys_vendor, sys_product, bios_version, bios_date);
    dump_stack();
    plogk("Kernel Offset: 0x%08x from %p\n", current_address - KERNEL_BASE_ADDRESS, KERNEL_BASE_ADDRESS);
    plogk("---[ end Kernel panic - not syncing: %s ]---", buff);

    tty_buff_flush();
    video_flush_now();
    krn_halt();
}

/* Assertion failure */
void assertion_failure(const char *exp, const char *file, int line)
{
    printk("assert(%s) failed!\nfile: %s\nline: %d\n\n", exp, file, line);

    tty_buff_flush();
    video_flush_now();
    krn_halt();
}
