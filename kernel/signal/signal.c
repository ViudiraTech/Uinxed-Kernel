/*
 *
 *      signal.c
 *      POSIX signal subsystem implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
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
#include <proc/ptrace.h>
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

static int signal_send_group(int64_t pgid, int64_t sid, int sig, process_t *sender, int code);

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

static int sigqueue_push(signal_state_t *state, const siginfo_t *info)
{
    sigqueue_t *q = sigqueue_alloc();
    if (!q) {
        plogk("signal: sigqueue allocation failed (sig %d)\n", info->si_signo);
        return -ENOMEM;
    }

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
    return 0;
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
    plogk("signal: POSIX signal handling available (%u signals)\n", NSIG);
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
    if (dst != src) spin_lock(&dst->lock);

    memcpy(dst->sighand, src->sighand, sizeof(dst->sighand));
    /* POSIX fork inheritance copies dispositions and the signal mask, but the
     * child starts with no pending signals and no inherited child-status
     * notification.  Copying the parent's SIGCHLD here made freshly forked
     * helpers spuriously interrupt their first blocking syscall. */
    sigemptyset(&dst->pending);
    dst->blocked      = src->restore_mask ? src->saved_mask : src->blocked;
    dst->saved_mask   = 0;
    dst->restore_mask = false;

    sigqueue_flush(dst);

    dst->altstack           = src->altstack;
    dst->child_exit_code    = 0;
    dst->child_exit_pending = 0;
    dst->child_exit_pid     = 0;
    dst->child_exit_status  = 0;

    if (dst != src) spin_unlock(&dst->lock);
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

/*
 * signal_exec_reset - Reset signal state for execve.
 *
 * Per POSIX: all signal handlers are reset to SIG_DFL (except
 * SIG_IGN which remains SIG_IGN), all pending signals are flushed,
 * and the alternate stack is disabled. The signal mask is preserved.
 */
void signal_exec_reset(process_t *proc)
{
    if (!proc) return;
    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    if (state->restore_mask) {
        state->blocked      = state->saved_mask;
        state->restore_mask = false;
    }

    sigemptyset(&state->pending);
    sigqueue_flush(state);

    for (int i = 0; i < SIG_ACTION_NUM; i++) {
        sig_handler_t cur = state->sighand[i].sa_handler;
        if (cur != SIG_IGN) {
            state->sighand[i].sa_handler = SIG_DFL;
            state->sighand[i].sa_flags   = 0;
            sigemptyset(&state->sighand[i].sa_mask);
        }
    }

    state->altstack.ss_sp    = NULL;
    state->altstack.ss_size  = 0;
    state->altstack.ss_flags = SS_DISABLE;

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
    /* Standard signals coalesce, but Linux retains the first siginfo. */
    if (!sig_is_rt(sig)) {
        if (sigismember(&state->pending, sig)) { return 0; }
    }

    /* Real-time signals: queue up to SIGQUEUE_MAX */
    if (sig_is_rt(sig) && state->sigqueue_count >= SIGQUEUE_MAX) return -EAGAIN;

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

    int queued = state->sigqueue_count < SIGQUEUE_MAX ? sigqueue_push(state, &queue_info) : -ENOMEM;
    if (queued && sig_is_rt(sig)) return -EAGAIN;
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

    bool resumed = false;
    if (ret == 0 && (sig == SIGCONT || sig == SIGKILL) && proc->task) resumed = task_continue(proc->task) == 0;
    if (resumed && sig == SIGCONT) process_child_continued(proc);

    if (ret == 0 && !resumed) {
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

    bool resumed = false;
    if (ret == 0 && (sig == SIGCONT || sig == SIGKILL)) resumed = task_continue(task) == 0;
    if (resumed && sig == SIGCONT) process_child_continued(proc);
    if (ret == 0 && !resumed) task_wakeup(task);

    return ret;
}

/* ---------- Signal delivery ---------- */

/*
 * Prepare a signal frame on the user stack and modify the syscall
 * frame so that the process enters the signal handler when it
 * returns to userspace.
 *
 * We save the full register context + old signal mask into a
 * signal_user_frame_t on the user stack, then redirect the syscall
 * frame's rip/rsp/rdi/rsi/rdx so the handler runs.
 *
 * The saved context is restored by sys_rt_sigreturn().
 *
 * NOTE: The interrupt-delivery path (user_exception, page_fault_handle)
 * creates a minimal syscall_frame_t with only rip/cs/rflags/rsp/ss.
 * For those paths, rdi/rsi/rdx/rax/rbx/rcx in the sigframe are 0;
 * the important thing is that rip/rflags/rsp are saved and restored
 * correctly. The signal handler will get its signal number in rdi
 * from the caller (which propagates sigframe.rdi back).
 */
static int signal_setup_frame(syscall_frame_t *frame, int sig, const sigaction_t *sa, const siginfo_t *info, sigset_t old_mask)
{
    process_t *proc = process_current();
    if (!proc || !frame) return -ESRCH;

    uintptr_t sp, stack_limit;

    if ((sa->sa_flags & SA_ONSTACK) && !(proc->signal.altstack.ss_flags & SS_DISABLE) && !(proc->signal.altstack.ss_flags & SS_ONSTACK)) {
        sp          = (uintptr_t)proc->signal.altstack.ss_sp + proc->signal.altstack.ss_size;
        stack_limit = (uintptr_t)proc->signal.altstack.ss_sp;
    } else {
        sp          = frame->rsp;
        stack_limit = proc->stack_brk;
    }

    /* Align to 16 bytes, leave 128-byte red zone (x86_64 ABI) */
    sp = (sp - 128) & ~(uint64_t)0xF;

    sp -= sizeof(signal_user_frame_t);
    sp &= ~(uint64_t)0xF;

    if (sp < stack_limit) return -EFAULT;

    /* Build the signal frame on the kernel stack first */
    signal_user_frame_t sig_frame;
    memset(&sig_frame, 0, sizeof(sig_frame));

    /* Return address for the handler (restorer trampoline) */
    uint64_t restorer = 0;
    if ((sa->sa_flags & SA_RESTORER) && sa->sa_restorer) { restorer = sa->sa_restorer; }
    sig_frame.pretcode = restorer;

    /* Copy siginfo */
    memcpy(&sig_frame.info, info, sizeof(siginfo_t));

    /* Save old blocked mask (from BEFORE this signal's mask was applied) */
    sig_frame.old_mask = old_mask;

    /* Save full register context from the syscall/interrupt frame */
    sig_frame.rax    = frame->rax;
    sig_frame.rbx    = frame->rbx;
    sig_frame.rcx    = frame->rcx;
    sig_frame.rdx    = frame->rdx;
    sig_frame.rsi    = frame->rsi;
    sig_frame.rdi    = frame->rdi;
    sig_frame.rbp    = frame->rbp;
    sig_frame.r8     = frame->r8;
    sig_frame.r9     = frame->r9;
    sig_frame.r10    = frame->r10;
    sig_frame.r11    = frame->r11;
    sig_frame.r12    = frame->r12;
    sig_frame.r13    = frame->r13;
    sig_frame.r14    = frame->r14;
    sig_frame.r15    = frame->r15;
    sig_frame.rip    = frame->rip;
    sig_frame.rflags = frame->rflags;
    sig_frame.rsp    = frame->rsp;
    sig_frame.cs     = frame->cs;
    sig_frame.ss     = frame->ss;

    /* Write the entire frame to user stack */
    if (copy_to_user((void *)sp, &sig_frame, sizeof(signal_user_frame_t))) return -EFAULT;

    /* Set up handler arguments */
    frame->rdi = (uint64_t)sig;
    if (sa->sa_flags & SA_SIGINFO) {
        frame->rsi = sp + offsetof(signal_user_frame_t, info);
        frame->rdx = sp + offsetof(signal_user_frame_t, old_mask);
    } else {
        frame->rsi = 0;
        frame->rdx = 0;
    }

    /* Redirect to handler */
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
            if (proc->task) {
                spin_lock(&scheduler.lock);
                bool stopped      = proc->task->state != TASK_STOPPED;
                proc->task->state = TASK_STOPPED;
                spin_unlock(&scheduler.lock);
                if (stopped) process_child_stopped(proc, sig);
            }
            return 0;

        case SIG_DFL_CONT :
            return 0;

        case SIG_DFL_TERM :
        case SIG_DFL_CORE :
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
 *   SIG_DELIV_HANDLED (0) - default/ignore action applied, continue
 *   SIG_DELIV_TERM    (1) - process terminated by default action
 *   SIG_DELIV_HANDLER (2) - user handler set up, frame modified
 */
static int signal_deliver_one(syscall_frame_t *frame, int sig, siginfo_t *info)
{
    process_t *proc = process_current();
    if (!proc) return SIG_DELIV_HANDLED;

    signal_state_t *state = &proc->signal;
    sigaction_t    *sa    = &state->sighand[sig];

    /* Deliver to signalfd */
    signalfd_deliver(proc, sig);

    /* Check if signal is ignored */
    if (sa->sa_handler == SIG_IGN) {
        sigdelset(&state->pending, sig);
        return SIG_DELIV_HANDLED;
    }

    /* Check if signal is default */
    if (sa->sa_handler == SIG_DFL) {
        int ret = signal_handle_default(proc, sig);
        sigdelset(&state->pending, sig);
        if (ret == 1) return SIG_DELIV_TERM;
        return SIG_DELIV_HANDLED;
    }

    /* User handler: save old mask before modifying */
    sigset_t old_mask = state->restore_mask ? state->saved_mask : state->blocked;

    /* Block the signal itself unless SA_NODEFER */
    if (!(sa->sa_flags & SA_NODEFER)) { sigaddset(&state->blocked, sig); }

    /* Block additional signals in sa_mask */
    sigorset(&state->blocked, &state->blocked, &sa->sa_mask);

    /* If SA_RESETHAND, reset handler to default before invoking */
    if (sa->sa_flags & SA_RESETHAND) {
        sa->sa_handler = SIG_DFL;
        sa->sa_flags   = 0;
    }

    /* Set up the signal frame on the user stack (saves old_mask for sigreturn) */
    if (signal_setup_frame(frame, sig, sa, info, old_mask) < 0) {
        plogk("signal: failed to set up frame for sig %d pid %llu\n", sig, proc->task ? proc->task->pid : 0);
    }
    state->restore_mask = false;

    /* Clear pending bit for this signal */
    sigdelset(&state->pending, sig);

    return SIG_DELIV_HANDLER;
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

int signal_has_interrupting_pending(signal_state_t *state)
{
    if (!state) return 0;

    sigset_t ready = state->pending & ~state->blocked;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!sigismember(&ready, sig)) continue;

        sigaction_t *action = &state->sighand[sig];
        if (action->sa_handler == SIG_IGN) continue;
        if (action->sa_handler != SIG_DFL) return 1;

        sig_dfl_action_t disposition = signal_default_action(sig);
        if (disposition == SIG_DFL_TERM || disposition == SIG_DFL_CORE || disposition == SIG_DFL_STOP) return 1;
    }
    return 0;
}

bool signal_is_blocked_or_ignored(process_t *proc, int sig)
{
    if (!proc || !sig_valid(sig)) return false;

    signal_state_t *state = &proc->signal;
    spin_lock(&state->lock);
    bool blocked_or_ignored = sigismember(&state->blocked, sig) || state->sighand[sig].sa_handler == SIG_IGN;
    spin_unlock(&state->lock);
    return blocked_or_ignored;
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
            sigqueue_t *cur  = state->sigqueue_head;
            sigqueue_t *prev = NULL;
            while (cur && cur->info.si_signo != sig) {
                prev = cur;
                cur  = cur->next;
            }
            if (cur) {
                memcpy(info, &cur->info, sizeof(*info));
                if (prev)
                    prev->next = cur->next;
                else
                    state->sigqueue_head = cur->next;
                if (state->sigqueue_tail == cur) state->sigqueue_tail = prev;
                state->sigqueue_count--;
                sigqueue_free(cur);
            } else {
                memset(info, 0, sizeof(*info));
                info->si_signo = sig;
                info->si_code  = SI_USER;
            }
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
 * to the signal handler if needed.
 *
 * IMPORTANT: This function does NOT modify frame->rax (the syscall
 * return value). The decision about whether to return -EINTR or
 * restart a syscall is made by the caller (syscall_dispatch based on
 * the syscall's own return value).
 *
 * Delivery rules (matching Linux behavior):
 * - SIG_IGN / default non-terminating actions: all such pending signals
 *   are cleared in one call (no need to return to userspace between them).
 * - Default terminating actions: process exits immediately.
 * - User handler: only ONE signal is delivered per call. The remaining
 *   pending signals will be delivered on the next return to userspace.
 *
 * Returns:
 *   0 if no signal was pending, or signal was delivered (continue)
 *   1 if the process was terminated by a signal default action
 */
int signal_deliver_if_pending(syscall_frame_t *frame)
{
    process_t *proc = process_current();
    if (!proc) return 0;

    signal_state_t *state = &proc->signal;

    spin_lock(&state->lock);

    while (signal_has_pending(state)) {
        siginfo_t info;
        int       sig = signal_dequeue(state, &info);

        if (sig < 0) break;

        /* A tracer observes the signal before normal disposition.  The
         * tracee sleeps outside signal.lock and resumes with either a
         * replacement signal or zero to suppress delivery. */
        if (ptrace_tracer_pid(current_task()) && sig != SIGKILL) {
            spin_unlock(&state->lock);
            int injected = ptrace_signal_delivery(frame, sig, &info);
            spin_lock(&state->lock);
            if (!injected) continue;
            sig           = injected;
            info.si_signo = injected;
        }

        int ret = signal_deliver_one(frame, sig, &info);

        if (ret == SIG_DELIV_TERM) {
            /* Default action terminates process */
            spin_unlock(&state->lock);
            process_exit(-sig);
            return 1; /* Never reached */
        }

        if (ret == SIG_DELIV_HANDLER) {
            /* User handler set up: deliver ONLY this signal, leave rest pending.
             * The handler will run when we return to userspace; on the next
             * syscall/interrupt return, remaining signals will be delivered. */
            break;
        }

        /* ret == SIG_DELIV_HANDLED: default/ignore action.
         * Continue loop to clear more pending default/ignore signals. */
    }

    /* No user handler consumed the deferred mask (for example, the signal
     * was ignored or used a non-terminating default action). */
    if (state->restore_mask) {
        state->blocked      = state->saved_mask;
        state->restore_mask = false;
    }

    bool stopped = proc->task && proc->task->state == TASK_STOPPED;
    spin_unlock(&state->lock);
    if (stopped) sched_yield();
    return 0;
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

void signal_notify_child_status(process_t *parent, int64_t child_pid, int status, int code)
{
    if (!parent) return;

    signal_state_t *state = &parent->signal;
    spin_lock(&state->lock);
    sigaction_t *sa = &state->sighand[SIGCHLD];
    if (!(sa->sa_flags & SA_NOCLDSTOP)) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo  = SIGCHLD;
        info.si_code   = code;
        info.si_pid    = child_pid;
        info.si_status = status;
        signal_send_locked(state, parent, SIGCHLD, &info);
    }
    spin_unlock(&state->lock);
    if (parent->task) task_wakeup(parent->task);
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
        process_t *proc = process_find_get(pid);
        if (!proc) return -ESRCH;

        process_t *cur = process_current();
        if (!cur) {
            process_put(proc);
            return -ESRCH;
        }
        if (signal_check_perm(cur, proc) < 0) {
            process_put(proc);
            return -EPERM;
        }

        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code  = SI_USER;
        info.si_pid   = cur->task->pid;
        info.si_uid   = cur->uid;

        int ret = signal_send(proc, sig, &info);
        process_put(proc);
        return ret;
    }

    if (pid == 0) {
        process_t *cur = process_current();
        if (!cur) return -ESRCH;
        return signal_send_group(cur->pgid, 0, sig, cur, SI_USER);
    }

    if (pid == -1) {
        /* Send to all processes (except self and init) */
        process_t *cur = process_current();
        if (!cur) return -ESRCH;

        size_t     pos = 0;
        process_t *target;
        int        found = 0;

        while ((target = process_iterate_get(&pos))) {
            if (target == cur || !target->task || target->task->pid == 1 || signal_check_perm(cur, target) < 0) {
                process_put(target);
                continue;
            }

            siginfo_t info;
            memset(&info, 0, sizeof(info));
            info.si_signo = sig;
            info.si_code  = SI_USER;
            info.si_pid   = cur->task->pid;
            info.si_uid   = cur->uid;
            signal_send(target, sig, &info);
            found = 1;
            process_put(target);
        }
        return found ? 0 : -ESRCH;
    }

    /* pid < -1: send to process group -pid */
    {
        pid_t      pgid = -pid;
        process_t *cur  = process_current();
        if (!cur) return -ESRCH;

        return signal_send_group(pgid, 0, sig, cur, SI_USER);
    }
}

/*
 * sys_tkill - Send a signal to a specific thread
 */
int64_t sys_tkill_impl(int64_t tid, int sig)
{
    if (!sig_valid(sig)) return -EINVAL;

    process_t *target = process_find_get(tid);
    if (!target) return -ESRCH;

    process_t *cur = process_current();
    if (!cur) {
        process_put(target);
        return -ESRCH;
    }
    if (signal_check_perm(cur, target) < 0) {
        process_put(target);
        return -EPERM;
    }

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code  = SI_TKILL;
    info.si_pid   = cur->task->pid;
    info.si_uid   = cur->uid;

    int ret = signal_send_thread(target->task, sig, &info);
    process_put(target);
    return ret;
}

/*
 * sys_tgkill - Send a signal to a specific thread in a specific process
 */
int64_t sys_tgkill(int64_t tgid, int64_t tid, int sig)
{
    if (!sig_valid(sig)) return -EINVAL;

    process_t *target = process_find_get(tgid);
    if (!target) return -ESRCH;

    if (!target->task || target->task->pid != (uint64_t)tid) {
        process_put(target);
        return -ESRCH;
    }

    process_t *cur = process_current();
    if (!cur) {
        process_put(target);
        return -ESRCH;
    }
    if (signal_check_perm(cur, target) < 0) {
        process_put(target);
        return -EPERM;
    }

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code  = SI_TKILL;
    info.si_pid   = cur->task->pid;
    info.si_uid   = cur->uid;

    int ret = signal_send_thread(target->task, sig, &info);
    process_put(target);
    return ret;
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
 * waits for a signal, using the wait queue to avoid the TOCTOU
 * race between checking for pending signals and blocking.
 * Always returns -EINTR.
 */
int64_t sys_rt_sigsuspend(const sigset_t *set, size_t sigsetsize)
{
    if (sigsetsize != sizeof(sigset_t)) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    signal_state_t *state = &proc->signal;

    sigset_t new_mask;
    if (copy_from_user(&new_mask, set, sizeof(sigset_t))) return -EFAULT;

    wait_queue_t wq;
    wait_queue_init(&wq);

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

    /* Atomically prepare to sleep: if a signal_send calls task_wakeup
     * between here and wait_queue_sleep(), the wakeup will be recorded
     * in the wait queue and wait_queue_sleep() will not block. */
    wait_queue_prepare(&wq);
    spin_unlock(&state->lock);

    /* Sleep (or continue immediately if already woken by a signal) */
    wait_queue_sleep();

    /* Restore old blocked mask */
    spin_lock(&state->lock);
    state->blocked = old_blocked;
    spin_unlock(&state->lock);

    return -EINTR;
}

/*
 * Dequeue the first signal from the sigqueue that matches `filter`.
 * Returns the signal number and fills `info`, or 0 if nothing matches.
 * Caller must hold state->lock.
 */
static int sigqueue_dequeue_filtered(signal_state_t *state, const sigset_t *filter, siginfo_t *info)
{
    sigqueue_t *cur  = state->sigqueue_head;
    sigqueue_t *prev = NULL;

    while (cur) {
        int sig = cur->info.si_signo;
        if (sigismember(filter, sig) && !sigismember(&state->blocked, sig)) {
            memcpy(info, &cur->info, sizeof(siginfo_t));
            if (prev)
                prev->next = cur->next;
            else
                state->sigqueue_head = cur->next;
            if (cur == state->sigqueue_tail) state->sigqueue_tail = prev;
            state->sigqueue_count--;
            sigqueue_free(cur);
            sigdelset(&state->pending, sig);
            return sig;
        }
        prev = cur;
        cur  = cur->next;
    }
    return 0;
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

    if (timeout) {
        /* Non-blocking poll */
        spin_lock(&state->lock);
        siginfo_t found;
        memset(&found, 0, sizeof(found));
        int sig = sigqueue_dequeue_filtered(state, &wait_set, &found);
        spin_unlock(&state->lock);
        if (!sig) return -EAGAIN;
        if (info && copy_to_user(info, &found, sizeof(siginfo_t))) return -EFAULT;
        return sig;
    }

    /* Blocking wait (no timeout): use wait queue for atomic sleep */
    wait_queue_t wq;
    wait_queue_init(&wq);

    spin_lock(&state->lock);
    siginfo_t found;
    memset(&found, 0, sizeof(found));
    int sig = sigqueue_dequeue_filtered(state, &wait_set, &found);
    if (sig) {
        spin_unlock(&state->lock);
        if (info && copy_to_user(info, &found, sizeof(siginfo_t))) return -EFAULT;
        return sig;
    }

    /* No matching signals pending; prepare to sleep */
    wait_queue_prepare(&wq);
    spin_unlock(&state->lock);

    wait_queue_sleep();

    /* Re-check after wakeup */
    spin_lock(&state->lock);
    memset(&found, 0, sizeof(found));
    sig = sigqueue_dequeue_filtered(state, &wait_set, &found);
    spin_unlock(&state->lock);
    if (sig) {
        if (info && copy_to_user(info, &found, sizeof(siginfo_t))) return -EFAULT;
        return sig;
    }
    return -EINTR;
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
 *
 * Uses the wait queue to avoid the TOCTOU race between checking
 * for pending signals and blocking. Always returns -EINTR.
 */
int64_t sys_pause(void)
{
    process_t *proc = process_current();
    if (!proc) return -EINTR;

    wait_queue_t wq;
    wait_queue_init(&wq);

    signal_state_t *state = &proc->signal;
    spin_lock(&state->lock);
    if (signal_has_pending(state)) {
        spin_unlock(&state->lock);
        return -EINTR;
    }
    wait_queue_prepare(&wq);
    spin_unlock(&state->lock);

    wait_queue_sleep();
    return -EINTR;
}

/*
 * sys_rt_sigqueueinfo - Queue a signal with data
 */
int64_t sys_rt_sigqueueinfo(int64_t pid, int sig, siginfo_t *info)
{
    if (!sig_valid(sig)) return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    process_t *proc = process_find_get((int64_t)pid);
    if (!proc) return -ESRCH;

    siginfo_t user_info;
    if (copy_from_user(&user_info, info, sizeof(siginfo_t))) {
        process_put(proc);
        return -EFAULT;
    }

    user_info.si_signo = sig;
    if (user_info.si_code >= 0) {
        process_put(proc);
        return -EPERM;
    }

    process_t *cur = process_current();
    if (signal_check_perm(cur, proc) < 0) {
        process_put(proc);
        return -EPERM;
    }

    int ret = signal_send(proc, sig, &user_info);
    process_put(proc);
    return ret;
}

/*
 * sys_rt_tgsigqueueinfo - Queue a signal with data to a specific thread
 */
int64_t sys_rt_tgsigqueueinfo(int64_t tgid, int64_t tid, int sig, siginfo_t *info)
{
    if (!sig_valid(sig)) return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    process_t *proc = process_find_get(tgid);
    if (!proc) return -ESRCH;

    if (!proc->task || (int64_t)proc->task->pid != tid) {
        process_put(proc);
        return -ESRCH;
    }

    siginfo_t user_info;
    if (copy_from_user(&user_info, info, sizeof(siginfo_t))) {
        process_put(proc);
        return -EFAULT;
    }

    user_info.si_signo = sig;
    if (user_info.si_code >= 0) {
        process_put(proc);
        return -EPERM;
    }

    process_t *cur = process_current();
    if (signal_check_perm(cur, proc) < 0) {
        process_put(proc);
        return -EPERM;
    }

    int ret = signal_send_thread(proc->task, sig, &user_info);
    process_put(proc);
    return ret;
}

/*
 * do_rt_sigreturn - Restore context from signal user frame.
 *
 * Called from syscall_dispatch (special-cased to have frame access).
 * Reads the signal_user_frame_t from the user stack (which was written
 * by signal_setup_frame) and restores all saved registers into the
 * current syscall frame, plus the blocked signal mask.
 *
 * After this call, the caller should invoke signal_deliver_if_pending
 * so that signals unblocked by the restored mask are delivered.
 *
 * Returns 0 on success, negative errno on error.
 */
int64_t do_rt_sigreturn(syscall_frame_t *frame)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!frame) return -EFAULT;

    /*
     * Stack layout at the time sigreturn is called:
     *
     * The restorer trampoline was called via the handler's `ret`
     * instruction, which popped the pretcode from user stack.
     * After the pop, user RSP points to the first byte after
     * pretcode, which is offsetof(signal_user_frame_t, info).
     *
     * The signal_user_frame_t starts at (user_RSP - 8).
     */
    signal_user_frame_t sig_frame;
    if (copy_from_user(&sig_frame, (void *)(frame->rsp - 8), sizeof(signal_user_frame_t))) return -EFAULT;

    /* Restore blocked signal mask */
    signal_state_t *state = &proc->signal;
    spin_lock(&state->lock);
    state->blocked = sig_frame.old_mask;
    spin_unlock(&state->lock);

    /* Restore all saved registers */
    frame->rax    = sig_frame.rax;
    frame->rbx    = sig_frame.rbx;
    frame->rcx    = sig_frame.rcx;
    frame->rdx    = sig_frame.rdx;
    frame->rsi    = sig_frame.rsi;
    frame->rdi    = sig_frame.rdi;
    frame->rbp    = sig_frame.rbp;
    frame->r8     = sig_frame.r8;
    frame->r9     = sig_frame.r9;
    frame->r10    = sig_frame.r10;
    frame->r11    = sig_frame.r11;
    frame->r12    = sig_frame.r12;
    frame->r13    = sig_frame.r13;
    frame->r14    = sig_frame.r14;
    frame->r15    = sig_frame.r15;
    frame->rip    = sig_frame.rip;
    frame->rflags = sig_frame.rflags;
    frame->rsp    = sig_frame.rsp;
    frame->cs     = sig_frame.cs;
    frame->ss     = sig_frame.ss;

    return 0;
}

/*
 * sys_rt_sigreturn - System call entry point (stub).
 *
 * This is called via the normal syscall table dispatch, which does
 * NOT give us access to the syscall frame. The actual work is done
 * by do_rt_sigreturn(), which is called directly from syscall_dispatch
 * (special-cased like fork/clone/execve) with the frame pointer.
 *
 * This stub exists so the syscall table entry has a valid handler;
 * it should never actually be called because syscall_dispatch
 * intercepts SYS_RT_SIGRETURN before the table dispatch.
 */
int64_t sys_rt_sigreturn(void)
{
    /* If somehow reached via the normal table path, fail safely */
    return -ENOSYS;
}

/*
 * sys_setpgid - Set process group ID
 */
int64_t sys_setpgid(int64_t pid, int64_t pgid)
{
    process_t *cur = process_current();
    return process_setpgid(cur, pid, pgid);
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
    pid_t      sid;
    int        ret = process_setsid(proc, &sid);
    return ret ? ret : (int64_t)sid;
}

/*
 * sys_getsid - Get session ID
 */
int64_t sys_getsid(int64_t pid)
{
    if (pid == 0) {
        process_t *proc = process_current();
        return proc ? (int64_t)proc->sid : -ESRCH;
    }
    process_t *proc = process_find_get(pid);
    if (!proc) return -ESRCH;
    int64_t sid = proc->sid;
    process_put(proc);
    return sid;
}

/*
 * sys_getpgid - Get process group ID
 */
int64_t sys_getpgid(int64_t pid)
{
    if (pid == 0) {
        process_t *proc = process_current();
        return proc ? (int64_t)proc->pgid : -ESRCH;
    }
    process_t *proc = process_find_get(pid);
    if (!proc) return -ESRCH;
    int64_t pgid = proc->pgid;
    process_put(proc);
    return pgid;
}

static int signal_send_group(int64_t pgid, int64_t sid, int sig, process_t *sender, int code)
{
    if (pgid <= 0 || !sig_valid(sig)) return -EINVAL;

    size_t     pos = 0;
    process_t *target;
    int        found  = 0;
    int        result = 0;
    while ((target = process_group_iterate_get(&pos, pgid, sid))) {
        if (sender && signal_check_perm(sender, target) < 0) {
            process_put(target);
            continue;
        }
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code  = code;
        if (sender) {
            info.si_pid = sender->task ? (int64_t)sender->task->pid : 0;
            info.si_uid = sender->uid;
        }
        int ret = signal_send(target, sig, &info);
        if (ret && !result) result = ret;
        found = 1;
        process_put(target);
    }
    return found ? result : -ESRCH;
}

int signal_send_pgrp(int64_t pgid, int sig)
{
    return signal_send_group(pgid, 0, sig, NULL, SI_KERNEL);
}

int signal_send_pgrp_session(int64_t pgid, int64_t sid, int sig)
{
    if (sid <= 0) return -EINVAL;
    return signal_send_group(pgid, sid, sig, NULL, SI_KERNEL);
}
