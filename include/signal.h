/*
 *
 *      signal.h
 *      POSIX signal subsystem header file
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SIGNAL_H_
#define INCLUDE_SIGNAL_H_

#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- Signal number definitions / Linux compatible ---------- */

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTKFLT   16
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGXCPU     24
#define SIGXFSZ     25
#define SIGVTALRM   26
#define SIGPROF     27
#define SIGWINCH    28
#define SIGIO       29
#define SIGPWR      30
#define SIGSYS      31

#define SIGRTMIN    32
#define SIGRTMAX    64
#define NSIG        65

#define SIG_ERR     ((sig_handler_t)-1)
#define SIG_DFL     ((sig_handler_t)0)
#define SIG_IGN     ((sig_handler_t)1)

/* ---------- sigset_t / 64-bit bitmap ---------- */

typedef uint64_t sigset_t;

#define _SIGSET_NWORDS 1

static inline void sigemptyset(sigset_t *set)
{
    *set = 0;
}

static inline void sigfillset(sigset_t *set)
{
    *set = ~(uint64_t)0;
}

static inline int sigaddset(sigset_t *set, int signo)
{
    if (signo <= 0 || signo > NSIG) return -1;
    *set |= (1ULL << (signo - 1));
    return 0;
}

static inline int sigdelset(sigset_t *set, int signo)
{
    if (signo <= 0 || signo > NSIG) return -1;
    *set &= ~(1ULL << (signo - 1));
    return 0;
}

static inline int sigismember(const sigset_t *set, int signo)
{
    if (signo <= 0 || signo > NSIG) return 0;
    return !!(*set & (1ULL << (signo - 1)));
}

static inline int sigisemptyset(const sigset_t *set)
{
    return *set == 0;
}

static inline void sigorset(sigset_t *dst, const sigset_t *a, const sigset_t *b)
{
    *dst = *a | *b;
}

static inline void sigandset(sigset_t *dst, const sigset_t *a, const sigset_t *b)
{
    *dst = *a & *b;
}

static inline int sigset_valid(const sigset_t *set)
{
    (void)set;
    /* sigset_t is uint64_t, all 64 bits are valid for signals 1-64 */
    return 1;
}

/* ---------- Signal handler types ---------- */

typedef void (*sig_handler_t)(int);

typedef struct {
    sig_handler_t sa_handler;
    uint64_t      sa_flags;
    uint64_t      sa_restorer;
    sigset_t      sa_mask;
} sigaction_t;

/* sa_flags */
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000
#define SA_ONSTACK    0x08000000
#define SA_RESTORER   0x04000000

/* sigprocmask how */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

/* ---------- siginfo_t ---------- */

typedef union sigval {
    int      sival_int;
    void    *sival_ptr;
} sigval_t;

typedef struct {
    int      si_signo;
    int      si_errno;
    int      si_code;
    union {
        int    _pad[29];
        /* kill / tkill / tgkill */
        struct {
            int64_t  _pid;
            uint32_t _uid;
        } _kill;
        /* POSIX timers */
        struct {
            int      _tid;
            int      _overrun;
            sigval_t _sigval;
        } _timer;
        /* POSIX.1b signals */
        struct {
            int64_t  _pid;
            uint32_t _uid;
            sigval_t _sigval;
        } _rt;
        /* SIGCHLD */
        struct {
            int64_t  _pid;
            uint32_t _uid;
            int      _status;
            int      _utime;
            int      _stime;
        } _sigchld;
        /* SIGILL, SIGFPE, SIGSEGV, SIGBUS */
        struct {
            void   *_addr;
            int     _addr_lsb;
            union {
                struct {
                    void *_lower;
                    void *_upper;
                } _addr_bnd;
                uint32_t _pkey;
            };
        } _sigfault;
        /* SIGPOLL */
        struct {
            int64_t _band;
            int     _fd;
        } _sigpoll;
        /* SIGSYS */
        struct {
            void   *_call_addr;
            int     _syscall;
            unsigned int _arch;
        } _sigsys;
    } _sifields;
} siginfo_t;

#define si_pid     _sifields._kill._pid
#define si_uid     _sifields._kill._uid
#define si_tid     _sifields._timer._tid
#define si_overrun _sifields._timer._overrun
#define si_status  _sifields._sigchld._status
#define si_utime   _sifields._sigchld._utime
#define si_stime   _sifields._sigchld._stime
#define si_value   _sifields._rt._sigval
#define si_int     _sifields._rt._sigval.sival_int
#define si_ptr     _sifields._rt._sigval.sival_ptr
#define si_addr    _sifields._sigfault._addr
#define si_addr_lsb _sifields._sigfault._addr_lsb
#define si_lower   _sifields._sigfault._addr_bnd._lower
#define si_upper   _sifields._sigfault._addr_bnd._upper
#define si_pkey    _sifields._sigfault._pkey
#define si_band    _sifields._sigpoll._band
#define si_fd      _sifields._sigpoll._fd
#define si_syscall _sifields._sigsys._syscall
#define si_call_addr _sifields._sigsys._call_addr
#define si_arch    _sifields._sigsys._arch

/* si_code values */
#define SI_USER      0
#define SI_KERNEL    0x80
#define SI_QUEUE    -1
#define SI_TIMER    -2
#define SI_MESGQ    -3
#define SI_ASYNCIO  -4
#define SI_SIGIO    -5
#define SI_TKILL    -6
#define SI_DETHREAD -7

#define ILL_ILLOPC   1
#define ILL_ILLOPN   2
#define ILL_ILLADR   3
#define ILL_ILLTRP   4
#define ILL_PRVOPC   5
#define ILL_PRVREG   6
#define ILL_COPROC   7
#define ILL_BADSTK   8

#define FPE_INTDIV   1
#define FPE_INTOVF   2
#define FPE_FLTDIV   3
#define FPE_FLTOVF   4
#define FPE_FLTUND   5
#define FPE_FLTRES   6
#define FPE_FLTINV   7
#define FPE_FLTSUB   8

#define SEGV_MAPERR  1
#define SEGV_ACCERR  2

#define BUS_ADRALN   1
#define BUS_ADRERR   2
#define BUS_OBJERR   3

#define TRAP_BRKPT   1
#define TRAP_TRACE   2

#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

#define POLL_IN      1
#define POLL_OUT     2
#define POLL_MSG     3
#define POLL_ERR     4
#define POLL_PRI     5
#define POLL_HUP     6

/* ---------- sigaltstack ---------- */

typedef struct {
    void    *ss_sp;
    int      ss_flags;
    size_t   ss_size;
} stack_t;

#define SS_ONSTACK  1
#define SS_DISABLE  2

/* ---------- Signal queue / real-time ---------- */

#define SIGQUEUE_MAX 32

typedef struct sigqueue {
    siginfo_t        info;
    struct sigqueue *next;
} sigqueue_t;

/* ---------- Per-process signal state ---------- */

#define SIG_ACTION_NUM NSIG

/* Forward declaration */
typedef struct process process_t;
typedef struct task    task_t;
typedef struct syscall_frame syscall_frame_t;

typedef struct signal_state {
    sigaction_t  sighand[SIG_ACTION_NUM];
    sigset_t     pending;
    sigset_t     blocked;
    sigqueue_t  *sigqueue_head;
    sigqueue_t  *sigqueue_tail;
    int          sigqueue_count;
    spinlock_t   lock;

    /* Alternate signal stack */
    stack_t      altstack;

    /* Pending child exit for SIGCHLD */
    int          child_exit_code;
    int          child_exit_pending;
    int64_t      child_exit_pid;
    int          child_exit_status;
} signal_state_t;

/* ---------- Default action type ---------- */

typedef enum {
    SIG_DFL_TERM,
    SIG_DFL_IGN,
    SIG_DFL_CORE,
    SIG_DFL_STOP,
    SIG_DFL_CONT,
} sig_dfl_action_t;

/* ---------- Kernel API ---------- */

/* Initialize the signal subsystem */
void signal_init(void);

/* Initialize signal state for a new process */
void signal_state_init(signal_state_t *state);

/* Free signal state resources */
void signal_state_free(signal_state_t *state);

/* Send a signal to a process */
int signal_send(process_t *proc, int sig, const siginfo_t *info);

/* Send a signal to a specific thread */
int signal_send_thread(task_t *task, int sig, const siginfo_t *info);

/* Check and deliver pending signals, called on return to userspace */
int signal_deliver_if_pending(syscall_frame_t *frame);

/* Check if there is a pending signal that should be delivered */
int signal_has_pending(signal_state_t *state);

/* Get the default action for a signal */
sig_dfl_action_t signal_default_action(int sig);

/* Check if a signal is a real-time signal */
static inline int sig_is_rt(int sig)
{
    return sig >= SIGRTMIN && sig <= SIGRTMAX;
}

/* Check if a signal number is valid */
static inline int sig_valid(int sig)
{
    return sig > 0 && sig <= NSIG;
}

/* Check if a signal is ignorable (cannot be ignored, caught, or blocked) */
static inline int sig_is_uncatchable(int sig)
{
    return sig == SIGKILL || sig == SIGSTOP;
}

/* Check if a signal is a stop signal */
static inline int sig_is_stop(int sig)
{
    return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

/* Flush all pending signals for a process */
void signal_flush(process_t *proc);

/* Copy signal state for fork */
void signal_state_copy(signal_state_t *dst, const signal_state_t *src);

/* Check permissions for sending a signal */
int signal_check_perm(const process_t *from, const process_t *to);

/* Notify the signal subsystem that a child process exited */
void signal_notify_child_exit(process_t *parent, int64_t child_pid, int exit_code, int status);

/* ---------- Syscall implementations (called from syscall.c) ---------- */

int64_t sys_kill_impl(int64_t pid, int sig);
int64_t sys_tkill_impl(int64_t tid, int sig);
int64_t sys_tgkill(int64_t tgid, int64_t tid, int sig);
int64_t sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oact, size_t sigsetsize);
int64_t sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset, size_t sigsetsize);
int64_t sys_rt_sigreturn(void);
int64_t sys_rt_sigpending(sigset_t *set, size_t sigsetsize);
int64_t sys_rt_sigtimedwait(const sigset_t *set, siginfo_t *info, const void *timeout, size_t sigsetsize);
int64_t sys_rt_sigqueueinfo(int64_t pid, int sig, siginfo_t *info);
int64_t sys_rt_sigsuspend(const sigset_t *set, size_t sigsetsize);
int64_t sys_sigaltstack(const stack_t *ss, stack_t *oss);
int64_t sys_pause(void);
int64_t sys_rt_tgsigqueueinfo(int64_t tgid, int64_t tid, int sig, siginfo_t *info);
int64_t sys_setpgid(int64_t pid, int64_t pgid);
int64_t sys_getpgrp(void);
int64_t sys_setsid(void);
int64_t sys_getsid(int64_t pid);
int64_t sys_getpgid(int64_t pid);

#endif /* INCLUDE_SIGNAL_H_ */