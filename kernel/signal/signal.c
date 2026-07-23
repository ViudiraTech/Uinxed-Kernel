/*
 *
 *      signal.c
 *      POSIX signal subsystem implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <kernel/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/signalfd.h>
#include <syscall/syscall.h>

/* ---------- Signal default action table ---------- */

static const sig_dfl_action_t sig_default_action_table[NSIG] = {
    [0] = SIG_DFL_TERM,       [SIGHUP] = SIG_DFL_TERM,    [SIGINT] = SIG_DFL_TERM,  [SIGQUIT] = SIG_DFL_CORE, [SIGILL] = SIG_DFL_CORE,
    [SIGTRAP] = SIG_DFL_CORE, [SIGABRT] = SIG_DFL_CORE,   [SIGBUS] = SIG_DFL_CORE,  [SIGFPE] = SIG_DFL_CORE,  [SIGKILL] = SIG_DFL_TERM,
    [SIGUSR1] = SIG_DFL_TERM, [SIGSEGV] = SIG_DFL_CORE,   [SIGUSR2] = SIG_DFL_TERM, [SIGPIPE] = SIG_DFL_TERM, [SIGALRM] = SIG_DFL_TERM,
    [SIGTERM] = SIG_DFL_TERM, [SIGSTKFLT] = SIG_DFL_TERM, [SIGCHLD] = SIG_DFL_IGN,  [SIGCONT] = SIG_DFL_CONT, [SIGSTOP] = SIG_DFL_STOP,
    [SIGTSTP] = SIG_DFL_STOP, [SIGTTIN] = SIG_DFL_STOP,   [SIGTTOU] = SIG_DFL_STOP, [SIGURG] = SIG_DFL_IGN,   [SIGXCPU] = SIG_DFL_CORE,
    [SIGXFSZ] = SIG_DFL_CORE, [SIGVTALRM] = SIG_DFL_TERM, [SIGPROF] = SIG_DFL_TERM, [SIGWINCH] = SIG_DFL_IGN, [SIGIO] = SIG_DFL_TERM,
    [SIGPWR] = SIG_DFL_TERM,  [SIGSYS] = SIG_DFL_CORE,
};

sig_dfl_action_t signal_default_action(int sig)
{
    if (!sig_valid(sig)) return SIG_DFL_TERM;
    if (sig >= SIGRTMIN && sig <= SIGRTMAX) return SIG_DFL_TERM;
    return sig_default_action_table[sig];
}

/* ---------- Signal queue helpers ---------- */

static sigqueue_t *sigqueue_alloc(void)
{
    return (sigqueue_t *)calloc(1, sizeof(sigqueue_t));
}

static void sigqueue_free(sigqueue_t *q)
{
    if (q) free(q);
}

static void sigqueue_push(signal_state_t *state, const siginfo_t *info)
{
    sigqueue_t *q = sigqueue_alloc();
    if (!q) return;

    memcpy(&q->info, info, sizeof(siginfo_t));
    q->next = NULL;

    if (!state->sigqueue_head) {
        state->sigqueue_head = q;
        state->sigqueue_tail = q;
    } else {
        state->sigqueue_tail->next = q;
        state->sigqueue_tail       = q;
    }
    state->sigqueue_count++;
}

static void sigqueue_flush(signal_state_t *state)
{
    while (state->sigqueue_head) {
        sigqueue_t *q        = state->sigqueue_head;
        state->sigqueue_head = q->next;
        sigqueue_free(q);
    }
    state->sigqueue_tail  = NULL;
    state->sigqueue_count = 0;
}

/* ---------- Signal state management ---------- */

void signal_init(void)
{
    plogk("signal: Subsystem initialized.\n");
}

void signal_state_init(signal_state_t *state)
{
    if (!state) return;

    memset(state, 0, sizeof(signal_state_t));

    /* All handlers default to SIG_DFL */
    for (int i = 0; i < SIG_ACTION_NUM; i++) {
        state->sighand[i].sa_handler = SIG_DFL;
        state->sighand[i].sa_flags   = 0;
        sigemptyset(&state->sighand[i].sa_mask);
    }

    sigemptyset(&state->pending);
    sigemptyset(&state->blocked);
    state->sigqueue_head      = NULL;
    state->sigqueue_tail      = NULL;
    state->sigqueue_count     = 0;
    state->altstack.ss_sp     = NULL;
    state->altstack.ss_size   = 0;
    state->altstack.ss_flags  = SS_DISABLE;
    state->child_exit_pending = 0;
}

void signal_state_free(signal_state_t *state)
{
    if (!state) return;
    spin_lock(&state->lock);
    sigqueue_flush(state);
    spin_unlock(&state->lock);
}

void signal_state_copy(signal_state_t *dst, const signal_state_t *src)
{
    if (!dst || !src) return;

    spin_lock(&((signal_state_t *)src)->lock);

    memcpy(dst->sighand, src->sighand, sizeof(dst->sighand));
    dst->pending = src->pending;
    dst->blocked = src->blocked;

    sigqueue_flush(dst);
    sigqueue_t *cur = src->sigqueue_head;
    while (cur) {
        sigqueue_push(dst, &cur->info);
        cur = cur->next;
    }

    dst->altstack           = src->altstack;
    dst->child_exit_code    = src->child_exit_code;
    dst->child_exit_pending = src->child_exit_pending;
    dst->child_exit_pid     = src->child_exit_pid;
    dst->child_exit_status  = src->child_exit_status;

    spin_unlock(&((signal_state_t *)src)->lock);
}

void signal_flush(process_t *proc)
{
    if (!proc) return;
    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);
    sigemptyset(&state->pending);
    sigqueue_flush(state);
    spin_unlock(&state->lock);
}

/* ---------- Permission check ---------- */

int signal_check_perm(const process_t *from, const process_t *to)
{
    if (!from || !to) return -ESRCH;

    /* Can always send to self */
    if (from == to) return 0;

    /* Root can send to anyone */
    if (from->uid == 0) return 0;

    /* Same UID */
    if (from->uid == to->uid) return 0;

    return -EPERM;
}

/* ---------- Signal sending ---------- */

static int signal_send_locked(signal_state_t *state, process_t *proc, int sig, const siginfo_t *info)
{
    /* For standard signals, non-RT: if already pending, just set the bit */
    if (!sig_is_rt(sig)) {
        if (sigismember(&state->pending, sig)) {
            /* Already pending, skip (non-RT signals don't queue) */
            return 0;
        }
        sigaddset(&state->pending, sig);
        return 0;
    }

    /* Real-time signals: queue up to SIGQUEUE_MAX */
    if (state->sigqueue_count >= SIGQUEUE_MAX) { return -EAGAIN; }

    siginfo_t queue_info;
    if (info) {
        memcpy(&queue_info, info, sizeof(siginfo_t));
    } else {
        memset(&queue_info, 0, sizeof(siginfo_t));
        queue_info.si_signo = sig;
        queue_info.si_code  = SI_USER;
        queue_info.si_pid   = (int64_t)(proc->task ? proc->task->pid : 0);
        queue_info.si_uid   = proc->uid;
    }
    queue_info.si_signo = sig;

    sigqueue_push(state, &queue_info);
    sigaddset(&state->pending, sig);
    return 0;
}

int signal_send(process_t *proc, int sig, const siginfo_t *info)
{
    if (!proc) return -ESRCH;
    if (!sig_valid(sig)) return -EINVAL;

    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    int ret = signal_send_locked(state, proc, sig, info);

    spin_unlock(&state->lock);

    if (ret == 0) {
        /* Wake the task if it's blocked */
        if (proc->task) { task_wakeup(proc->task); }
    }

    return ret;
}

int signal_send_thread(task_t *task, int sig, const siginfo_t *info)
{
    if (!task || !task->process) return -ESRCH;
    if (!sig_valid(sig)) return -EINVAL;

    process_t      *proc  = task->process;
    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    int ret = signal_send_locked(state, proc, sig, info);

    spin_unlock(&state->lock);

    if (ret == 0) { task_wakeup(task); }

    return ret;
}

/* ---------- Signal delivery ---------- */

/*
 * Prepare a signal frame on the user stack and modify the syscall
 * frame so that the process enters the signal handler when it
 * returns to userspace.
 *
 * We modify the syscall frame's rip, rsp, rdi, rsi, rdx directly.
 * The kernel's syscall_return (iretq) will pop these values and
 * jump to the handler.
 */
static int signal_setup_frame(syscall_frame_t *frame, int sig, const sigaction_t *sa, const siginfo_t *info)
{
    process_t *proc = process_current();
    if (!proc || !frame) return -ESRCH;

    /* Determine stack pointer and stack boundaries */
    uintptr_t sp, stack_limit;

    if ((sa->sa_flags & SA_ONSTACK) && !(proc->signal.altstack.ss_flags & SS_DISABLE) && !(proc->signal.altstack.ss_flags & SS_ONSTACK)) {
        sp          = (uintptr_t)proc->signal.altstack.ss_sp + proc->signal.altstack.ss_size;
        stack_limit = (uintptr_t)proc->signal.altstack.ss_sp;
    } else {
        sp          = frame->rsp;
        stack_limit = proc->stack_brk;
    }

    /* Align to 16 bytes */
    sp = (sp - 128) & ~(uint64_t)0xF;

    /*
     * Compute the final stack pointer after laying out siginfo_t,
     * ucontext reservation, and the return address.  We compute
     * first and validate the range before writing to user memory.
     */
    sp -= sizeof(siginfo_t);
    sp &= ~(uint64_t)0xF;
    uintptr_t siginfo_sp = sp;

    sp -= sizeof(uint64_t) * 8;
    sp &= ~(uint64_t)0xF;

    sp -= sizeof(uint64_t);
    sp &= ~(uint64_t)0xF;

    if (sp < stack_limit) return -EFAULT;

    /*
     * Write siginfo_t onto the user stack.
     */
    siginfo_t *user_info = (siginfo_t *)siginfo_sp;
    if (copy_to_user(user_info, info, sizeof(siginfo_t))) return -EFAULT;

    /*
     * Push return address (sigreturn trampoline) onto the user stack.
     */
    uint64_t *ret_addr = (uint64_t *)sp;
    uint64_t  restorer = sa->sa_restorer;
    if (!(sa->sa_flags & SA_RESTORER) || !restorer) { restorer = (uint64_t)0; }

    if (copy_to_user(ret_addr, &restorer, sizeof(uint64_t))) return -EFAULT;

    /*
     * Modify the syscall frame so that when iretq executes,
     * it jumps to the handler instead of the original return address.
     *
     * On x86_64, the syscall frame (pushed by the CPU on syscall entry)
     * has rip, cs, rflags, rsp, ss at the top. The handler arguments
     * go in rdi, rsi, rdx (which are in the frame too).
     */
    if (sa->sa_flags & SA_SIGINFO) {
        frame->rdi = (uint64_t)sig;
        frame->rsi = (uint64_t)user_info;
        frame->rdx = (uint64_t)NULL; /* ucontext pointer */
    } else {
        frame->rdi = (uint64_t)sig;
        frame->rsi = 0;
        frame->rdx = 0;
    }

    frame->rsp = sp;
    frame->rip = (uint64_t)sa->sa_handler;

    return 0;
}

/*
 * Handle the default action for a signal.
 * Returns 1 if the process should be terminated, 0 otherwise.
 */
static int signal_handle_default(process_t *proc, int sig)
{
    sig_dfl_action_t action = signal_default_action(sig);

    switch (action) {
        case SIG_DFL_IGN :
            return 0;

        case SIG_DFL_STOP :
            if (proc->task) { proc->task->state = TASK_BLOCKED; }
            return 0;

        case SIG_DFL_CONT :
            if (proc->task && proc->task->state == TASK_BLOCKED) {
                proc->task->state = TASK_READY;
                enqueue_task(proc->task);
            }
            return 0;

        case SIG_DFL_TERM :
        case SIG_DFL_CORE :
            /* Terminate process */
            proc->exit_code = -sig;
            if (proc->task) { proc->task->state = TASK_ZOMBIE; }
            return 1;

        default :
            return 0;
    }
}

/*
 * Deliver a single signal to the current process.
 * Called from signal_deliver_if_pending() when returning to userspace.
 *
 * Returns:
 *   0 - signal delivered to handler, continue execution
 *   1 - process terminated, do not return to userspace
 *  -1 - signal ignored or deferred
 */
static int signal_deliver_one(syscall_frame_t *frame, int sig, siginfo_t *info)
{
    process_t *proc = process_current();
    if (!proc) return -1;

    signal_state_t *state = &proc->signal;
    sigaction_t    *sa    = &state->sighand[sig];

    /* Deliver to signalfd */
    signalfd_deliver(proc, sig);

    /* Check if signal is ignored */
    if (sa->sa_handler == SIG_IGN) { goto clear_pending; }

    /* Check if signal is default */
    if (sa->sa_handler == SIG_DFL) { return signal_handle_default(proc, sig); }

    /* User handler: block the signal unless SA_NODEFER */
    if (!(sa->sa_flags & SA_NODEFER)) { sigaddset(&state->blocked, sig); }

    /* Block signals in sa_mask */
    sigorset(&state->blocked, &state->blocked, &sa->sa_mask);

    /* If SA_RESETHAND, reset handler to default */
    if (sa->sa_flags & SA_RESETHAND) {
        sa->sa_handler = SIG_DFL;
        sa->sa_flags   = 0;
    }

    /* Set up the signal frame on the user stack */
    signal_setup_frame(frame, sig, sa, info);

    /* Clear pending */
clear_pending:
    sigdelset(&state->pending, sig);
    return 0;
}

/*
 * Check if there is any pending signal that should be delivered.
 * Considers the blocked mask.
 */
int signal_has_pending(signal_state_t *state)
{
    if (!state) return 0;

    /* pending & ~blocked */
    sigset_t ready = state->pending & ~state->blocked;

    return !sigisemptyset(&ready);
}

/*
 * Dequeue the next pending signal to deliver.
 * Real-time signals are dequeued from the queue; standard signals
 * are found by scanning the pending bitmap.
 */
static int signal_dequeue(signal_state_t *state, siginfo_t *info)
{
    /* First, check real-time queue */
    if (state->sigqueue_head) {
        sigqueue_t *cur  = state->sigqueue_head;
        sigqueue_t *prev = NULL;
        while (cur) {
            int sig = cur->info.si_signo;
            if (!sigismember(&state->blocked, sig)) {
                /* Found a deliverable RT signal */
                memcpy(info, &cur->info, sizeof(siginfo_t));
                if (prev) {
                    prev->next = cur->next;
                } else {
                    state->sigqueue_head = cur->next;
                }
                if (cur == state->sigqueue_tail) { state->sigqueue_tail = prev; }
                state->sigqueue_count--;
                sigqueue_free(cur);
                sigdelset(&state->pending, sig);
                return sig;
            }
            prev = cur;
            cur  = cur->next;
        }
    }

    /* Check standard signals in priority order (lowest first) */
    sigset_t ready = state->pending & ~state->blocked;
    for (int sig = 1; sig < SIGRTMIN; sig++) {
        if (sigismember(&ready, sig)) {
            memset(info, 0, sizeof(siginfo_t));
            info->si_signo = sig;
            info->si_code  = SI_USER;
            sigdelset(&state->pending, sig);
            return sig;
        }
    }

    return -1;
}

/*
 * Main signal delivery entry point.
 * Called on every return from kernel to userspace (syscall return,
 * interrupt return). Modifies the syscall frame to redirect execution
 * to the signal handler.
 *
 * Returns:
 *  -EINTR if a signal was delivered and a syscall should be restarted
 *   0 if no signal delivered, continue normally
 *   1 if process was terminated
 */
int signal_deliver_if_pending(syscall_frame_t *frame)
{
    process_t *proc = process_current();
    if (!proc) return 0;

    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    if (!signal_has_pending(state)) {
        spin_unlock(&state->lock);
        return 0;
    }

    siginfo_t info;
    int       sig = signal_dequeue(state, &info);

    if (sig < 0) {
        spin_unlock(&state->lock);
        return 0;
    }

    spin_unlock(&state->lock);

    int ret = signal_deliver_one(frame, sig, &info);

    if (ret == 1) {
        /* Process terminated */
        return 1;
    }

    /* Check if the handler had SA_RESTART */
    sigaction_t *sa = &state->sighand[sig];
    if (sa->sa_flags & SA_RESTART) { return -ERESTART; }

    return -EINTR;
}

/* ---------- SIGCHLD notification ---------- */

void signal_notify_child_exit(process_t *parent, int64_t child_pid, int exit_code, int status)
{
    if (!parent) return;

    signal_state_t *state = &parent->signal;
    sigaction_t    *sa    = &state->sighand[SIGCHLD];

    spin_lock(&state->lock);

    /* Check SA_NOCLDWAIT */
    if (sa->sa_flags & SA_NOCLDWAIT) {
        spin_unlock(&state->lock);
        return;
    }

    state->child_exit_code    = exit_code;
    state->child_exit_pid     = child_pid;
    state->child_exit_status  = status;
    state->child_exit_pending = 1;

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo  = SIGCHLD;
    info.si_code   = CLD_EXITED;
    info.si_pid    = child_pid;
    info.si_uid    = parent->uid;
    info.si_status = exit_code;

    signal_send_locked(state, parent, SIGCHLD, &info);

    spin_unlock(&state->lock);

    if (parent->task) { task_wakeup(parent->task); }
}

/* ---------- Syscall implementations ---------- */

/*
 * sys_kill - Send a signal to a process
 *   pid > 0:  send to process with pid
 *   pid == 0: send to all processes in the same process group
 *   pid == -1: send to all processes (subject to permissions)
 *   pid < -1: send to all processes in process group -pid
 */
int64_t sys_kill_impl(int64_t pid, int sig)
{
    if (!sig_valid(sig)) return -EINVAL;

    if (pid > 0) {
        process_t *proc = process_find(pid);
        if (!proc) return -ESRCH;

        if (signal_check_perm(process_current(), proc) < 0) return -EPERM;

        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code  = SI_USER;
        info.si_pid   = process_current()->task->pid;
        info.si_uid   = process_current()->uid;

        return signal_send(proc, sig, &info);
    }

    if (pid == 0) {
        /* Send to process group */
        process_t *cur = process_current();
        if (!cur) return -ESRCH;

        size_t     pos = 0;
        process_t *target;
        int        ret   = 0;
        int        found = 0;

        while ((target = process_iterate(&pos))) {
            if (target->pgid == cur->pgid) {
                siginfo_t info;
                memset(&info, 0, sizeof(info));
                info.si_signo = sig;
                info.si_code  = SI_USER;
                info.si_pid   = cur->task->pid;
                info.si_uid   = cur->uid;
                ret           = signal_send(target, sig, &info);
                found         = 1;
            }
        }
        return found ? ret : -ESRCH;
    }

    if (pid == -1) {
        /* Send to all processes (except self and init) */
        process_t *cur = process_current();
        if (!cur) return -ESRCH;

        size_t     pos = 0;
        process_t *target;
        int        found = 0;

        while ((target = process_iterate(&pos))) {
            if (target == cur) continue;
            if (target->task->pid == 1) continue;
            if (signal_check_perm(cur, target) < 0) continue;

            siginfo_t info;
            memset(&info, 0, sizeof(info));
            info.si_signo = sig;
            info.si_code  = SI_USER;
            info.si_pid   = cur->task->pid;
            info.si_uid   = cur->uid;
            signal_send(target, sig, &info);
            found = 1;
        }
        return found ? 0 : -ESRCH;
    }

    /* pid < -1: send to process group -pid */
    {
        pid_t      pgid = -pid;
        process_t *cur  = process_current();
        if (!cur) return -ESRCH;

        size_t     pos = 0;
        process_t *target;
        int        found = 0;

        while ((target = process_iterate(&pos))) {
            if (target->pgid == pgid) {
                if (signal_check_perm(cur, target) < 0) continue;

                siginfo_t info;
                memset(&info, 0, sizeof(info));
                info.si_signo = sig;
                info.si_code  = SI_USER;
                info.si_pid   = cur->task->pid;
                info.si_uid   = cur->uid;
                signal_send(target, sig, &info);
                found = 1;
            }
        }
        return found ? 0 : -ESRCH;
    }
}

/*
 * sys_tkill - Send a signal to a specific thread
 */
int64_t sys_tkill_impl(int64_t tid, int sig)
{
    if (!sig_valid(sig)) return -EINVAL;

    process_t *proc = process_find(tid);
    if (!proc) return -ESRCH;

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code  = SI_TKILL;
    info.si_pid   = process_current()->task->pid;
    info.si_uid   = process_current()->uid;

    return signal_send_thread(proc->task, sig, &info);
}

/*
 * sys_tgkill - Send a signal to a specific thread in a specific process
 */
int64_t sys_tgkill(int64_t tgid, int64_t tid, int sig)
{
    if (!sig_valid(sig)) return -EINVAL;

    process_t *proc = process_find(tgid);
    if (!proc) return -ESRCH;

    if (proc->task->pid != (uint64_t)tid) {
        /* In this kernel, pid == tid (1:1 mapping), so this is an error */
        return -ESRCH;
    }

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code  = SI_TKILL;
    info.si_pid   = process_current()->task->pid;
    info.si_uid   = process_current()->uid;

    return signal_send_thread(proc->task, sig, &info);
}

/*
 * sys_rt_sigaction - Examine and change a signal action
 */
int64_t sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oact, size_t sigsetsize)
{
    if (!sig_valid(sig)) return -EINVAL;
    if (sig_is_uncatchable(sig)) return -EINVAL;
    if (sigsetsize != sizeof(sigset_t)) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    if (oact) {
        if (copy_to_user(oact, &state->sighand[sig], sizeof(sigaction_t))) {
            spin_unlock(&state->lock);
            return -EFAULT;
        }
    }

    if (act) {
        sigaction_t new_sa;
        if (copy_from_user(&new_sa, act, sizeof(sigaction_t))) {
            spin_unlock(&state->lock);
            return -EFAULT;
        }

        /* Validate */
        if (new_sa.sa_handler == SIG_ERR) {
            spin_unlock(&state->lock);
            return -EINVAL;
        }

        /* SA_NOCLDSTOP and SA_NOCLDWAIT only meaningful for SIGCHLD */
        if (sig != SIGCHLD) { new_sa.sa_flags &= ~(SA_NOCLDSTOP | SA_NOCLDWAIT); }

        state->sighand[sig] = new_sa;
    }

    spin_unlock(&state->lock);
    return 0;
}

/*
 * sys_rt_sigprocmask - Examine and change blocked signals
 */
int64_t sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset, size_t sigsetsize)
{
    if (sigsetsize != sizeof(sigset_t)) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    if (oset) {
        if (copy_to_user(oset, &state->blocked, sizeof(sigset_t))) {
            spin_unlock(&state->lock);
            return -EFAULT;
        }
    }

    if (set) {
        sigset_t new_set;
        if (copy_from_user(&new_set, set, sizeof(sigset_t))) {
            spin_unlock(&state->lock);
            return -EFAULT;
        }

        switch (how) {
            case SIG_BLOCK :
                sigorset(&state->blocked, &state->blocked, &new_set);
                break;
            case SIG_UNBLOCK :
                state->blocked &= ~new_set;
                break;
            case SIG_SETMASK :
                state->blocked = new_set;
                break;
            default :
                spin_unlock(&state->lock);
                return -EINVAL;
        }

        /* SIGKILL and SIGSTOP cannot be blocked */
        sigdelset(&state->blocked, SIGKILL);
        sigdelset(&state->blocked, SIGSTOP);
    }

    spin_unlock(&state->lock);
    return 0;
}

/*
 * sys_rt_sigpending - Examine pending signals
 */
int64_t sys_rt_sigpending(sigset_t *set, size_t sigsetsize)
{
    if (sigsetsize != sizeof(sigset_t)) return -EINVAL;
    if (!set) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);
    sigset_t pending = state->pending;
    spin_unlock(&state->lock);

    if (copy_to_user(set, &pending, sizeof(sigset_t))) return -EFAULT;
    return 0;
}

/*
 * sys_rt_sigsuspend - Wait for a signal
 *
 * Atomically replaces the blocked signal mask with 'set' and
 * waits for a signal. When a signal is delivered and handled,
 * returns -EINTR.
 */
int64_t sys_rt_sigsuspend(const sigset_t *set, size_t sigsetsize)
{
    if (sigsetsize != sizeof(sigset_t)) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    signal_state_t *state = &proc->signal;

    sigset_t new_mask;
    if (copy_from_user(&new_mask, set, sizeof(sigset_t))) return -EFAULT;

    spin_lock(&state->lock);

    /* Save old blocked */
    sigset_t old_blocked = state->blocked;

    /* Set new blocked mask */
    state->blocked = new_mask;
    sigdelset(&state->blocked, SIGKILL);
    sigdelset(&state->blocked, SIGSTOP);

    /* Check if any signal is already pending and unblocked */
    if (signal_has_pending(state)) {
        state->blocked = old_blocked;
        spin_unlock(&state->lock);
        return -EINTR;
    }

    spin_unlock(&state->lock);

    /* Block until a signal arrives */
    task_block();

    /* Restore old blocked mask */
    spin_lock(&state->lock);
    state->blocked = old_blocked;
    spin_unlock(&state->lock);

    return -EINTR;
}

/*
 * sys_rt_sigtimedwait - Synchronously wait for queued signals
 */
int64_t sys_rt_sigtimedwait(const sigset_t *set, siginfo_t *info, const void *timeout, size_t sigsetsize)
{
    if (sigsetsize != sizeof(sigset_t)) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    signal_state_t *state = &proc->signal;

    sigset_t wait_set;
    if (copy_from_user(&wait_set, set, sizeof(sigset_t))) return -EFAULT;

    /* For now, non-blocking poll */
    spin_lock(&state->lock);

    sigset_t ready = state->pending & wait_set;
    if (sigisemptyset(&ready)) {
        spin_unlock(&state->lock);
        if (timeout) {
            /* Non-blocking: return immediately */
            return -EAGAIN;
        }
        /* Blocking wait without timeout: block until signal */
        spin_unlock(&state->lock);
        task_block();
        return -EINTR;
    }

    /* Dequeue the first matching signal */
    siginfo_t found;
    memset(&found, 0, sizeof(found));

    for (int sig = 1; sig <= SIGRTMAX; sig++) {
        if (sigismember(&ready, sig)) {
            found.si_signo = sig;
            found.si_code  = SI_USER;
            sigdelset(&state->pending, sig);
            break;
        }
    }

    spin_unlock(&state->lock);

    if (info && copy_to_user(info, &found, sizeof(siginfo_t))) return -EFAULT;
    return found.si_signo;
}

/*
 * sys_sigaltstack - Set or get the alternate signal stack
 */
int64_t sys_sigaltstack(const stack_t *ss, stack_t *oss)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    if (oss) {
        if (copy_to_user(oss, &state->altstack, sizeof(stack_t))) {
            spin_unlock(&state->lock);
            return -EFAULT;
        }
    }

    if (ss) {
        stack_t new_ss;
        if (copy_from_user(&new_ss, ss, sizeof(stack_t))) {
            spin_unlock(&state->lock);
            return -EFAULT;
        }

        if (new_ss.ss_flags & SS_DISABLE) {
            state->altstack.ss_flags = SS_DISABLE;
            state->altstack.ss_sp    = NULL;
            state->altstack.ss_size  = 0;
        } else {
            if (new_ss.ss_size < (size_t)4096) {
                spin_unlock(&state->lock);
                return -ENOMEM;
            }
            state->altstack = new_ss;
        }
    }

    spin_unlock(&state->lock);
    return 0;
}

/*
 * sys_pause - Wait for a signal
 */
int64_t sys_pause(void)
{
    task_block();
    return -EINTR;
}

/*
 * sys_rt_sigqueueinfo - Queue a signal with data
 */
int64_t sys_rt_sigqueueinfo(int64_t pid, int sig, siginfo_t *info)
{
    if (!sig_valid(sig)) return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    process_t *proc = process_find((int64_t)pid);
    if (!proc) return -ESRCH;

    siginfo_t user_info;
    if (copy_from_user(&user_info, info, sizeof(siginfo_t))) return -EFAULT;

    user_info.si_signo = sig;
    if (user_info.si_code >= 0) return -EPERM;

    process_t *cur = process_current();
    if (signal_check_perm(cur, proc) < 0) return -EPERM;

    return signal_send(proc, sig, &user_info);
}

/*
 * sys_rt_tgsigqueueinfo - Queue a signal with data to a specific thread
 */
int64_t sys_rt_tgsigqueueinfo(int64_t tgid, int64_t tid, int sig, siginfo_t *info)
{
    if (!sig_valid(sig)) return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    process_t *proc = process_find(tgid);
    if (!proc) return -ESRCH;

    if ((int64_t)proc->task->pid != tid) return -ESRCH;

    siginfo_t user_info;
    if (copy_from_user(&user_info, info, sizeof(siginfo_t))) return -EFAULT;

    user_info.si_signo = sig;
    if (user_info.si_code >= 0) return -EPERM;

    process_t *cur = process_current();
    if (signal_check_perm(cur, proc) < 0) return -EPERM;

    return signal_send_thread(proc->task, sig, &user_info);
}

/*
 * sys_rt_sigreturn - Return from signal handler and restore context
 *
 * This is called by the user-space signal trampoline after the
 * signal handler returns. It restores the saved context and
 * returns to the point where the process was interrupted.
 */
int64_t sys_rt_sigreturn(void)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    task_t *task = proc->task;
    if (!task) return -ESRCH;

    /*
     * In a full implementation, we would restore the saved context
     * from the signal frame on the user stack. Since we don't
     * save a full context in this minimal implementation, we
     * just return 0 to resume execution.
     *
     * The actual context restoration would involve:
     * 1. Reading the saved ucontext from the user stack
     * 2. Restoring all registers (RIP, RSP, RFLAGS, etc.)
     * 3. Restoring the blocked signal mask
     * 4. Returning to the original execution point
     */

    (void)task;
    return 0;
}

/*
 * sys_setpgid - Set process group ID
 */
int64_t sys_setpgid(int64_t pid, int64_t pgid)
{
    process_t *proc;
    if (pid == 0) {
        proc = process_current();
    } else {
        proc = process_find(pid);
    }

    if (!proc) return -ESRCH;

    if (pgid == 0) pgid = (int64_t)proc->task->pid;

    proc->pgid = pgid;
    return 0;
}

/*
 * sys_getpgrp - Get process group ID
 */
int64_t sys_getpgrp(void)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return (int64_t)proc->pgid;
}

/*
 * sys_setsid - Create a new session
 */
int64_t sys_setsid(void)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    pid_t my_pid = (pid_t)proc->task->pid;

    /* Check if already session leader */
    if (proc->sid == my_pid) return -EPERM;

    /* Check if process group leader */
    if (proc->pgid == my_pid) return -EPERM;

    proc->sid  = my_pid;
    proc->pgid = my_pid;

    return (int64_t)my_pid;
}

/*
 * sys_getsid - Get session ID
 */
int64_t sys_getsid(int64_t pid)
{
    process_t *proc;
    if (pid == 0) {
        proc = process_current();
    } else {
        proc = process_find(pid);
    }

    if (!proc) return -ESRCH;
    return (int64_t)proc->sid;
}

/*
 * sys_getpgid - Get process group ID
 */
int64_t sys_getpgid(int64_t pid)
{
    process_t *proc;
    if (pid == 0) {
        proc = process_current();
    } else {
        proc = process_find(pid);
    }

    if (!proc) return -ESRCH;
    return (int64_t)proc->pgid;
}