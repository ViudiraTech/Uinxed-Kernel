/*
 *
 *      ptrace.c
 *      Linux-compatible process tracing
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <process/process.h>
#include <process/ptrace.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <syscall/syscall.h>

#define AUDIT_ARCH_X86_64   0xc000003eU
#define PTRACE_WAIT_WNOHANG 0x00000001

typedef struct ptrace_user_area {
        ptrace_user_regs_t regs;
        int32_t            fp_valid;
        uint32_t           fp_pad;
        uint8_t            fpregs[512];
        uint64_t           text_pages;
        uint64_t           data_pages;
        uint64_t           stack_pages;
        uint64_t           start_code;
        uint64_t           start_stack;
        int64_t            signal;
        uint32_t           reserved;
        uint32_t           reserved_pad;
        uint64_t           regs_pointer;
        uint64_t           fpregs_pointer;
        uint64_t           magic;
        char               command[32];
        uint64_t           debug_regs[8];
} ptrace_user_area_t;

_Static_assert(offsetof(ptrace_user_area_t, debug_regs) == PTRACE_USER_DEBUGREG_OFFSET, "Linux x86-64 struct user debug-register offset");
_Static_assert(sizeof(ptrace_user_area_t) == PTRACE_USER_AREA_SIZE, "Linux x86-64 struct user size");

static task_t *ptrace_find_task_get(int64_t pid, process_t **owner)
{
    if (owner) *owner = NULL;
    if (pid <= 0) return NULL;

    return process_task_find_get((pid_t)pid, owner);
}

void ptrace_state_init(ptrace_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->mode = PTRACE_RUN_CONT;
}

void ptrace_regs_from_frame(ptrace_user_regs_t *regs, const syscall_frame_t *frame, uint64_t orig_rax)
{
    if (!regs || !frame) return;
    memset(regs, 0, sizeof(*regs));
    regs->r15      = frame->r15;
    regs->r14      = frame->r14;
    regs->r13      = frame->r13;
    regs->r12      = frame->r12;
    regs->rbp      = frame->rbp;
    regs->rbx      = frame->rbx;
    regs->r11      = frame->r11;
    regs->r10      = frame->r10;
    regs->r9       = frame->r9;
    regs->r8       = frame->r8;
    regs->rax      = frame->rax;
    regs->rcx      = frame->rcx;
    regs->rdx      = frame->rdx;
    regs->rsi      = frame->rsi;
    regs->rdi      = frame->rdi;
    regs->orig_rax = orig_rax;
    regs->rip      = frame->rip;
    regs->cs       = frame->cs;
    regs->eflags   = frame->rflags;
    regs->rsp      = frame->rsp;
    regs->ss       = frame->ss;
}

void ptrace_regs_to_frame(syscall_frame_t *frame, const ptrace_user_regs_t *regs)
{
    if (!frame || !regs) return;
    frame->r15    = regs->r15;
    frame->r14    = regs->r14;
    frame->r13    = regs->r13;
    frame->r12    = regs->r12;
    frame->rbp    = regs->rbp;
    frame->rbx    = regs->rbx;
    frame->r11    = regs->r11;
    frame->r10    = regs->r10;
    frame->r9     = regs->r9;
    frame->r8     = regs->r8;
    frame->rax    = regs->rax;
    frame->rcx    = regs->rcx;
    frame->rdx    = regs->rdx;
    frame->rsi    = regs->rsi;
    frame->rdi    = regs->rdi;
    frame->rip    = regs->rip;
    frame->rflags = (regs->eflags & PTRACE_EFLAGS_USER_MASK) | PTRACE_EFLAGS_FIXED;
    frame->rsp    = regs->rsp;
    /* User segment selectors are invariant in the Uinxed x86-64 ABI. */
    frame->cs = 0x1b;
    frame->ss = 0x23;
}

int ptrace_wait_status(int sig, uint32_t event)
{
    return ((sig & 0xff) << 8) | 0x7f | ((int)event << 16);
}

int ptrace_syscall_stop_signal(uint32_t options)
{
    return SIGTRAP | ((options & PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
}

int64_t ptrace_tracer_pid(const task_t *task)
{
    if (!task) return 0;
    return __atomic_load_n(&task->ptrace.tracer_pid, __ATOMIC_ACQUIRE);
}

static int ptrace_access_allowed(process_t *tracer, process_t *tracee)
{
    if (!tracer || !tracee || tracer == tracee) return -EPERM;
    if (!tracee->user_page_dir) return -EPERM;
    if (tracer->uid != 0 && tracer->uid != tracee->uid) return -EPERM;
    return 0;
}

static int ptrace_attached_by_current(task_t *target)
{
    task_t *current = current_task();
    if (!current || !target) return 0;
    return ptrace_tracer_pid(target) == (int64_t)current->pid;
}

static void ptrace_fpu_save(void *area)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

static void ptrace_fpu_restore(const void *area)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}

static void ptrace_debug_save(task_t *task)
{
    if (!task) return;
    __asm__ volatile("mov %%dr0, %0" : "=r"(task->ptrace.debug_regs[0]));
    __asm__ volatile("mov %%dr1, %0" : "=r"(task->ptrace.debug_regs[1]));
    __asm__ volatile("mov %%dr2, %0" : "=r"(task->ptrace.debug_regs[2]));
    __asm__ volatile("mov %%dr3, %0" : "=r"(task->ptrace.debug_regs[3]));
    __asm__ volatile("mov %%dr6, %0" : "=r"(task->ptrace.debug_regs[6]));
    __asm__ volatile("mov %%dr7, %0" : "=r"(task->ptrace.debug_regs[7]));
}

static void ptrace_debug_restore(const task_t *task)
{
    uint64_t dr0 = task ? task->ptrace.debug_regs[0] : 0;
    uint64_t dr1 = task ? task->ptrace.debug_regs[1] : 0;
    uint64_t dr2 = task ? task->ptrace.debug_regs[2] : 0;
    uint64_t dr3 = task ? task->ptrace.debug_regs[3] : 0;
    uint64_t dr6 = task ? task->ptrace.debug_regs[6] : 0xffff0ff0ULL;
    uint64_t dr7 = task ? task->ptrace.debug_regs[7] : 0;
    __asm__ volatile("mov %0, %%dr0" : : "r"(dr0));
    __asm__ volatile("mov %0, %%dr1" : : "r"(dr1));
    __asm__ volatile("mov %0, %%dr2" : : "r"(dr2));
    __asm__ volatile("mov %0, %%dr3" : : "r"(dr3));
    __asm__ volatile("mov %0, %%dr6" : : "r"(dr6));
    __asm__ volatile("mov %0, %%dr7" : : "r"(dr7));
}

void ptrace_arch_switch(task_t *previous, task_t *next)
{
    int previous_active = previous && previous->ptrace.debug_regs[7];
    int next_active     = next && next->ptrace.debug_regs[7];

    /*
     * Debug-register accesses serialize execution.  The overwhelmingly
     * common case has no hardware breakpoints and needs no DR traffic.
     */
    if (!previous_active && !next_active) return;
    if (previous_active) ptrace_debug_save(previous);
    ptrace_debug_restore(next_active ? next : NULL);
}

static void ptrace_build_user_area(task_t *target, ptrace_user_area_t *area)
{
    memset(area, 0, sizeof(*area));
    area->regs        = target->ptrace.regs;
    area->fp_valid    = 1;
    area->signal      = target->ptrace.stop_signal;
    area->start_code  = PROCESS_USER_CODE_MIN;
    area->start_stack = PROCESS_USER_STACK_TOP;
    memcpy(area->fpregs, target->ptrace.fpregs, sizeof(area->fpregs));
    memcpy(area->debug_regs, target->ptrace.debug_regs, sizeof(area->debug_regs));
    if (target->process) {
        strncpy(area->command, target->name, sizeof(area->command) - 1);
        for (vm_area_t *vma = target->process->mmap_list; vma; vma = vma->next) {
            uint64_t pages = (vma->end - vma->start) / PAGE_4K_SIZE;
            if (vma->type == VM_REGION_CODE) area->text_pages += pages;
            if (vma->type == VM_REGION_DATA || vma->type == VM_REGION_HEAP) area->data_pages += pages;
            if (vma->type == VM_REGION_STACK) area->stack_pages += pages;
        }
    }
}

static int ptrace_poke_user(task_t *target, uintptr_t offset, uint64_t value)
{
    if ((offset & (sizeof(uint64_t) - 1)) || offset >= sizeof(ptrace_user_area_t)) {
        plogk("ptrace: POKEUSR unaligned or out-of-range offset %lx (pid=%d)\n", (unsigned long)offset, (int)target->pid);
        return -EIO;
    }
    if (offset < sizeof(ptrace_user_regs_t)) {
        *(uint64_t *)((uint8_t *)&target->ptrace.regs + offset) = value;
        return 0;
    }
    uintptr_t debug_start = offsetof(ptrace_user_area_t, debug_regs);
    uintptr_t debug_end   = debug_start + sizeof(target->ptrace.debug_regs);
    if (offset >= debug_start && offset < debug_end) {
        size_t index = (offset - debug_start) / sizeof(uint64_t);
        if (index == 4 || index == 5) {
            plogk("ptrace: POKEUSR reserved debug register index %lu (pid=%d)\n", (unsigned long)index, (int)target->pid);
            return -EIO;
        }
        if (index < 4 && value >= PROCESS_USER_STACK_TOP) {
            plogk("ptrace: POKEUSR invalid debug register value %lx (pid=%d)\n", (unsigned long)value, (int)target->pid);
            return -EIO;
        }
        if (index == 7 && (value & (1ULL << 13))) { // General detect is kernel-only.
            plogk("ptrace: POKEUSR general-detect bit rejected (pid=%d)\n", (int)target->pid);
            return -EIO;
        }
        target->ptrace.debug_regs[index] = value;
        return 0;
    }
    plogk("ptrace: POKEUSR offset %lx outside valid ranges (pid=%d)\n", (unsigned long)offset, (int)target->pid);
    return -EIO;
}

static int ptrace_translate(process_t *proc, uintptr_t addr, bool write, void **mapped, size_t *available)
{
    if (!proc || !proc->user_page_dir || !proc->user_page_dir->table || addr >= PROCESS_USER_STACK_TOP) return -EIO;

    if (write && page_resolve_cow_fault(proc, addr) < 0) { // Non-COW writable mappings continue through the normal walk.
    }

    uint16_t l4i = (addr >> 39) & 0x1ff;
    uint16_t l3i = (addr >> 30) & 0x1ff;
    uint16_t l2i = (addr >> 21) & 0x1ff;
    uint16_t l1i = (addr >> 12) & 0x1ff;

    page_table_t *l4  = proc->user_page_dir->table;
    uint64_t      l4e = l4->entries[l4i].value;
    if (!(l4e & PTE_PRESENT) || !(l4e & PTE_USER)) return -EIO;
    page_table_t *l3  = phys_to_virt(l4e & PAGE_4K_MASK);
    uint64_t      l3e = l3->entries[l3i].value;
    if (!(l3e & PTE_PRESENT) || !(l3e & PTE_USER)) return -EIO;
    if (l3e & PTE_HUGE) {
        uintptr_t offset = addr & (PAGE_1G_SIZE - 1);
        *mapped          = phys_to_virt((l3e & PAGE_1G_MASK) + offset);
        *available       = PAGE_1G_SIZE - offset;
        return 0;
    }
    page_table_t *l2  = phys_to_virt(l3e & PAGE_4K_MASK);
    uint64_t      l2e = l2->entries[l2i].value;
    if (!(l2e & PTE_PRESENT) || !(l2e & PTE_USER)) return -EIO;
    if (l2e & PTE_HUGE) {
        uintptr_t offset = addr & (PAGE_2M_SIZE - 1);
        *mapped          = phys_to_virt((l2e & PAGE_2M_MASK) + offset);
        *available       = PAGE_2M_SIZE - offset;
        return 0;
    }
    page_table_t *l1  = phys_to_virt(l2e & PAGE_4K_MASK);
    uint64_t      l1e = l1->entries[l1i].value;
    if (!(l1e & PTE_PRESENT) || !(l1e & PTE_USER)) return -EIO;

    uintptr_t offset = addr & (PAGE_4K_SIZE - 1);
    *mapped          = phys_to_virt((l1e & PAGE_4K_MASK) + offset);
    *available       = PAGE_4K_SIZE - offset;
    return 0;
}

static int ptrace_access_vm(process_t *proc, uintptr_t addr, void *buffer, size_t size, bool write)
{
    uint8_t *bytes = buffer;
    while (size) {
        void  *mapped;
        size_t available;
        int    ret = ptrace_translate(proc, addr, write, &mapped, &available);
        if (ret) return ret;
        size_t chunk = size < available ? size : available;
        if (write) {
            memcpy(mapped, bytes, chunk);
        } else {
            memcpy(bytes, mapped, chunk);
        }
        addr += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return 0;
}

static void ptrace_notify_tracer(task_t *tracee)
{
    int64_t tracer_pid = ptrace_tracer_pid(tracee);
    if (!tracer_pid) return;
    process_t *tracer = process_find_get((pid_t)tracer_pid);
    if (!tracer) return;

    ptrace_state_t *state = &tracee->ptrace;
    spin_lock(&state->lock);
    int status = state->wait_status;
    spin_unlock(&state->lock);
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo  = SIGCHLD;
    info.si_code   = (status & 0x7f) == 0x7f ? CLD_TRAPPED : (status & 0x7f ? CLD_KILLED : CLD_EXITED);
    info.si_pid    = (int64_t)tracee->pid;
    info.si_uid    = tracee->process ? tracee->process->uid : 0;
    info.si_status = (status & 0x7f) == 0x7f ? ((status >> 8) & 0xff) : (status & 0x7f ? status & 0x7f : (status >> 8) & 0xff);
    signal_send(tracer, SIGCHLD, &info);
    process_put(tracer);
}

static int ptrace_stop_current(syscall_frame_t *frame, int sig, ptrace_stop_reason_t reason, uint32_t event, uint64_t msg, const siginfo_t *info)
{
    task_t *task = current_task();
    if (!task || !frame || !ptrace_tracer_pid(task)) return sig;

    ptrace_state_t *state = &task->ptrace;
    spin_lock(&state->lock);
    if (!state->tracer_pid) {
        spin_unlock(&state->lock);
        return sig;
    }
    ptrace_regs_from_frame(&state->regs, frame, state->syscall_nr);
    if (reason == PTRACE_STOP_SYSCALL_ENTRY) state->regs.rax = (uint64_t)-ENOSYS;
    state->regs.fs_base = task->thread.fs_base;
    state->regs.gs_base = task->thread.gs_base;
    ptrace_debug_save(task);
    ptrace_fpu_save(state->fpregs);
    state->regs_valid    = true;
    state->stopped       = true;
    state->wait_pending  = true;
    state->stop_reason   = reason;
    state->stop_signal   = sig;
    state->event         = event;
    state->event_msg     = msg;
    state->wait_status   = ptrace_wait_status(sig, event);
    state->final_exit    = false;
    state->resume_signal = 0;
    if (info) {
        state->siginfo = *info;
    } else {
        memset(&state->siginfo, 0, sizeof(state->siginfo));
        state->siginfo.si_signo = sig;
        state->siginfo.si_code  = SI_KERNEL;
    }
    spin_unlock(&state->lock);

    ptrace_notify_tracer(task);
    task_block();

    spin_lock(&state->lock);
    if (state->regs_valid) {
        ptrace_regs_to_frame(frame, &state->regs);
        task->thread.fs_base = state->regs.fs_base;
        task->thread.gs_base = state->regs.gs_base;
        ptrace_fpu_restore(state->fpregs);
    }
    int resume_signal    = state->resume_signal;
    state->resume_signal = 0;
    spin_unlock(&state->lock);
    return resume_signal;
}

static int ptrace_resume(task_t *target, ptrace_run_mode_t mode, int sig)
{
    if (!sig_valid(sig) && sig != 0) {
        plogk("ptrace: Resume with invalid signal %d (pid=%d)\n", sig, (int)target->pid);
        return -EIO;
    }
    ptrace_state_t *state = &target->ptrace;
    spin_lock(&state->lock);
    if (!state->stopped) {
        spin_unlock(&state->lock);
        plogk("ptrace: Resume of non-stopped task (pid=%d)\n", (int)target->pid);
        return -ESRCH;
    }
    state->mode          = mode;
    state->resume_signal = sig;
    state->stopped       = false;
    state->stop_reason   = PTRACE_STOP_NONE;
    state->event         = 0;
    if (mode == PTRACE_RUN_SINGLESTEP && state->regs_valid) state->regs.eflags |= PTRACE_EFLAGS_TF;
    if (mode != PTRACE_RUN_SINGLESTEP && state->regs_valid) state->regs.eflags &= ~PTRACE_EFLAGS_TF;
    spin_unlock(&state->lock);
    task_wakeup(target);
    return 0;
}

static int ptrace_attach(task_t *target, process_t *owner, bool seize, uint32_t options)
{
    process_t *current = process_current();
    task_t    *self    = current_task();
    int        ret     = ptrace_access_allowed(current, owner);
    if (ret) return ret;
    if (!self || target == self || (options & ~PTRACE_O_MASK)) {
        plogk("ptrace: Attach invalid args (target=%p, options=%x)\n", (void *)target, (unsigned)options);
        return -EINVAL;
    }
    if ((options & PTRACE_O_SUSPEND_SECCOMP) && current->uid != 0) {
        plogk("ptrace: Attach with SUSPEND_SECCOMP requires root (target=%d)\n", (int)target->pid);
        return -EPERM;
    }
    if (target->state == TASK_ZOMBIE) {
        plogk("ptrace: Attach to zombie (pid=%d)\n", (int)target->pid);
        return -ESRCH;
    }

    ptrace_state_t *state = &target->ptrace;
    spin_lock(&state->lock);
    if (state->tracer_pid) {
        spin_unlock(&state->lock);
        plogk("ptrace: Attach denied, task %d already traced (tracer=%lld)\n", (int)target->pid, (long long)state->tracer_pid);
        return -EPERM;
    }
    state->tracer_pid = (int64_t)self->pid;
    state->options    = options;
    state->seized     = seize;
    state->mode       = PTRACE_RUN_CONT;
    spin_unlock(&state->lock);

    if (!seize) {
        siginfo_t info = {0};
        info.si_signo  = SIGSTOP;
        info.si_code   = SI_USER;
        info.si_pid    = (int64_t)self->pid;
        info.si_uid    = current->uid;
        ret            = signal_send_thread(target, SIGSTOP, &info);
        if (ret) {
            spin_lock(&state->lock);
            state->tracer_pid = 0;
            spin_unlock(&state->lock);
        }
    }
    return ret;
}

static int ptrace_copy_regset(task_t *target, uintptr_t note, uintptr_t data, bool write)
{
    ptrace_iovec_t iov;
    if (copy_from_user(&iov, (void *)data, sizeof(iov))) return -EFAULT;
    void  *source;
    size_t source_size;
    if (note == NT_PRSTATUS) {
        source      = &target->ptrace.regs;
        source_size = sizeof(target->ptrace.regs);
    } else if (note == NT_FPREGSET) {
        source      = target->ptrace.fpregs;
        source_size = sizeof(target->ptrace.fpregs);
    } else {
        plogk("ptrace: GET/SETREGSET with unknown note %lx (pid=%d)\n", (unsigned long)note, (int)target->pid);
        return -EINVAL;
    }
    size_t length = iov.len < source_size ? iov.len : source_size;
    if (write) {
        if (copy_from_user(source, iov.base, length)) return -EFAULT;
    } else {
        if (copy_to_user(iov.base, source, length)) return -EFAULT;
    }
    iov.len = length;
    return copy_to_user((void *)data, &iov, sizeof(iov)) ? -EFAULT : 0;
}

static int64_t ptrace_peek_siginfo(task_t *target, uintptr_t addr, uintptr_t data)
{
    ptrace_peeksiginfo_args_t args;
    if (copy_from_user(&args, (void *)addr, sizeof(args))) return -EFAULT;
    if (args.flags & ~PTRACE_PEEKSIGINFO_SHARED || args.nr < 0) {
        plogk("ptrace: PEEKSIGINFO invalid args (flags=%lx, pid=%d)\n", (unsigned long)args.flags, (int)target->pid);
        return -EINVAL;
    }

    signal_state_t *signals = &target->process->signal;
    spin_lock(&signals->lock);
    sigqueue_t *item = signals->sigqueue_head;
    uint64_t    skip = args.off;
    while (item && skip--) item = item->next;
    int copied = 0;
    while (item && copied < args.nr) {
        siginfo_t info = item->info;
        spin_unlock(&signals->lock);
        if (copy_to_user((siginfo_t *)data + copied, &info, sizeof(info))) return -EFAULT;
        copied++;
        spin_lock(&signals->lock);
        item = item->next;
    }
    spin_unlock(&signals->lock);
    return copied;
}

static int64_t ptrace_get_syscall_info(task_t *target, uintptr_t size, uintptr_t data)
{
    ptrace_syscall_info_t info;
    memset(&info, 0, sizeof(info));
    ptrace_state_t *state = &target->ptrace;
    if (state->stop_reason == PTRACE_STOP_SYSCALL_ENTRY) {
        info.op = PTRACE_SYSCALL_INFO_ENTRY;
    } else if (state->stop_reason == PTRACE_STOP_SYSCALL_EXIT) {
        info.op = PTRACE_SYSCALL_INFO_EXIT;
    } else {
        info.op = PTRACE_SYSCALL_INFO_NONE;
    }
    info.arch                = AUDIT_ARCH_X86_64;
    info.instruction_pointer = state->regs.rip;
    info.stack_pointer       = state->regs.rsp;
    if (info.op == PTRACE_SYSCALL_INFO_ENTRY) {
        info.data.entry.nr      = state->regs.orig_rax;
        info.data.entry.args[0] = state->regs.rdi;
        info.data.entry.args[1] = state->regs.rsi;
        info.data.entry.args[2] = state->regs.rdx;
        info.data.entry.args[3] = state->regs.r10;
        info.data.entry.args[4] = state->regs.r8;
        info.data.entry.args[5] = state->regs.r9;
    } else if (info.op == PTRACE_SYSCALL_INFO_EXIT) {
        info.data.exit.rval     = (int64_t)state->regs.rax;
        info.data.exit.is_error = info.data.exit.rval < 0 && info.data.exit.rval >= -4095;
    }
    size_t copy_size = size < sizeof(info) ? size : sizeof(info);
    if (copy_size && copy_to_user((void *)data, &info, copy_size)) return -EFAULT;
    return sizeof(info);
}

int64_t sys_ptrace(int request, int64_t pid, uintptr_t addr, uintptr_t data)
{
    task_t    *self    = current_task();
    process_t *current = process_current();
    if (!self || !current) {
        plogk("ptrace: No current task for ptrace request %d\n", request);
        return -ESRCH;
    }

    if (request == PTRACE_TRACEME) {
        if (!current->parent || !current->parent->task) {
            plogk("ptrace: TRACEME with no parent (pid=%d)\n", (int)self->pid);
            return -ESRCH;
        }
        ptrace_state_t *state = &self->ptrace;
        spin_lock(&state->lock);
        if (state->tracer_pid) {
            spin_unlock(&state->lock);
            plogk("ptrace: TRACEME already traced (pid=%d, tracer=%lld)\n", (int)self->pid, (long long)state->tracer_pid);
            return -EPERM;
        }
        state->tracer_pid = (int64_t)current->parent->task->pid;
        state->mode       = PTRACE_RUN_CONT;
        spin_unlock(&state->lock);
        return 0;
    }

    process_t *owner  = NULL;
    task_t    *target = ptrace_find_task_get(pid, &owner);
    if (!target) {
        plogk("ptrace: Target pid %lld not found.\n", (long long)pid);
        return -ESRCH;
    }
    int64_t ret = 0;

    if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
        if (request == PTRACE_SEIZE && addr) {
            plogk("ptrace: SEIZE with non-null addr %lx (pid=%lld)\n", (unsigned long)addr, (long long)pid);
            process_put(owner);
            return -EIO;
        }
        ret = ptrace_attach(target, owner, request == PTRACE_SEIZE, request == PTRACE_SEIZE ? (uint32_t)data : 0);
        process_put(owner);
        return ret;
    }
    if (!ptrace_attached_by_current(target)) {
        plogk("ptrace: Task %lld not attached by pid %d\n", (long long)pid, (int)self->pid);
        process_put(owner);
        return -ESRCH;
    }

    ptrace_state_t *state = &target->ptrace;
    spin_lock(&state->lock);
    bool stopped      = state->stopped;
    bool wait_pending = state->wait_pending;
    spin_unlock(&state->lock);
    if (request != PTRACE_INTERRUPT && request != PTRACE_KILL && (!stopped || wait_pending)) {
        plogk("ptrace: Task %lld not stopped (request=%d, stopped=%d, wait_pending=%d)\n", (long long)pid, request, stopped, wait_pending);
        process_put(owner);
        return -ESRCH;
    }
    switch (request) {
        case PTRACE_PEEKTEXT :
        case PTRACE_PEEKDATA : {
            uint64_t word;
            ret = ptrace_access_vm(owner, addr, &word, sizeof(word), false);
            if (!ret) ret = (int64_t)word;
            break;
        }
        case PTRACE_POKETEXT :
        case PTRACE_POKEDATA : {
            uint64_t word = data;
            ret           = ptrace_access_vm(owner, addr, &word, sizeof(word), true);
            break;
        }
        case PTRACE_PEEKUSR :
            if ((addr & (sizeof(uint64_t) - 1)) || addr >= sizeof(ptrace_user_area_t)) {
                plogk("ptrace: PEEKUSR invalid offset %lx (pid=%lld)\n", (unsigned long)addr, (long long)pid);
                ret = -EIO;
            } else {
                ptrace_user_area_t area;
                ptrace_build_user_area(target, &area);
                ret = *(int64_t *)((uint8_t *)&area + addr);
            }
            break;
        case PTRACE_POKEUSR :
            ret = ptrace_poke_user(target, addr, data);
            break;
        case PTRACE_GETREGS :
            ret = copy_to_user((void *)data, &state->regs, sizeof(state->regs)) ? -EFAULT : 0;
            break;
        case PTRACE_SETREGS : {
            ptrace_user_regs_t regs;
            if (copy_from_user(&regs, (void *)data, sizeof(regs))) {
                ret = -EFAULT;
            } else {
                regs.cs     = 0x1b;
                regs.ss     = 0x23;
                regs.eflags = (regs.eflags & PTRACE_EFLAGS_USER_MASK) | PTRACE_EFLAGS_FIXED;
                state->regs = regs;
            }
            break;
        }
        case PTRACE_GETFPREGS :
        case PTRACE_GETFPXREGS :
            ret = copy_to_user((void *)data, state->fpregs, sizeof(state->fpregs)) ? -EFAULT : 0;
            break;
        case PTRACE_SETFPREGS :
        case PTRACE_SETFPXREGS :
            ret = copy_from_user(state->fpregs, (void *)data, sizeof(state->fpregs)) ? -EFAULT : 0;
            break;
        case PTRACE_GETREGSET :
            ret = ptrace_copy_regset(target, addr, data, false);
            break;
        case PTRACE_SETREGSET :
            ret = ptrace_copy_regset(target, addr, data, true);
            break;
        case PTRACE_CONT :
            ret = ptrace_resume(target, PTRACE_RUN_CONT, (int)data);
            break;
        case PTRACE_SYSCALL :
            ret = ptrace_resume(target, PTRACE_RUN_SYSCALL, (int)data);
            break;
        case PTRACE_SINGLESTEP :
            ret = ptrace_resume(target, PTRACE_RUN_SINGLESTEP, (int)data);
            break;
        case PTRACE_LISTEN :
            spin_lock(&state->lock);
            if (!state->seized || state->stop_reason != PTRACE_STOP_GROUP) {
                ret = -EIO;
            } else {
                state->mode = PTRACE_RUN_LISTEN;
            }
            spin_unlock(&state->lock);
            break;
        case PTRACE_INTERRUPT :
            spin_lock(&state->lock);
            bool listening = state->mode == PTRACE_RUN_LISTEN && state->stopped;
            spin_unlock(&state->lock);
            if (!state->seized || (stopped && !listening)) {
                ret = -EIO;
            } else if (listening) {
                spin_lock(&state->lock);
                state->mode         = PTRACE_RUN_CONT;
                state->stop_reason  = PTRACE_STOP_EVENT;
                state->stop_signal  = SIGTRAP;
                state->event        = PTRACE_EVENT_STOP;
                state->event_msg    = 0;
                state->wait_status  = ptrace_wait_status(SIGTRAP, PTRACE_EVENT_STOP);
                state->wait_pending = true;
                spin_unlock(&state->lock);
                ptrace_notify_tracer(target);
            } else {
                spin_lock(&state->lock);
                state->interrupt_requested = true;
                spin_unlock(&state->lock);
                ret = signal_send_thread(target, SIGTRAP, NULL);
            }
            break;
        case PTRACE_KILL :
            ret = signal_send_thread(target, SIGKILL, NULL);
            if (state->stopped) task_wakeup(target);
            break;
        case PTRACE_DETACH :
            if (!sig_valid((int)data) && data != 0) {
                ret = -EIO;
                break;
            }
            spin_lock(&state->lock);
            state->tracer_pid    = 0;
            state->options       = 0;
            state->seized        = false;
            state->resume_signal = (int)data;
            bool stopped         = state->stopped;
            state->stopped       = false;
            spin_unlock(&state->lock);
            if (stopped) task_wakeup(target);
            break;
        case PTRACE_SETOPTIONS :
            if (data & ~PTRACE_O_MASK) {
                ret = -EINVAL;
            } else if ((data & PTRACE_O_SUSPEND_SECCOMP) && current->uid != 0) {
                ret = -EPERM;
            } else {
                spin_lock(&state->lock);
                state->options = (uint32_t)data;
                spin_unlock(&state->lock);
            }
            break;
        case PTRACE_GETEVENTMSG :
            ret = copy_to_user((void *)data, &state->event_msg, sizeof(state->event_msg)) ? -EFAULT : 0;
            break;
        case PTRACE_GETSIGINFO :
            ret = state->stop_reason == PTRACE_STOP_NONE ? -EINVAL :
                                                           (copy_to_user((void *)data, &state->siginfo, sizeof(state->siginfo)) ? -EFAULT : 0);
            break;
        case PTRACE_SETSIGINFO :
            ret = state->stop_reason == PTRACE_STOP_NONE ? -EINVAL :
                                                           (copy_from_user(&state->siginfo, (void *)data, sizeof(state->siginfo)) ? -EFAULT : 0);
            break;
        case PTRACE_GETSIGMASK :
            if (addr != sizeof(sigset_t)) {
                ret = -EINVAL;
            } else {
                spin_lock(&owner->signal.lock);
                sigset_t mask = owner->signal.blocked;
                spin_unlock(&owner->signal.lock);
                ret = copy_to_user((void *)data, &mask, sizeof(mask)) ? -EFAULT : 0;
            }
            break;
        case PTRACE_SETSIGMASK : {
            sigset_t mask;
            if (addr != sizeof(sigset_t)) {
                ret = -EINVAL;
            } else if (copy_from_user(&mask, (void *)data, sizeof(mask))) {
                ret = -EFAULT;
            } else {
                sigdelset(&mask, SIGKILL);
                sigdelset(&mask, SIGSTOP);
                spin_lock(&owner->signal.lock);
                owner->signal.blocked = mask;
                spin_unlock(&owner->signal.lock);
            }
            break;
        }
        case PTRACE_PEEKSIGINFO :
            ret = ptrace_peek_siginfo(target, addr, data);
            break;
        case PTRACE_GET_SYSCALL_INFO :
            ret = ptrace_get_syscall_info(target, addr, data);
            break;
        case PTRACE_SECCOMP_GET_FILTER :
        case PTRACE_SECCOMP_GET_METADATA :
            plogk("ptrace: SECCOMP filter/metadata requests unsupported (pid=%lld)\n", (long long)pid);
            ret = -EINVAL;
            break;
        default :
            plogk("ptrace: Unknown request %d (pid=%lld, addr=%lx)\n", request, (long long)pid, (unsigned long)addr);
            ret = -EIO;
            break;
    }

    process_put(owner);
    return ret;
}

static task_t *ptrace_wait_target_get(task_t *tracer, int64_t pid, process_t **owner, bool *exists)
{
    *owner  = NULL;
    *exists = false;
    if (pid > 0) {
        task_t *target = ptrace_find_task_get(pid, owner);
        if (!target || ptrace_tracer_pid(target) != (int64_t)tracer->pid) {
            process_put(*owner);
            *owner = NULL;
            return NULL;
        }
        *exists = true;
        return target;
    }

    size_t     position = 0;
    process_t *proc;
    while ((proc = process_iterate_get(&position))) {
        uint64_t requested_group = pid < -1 ? (uint64_t)(-(pid + 1)) + 1 : 0;
        bool     group_match
            = pid == -1 || (pid == 0 && proc->pgid == tracer->process->pgid) || (pid < -1 && (uint64_t)proc->pgid == requested_group);
        if (!group_match) {
            process_put(proc);
            continue;
        }
        for (ilist_node_t *node = proc->threads.next; node != &proc->threads; node = node->next) {
            task_t *target = rb_entry(node, task_t, thread_node);
            if (ptrace_tracer_pid(target) != (int64_t)tracer->pid) continue;
            *exists = true;
            spin_lock(&target->ptrace.lock);
            bool pending = target->ptrace.wait_pending;
            spin_unlock(&target->ptrace.lock);
            if (pending) {
                *owner = proc;
                return target;
            }
        }
        process_put(proc);
    }
    return NULL;
}

int64_t ptrace_wait_event(int64_t pid, int *status, int options)
{
    task_t *self = current_task();
    if (!self) return -ECHILD;

    for (;;) {
        process_t *owner = NULL;
        bool       exists;
        task_t    *target = ptrace_wait_target_get(self, pid, &owner, &exists);
        if (!exists) return -ECHILD;
        if (!target) goto wait_again;
        ptrace_state_t *state = &target->ptrace;
        spin_lock(&state->lock);
        if (state->wait_pending) {
            int      value      = state->wait_status;
            bool     final_exit = state->final_exit;
            uint64_t target_pid = target->pid;
            state->wait_pending = false;
            if (final_exit) state->tracer_pid = 0;
            spin_unlock(&state->lock);
            bool       reap        = final_exit && target->pid == target->tgid && owner->parent == self->process;
            process_t *real_parent = NULL;
            if (owner->parent && owner->parent->task) real_parent = process_find_get(owner->parent->task->tgid);
            process_put(owner);
            if (reap) (void)process_wait((pid_t)target_pid, NULL);
            if (final_exit && real_parent && real_parent->task) task_wakeup(real_parent->task);
            process_put(real_parent);
            if (status) *status = value;
            return (int64_t)target_pid;
        }
        spin_unlock(&state->lock);
        process_put(owner);

wait_again:
        if (options & PTRACE_WAIT_WNOHANG) return 0;
        signal_state_t *signals = &self->process->signal;
        spin_lock(&signals->lock);
        bool pending = signal_has_pending(signals);
        spin_unlock(&signals->lock);
        if (pending) return -ERESTARTSYS;
        task_block();
    }
}

int ptrace_signal_delivery(syscall_frame_t *frame, int sig, siginfo_t *info)
{
    task_t *task = current_task();
    if (!task || !ptrace_tracer_pid(task) || sig == SIGKILL) return sig;
    ptrace_state_t *state = &task->ptrace;
    spin_lock(&state->lock);
    bool interrupt             = state->interrupt_requested;
    state->interrupt_requested = false;
    bool group_stop            = sig_is_stop(sig) && state->seized;
    spin_unlock(&state->lock);
    int injected;
    if (interrupt) {
        injected = ptrace_stop_current(frame, SIGTRAP, PTRACE_STOP_EVENT, PTRACE_EVENT_STOP, (uint64_t)sig, info);
    } else {
        injected
            = ptrace_stop_current(frame, sig, group_stop ? PTRACE_STOP_GROUP : PTRACE_STOP_SIGNAL, group_stop ? PTRACE_EVENT_STOP : 0, 0, info);
    }
    if (injected && info) {
        spin_lock(&state->lock);
        *info = state->siginfo;
        spin_unlock(&state->lock);
        info->si_signo = injected;
    }
    return injected;
}

void ptrace_syscall_enter(syscall_frame_t *frame, uint64_t syscall_nr)
{
    task_t *task = current_task();
    if (!task || !ptrace_tracer_pid(task)) return;
    ptrace_state_t *state = &task->ptrace;
    spin_lock(&state->lock);
    state->active_frame = frame;
    state->syscall_nr   = syscall_nr;
    if (!state->tracer_pid) {
        spin_unlock(&state->lock);
        return;
    }
    bool     stop    = state->mode == PTRACE_RUN_SYSCALL;
    uint32_t options = state->options;
    spin_unlock(&state->lock);
    if (stop) {
        ptrace_stop_current(frame, ptrace_syscall_stop_signal(options), PTRACE_STOP_SYSCALL_ENTRY, 0, 0, NULL);
        spin_lock(&state->lock);
        frame->rax = state->regs.orig_rax;
        spin_unlock(&state->lock);
    }
}

void ptrace_syscall_exit(syscall_frame_t *frame, int64_t result)
{
    task_t *task = current_task();
    if (!task || !ptrace_tracer_pid(task)) return;
    ptrace_state_t *state = &task->ptrace;
    spin_lock(&state->lock);
    bool     stop    = state->mode == PTRACE_RUN_SYSCALL;
    uint32_t options = state->options;
    spin_unlock(&state->lock);
    if (stop) {
        frame->rax = (uint64_t)result;
        ptrace_stop_current(frame, ptrace_syscall_stop_signal(options), PTRACE_STOP_SYSCALL_EXIT, 0, 0, NULL);
    }
}

void ptrace_exec_event(syscall_frame_t *frame)
{
    task_t *task = current_task();
    if (!task || !ptrace_tracer_pid(task)) return;
    ptrace_state_t *state = &task->ptrace;
    spin_lock(&state->lock);
    bool event = (state->options & PTRACE_O_TRACEEXEC) != 0;
    spin_unlock(&state->lock);
    ptrace_stop_current(frame, SIGTRAP, event ? PTRACE_STOP_EVENT : PTRACE_STOP_SIGNAL, event ? PTRACE_EVENT_EXEC : 0, task->pid, NULL);
}

void ptrace_exit_event(int exit_code)
{
    task_t *task = current_task();
    if (!task || !ptrace_tracer_pid(task)) return;
    ptrace_state_t *state = &task->ptrace;
    spin_lock(&state->lock);
    bool event = (state->options & PTRACE_O_TRACEEXIT) != 0;
    spin_unlock(&state->lock);
    if (!event) return;

    syscall_frame_t  fallback;
    syscall_frame_t *frame = state->active_frame;
    if (!frame) {
        memset(&fallback, 0, sizeof(fallback));
        fallback.cs     = 0x1b;
        fallback.ss     = 0x23;
        fallback.rflags = PTRACE_EFLAGS_FIXED;
        frame           = &fallback;
    }
    ptrace_stop_current(frame, SIGTRAP, PTRACE_STOP_EVENT, PTRACE_EVENT_EXIT, (uint64_t)(uint32_t)exit_code, NULL);
}

void ptrace_exit_notify(int exit_code)
{
    task_t *task = current_task();
    if (!task || !ptrace_tracer_pid(task)) return;
    ptrace_state_t *state = &task->ptrace;
    spin_lock(&state->lock);
    if (!state->tracer_pid) {
        spin_unlock(&state->lock);
        return;
    }
    state->wait_status  = exit_code < 0 ? ((-exit_code) & 0x7f) : ((exit_code & 0xff) << 8);
    state->wait_pending = true;
    state->final_exit   = true;
    state->stopped      = false;
    state->stop_reason  = PTRACE_STOP_NONE;
    state->stop_signal  = 0;
    state->event        = 0;
    spin_unlock(&state->lock);
    ptrace_notify_tracer(task);
}

static uint32_t ptrace_event_option(uint32_t event)
{
    switch (event) {
        case PTRACE_EVENT_FORK :
            return PTRACE_O_TRACEFORK;
        case PTRACE_EVENT_VFORK :
            return PTRACE_O_TRACEVFORK;
        case PTRACE_EVENT_CLONE :
            return PTRACE_O_TRACECLONE;
        case PTRACE_EVENT_VFORK_DONE :
            return PTRACE_O_TRACEVFORKDONE;
        default :
            return 0;
    }
}

bool ptrace_fork_child(task_t *parent, task_t *child, uint32_t event)
{
    if (!parent || !child || !ptrace_tracer_pid(parent)) return false;
    uint32_t        option       = ptrace_event_option(event);
    ptrace_state_t *parent_state = &parent->ptrace;
    spin_lock(&parent_state->lock);
    if (!(parent_state->options & option)) {
        spin_unlock(&parent_state->lock);
        return false;
    }
    int64_t  tracer  = parent_state->tracer_pid;
    uint32_t options = parent_state->options;
    bool     seized  = parent_state->seized;
    spin_unlock(&parent_state->lock);

    ptrace_state_t *state = &child->ptrace;
    spin_lock(&state->lock);
    state->tracer_pid   = tracer;
    state->options      = options;
    state->seized       = seized;
    state->mode         = PTRACE_RUN_CONT;
    state->stopped      = true;
    state->wait_pending = true;
    state->stop_reason  = PTRACE_STOP_EVENT;
    state->stop_signal  = SIGSTOP;
    state->wait_status  = ptrace_wait_status(SIGSTOP, 0);
    spin_unlock(&state->lock);
    child->state = TASK_BLOCKED;
    return true;
}

void ptrace_fork_event(syscall_frame_t *frame, uint32_t event, uint64_t child_pid)
{
    task_t *task = current_task();
    if (!task || !ptrace_tracer_pid(task)) return;
    ptrace_state_t *state  = &task->ptrace;
    uint32_t        option = ptrace_event_option(event);
    spin_lock(&state->lock);
    bool enabled = (state->options & option) != 0;
    spin_unlock(&state->lock);
    if (enabled) ptrace_stop_current(frame, SIGTRAP, PTRACE_STOP_EVENT, event, child_pid, NULL);
}

void ptrace_tracer_exit(int64_t tracer_pid)
{
    if (tracer_pid <= 0) return;
    size_t     pos = 0;
    process_t *proc;
    while ((proc = process_iterate_get(&pos))) {
        for (ilist_node_t *node = proc->threads.next; node != &proc->threads; node = node->next) {
            task_t         *task  = rb_entry(node, task_t, thread_node);
            ptrace_state_t *state = &task->ptrace;
            spin_lock(&state->lock);
            if (state->tracer_pid != tracer_pid) {
                spin_unlock(&state->lock);
                continue;
            }
            bool kill         = (state->options & PTRACE_O_EXITKILL) != 0;
            bool stopped      = state->stopped;
            state->tracer_pid = 0;
            state->stopped    = false;
            state->options    = 0;
            spin_unlock(&state->lock);
            if (kill) signal_send_thread(task, SIGKILL, NULL);
            if (stopped) task_wakeup(task);
            if (proc->parent && proc->parent->task) task_wakeup(proc->parent->task);
        }
        process_put(proc);
    }
}
