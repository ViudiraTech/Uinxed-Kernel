/*
 *
 *      ptrace.h
 *      Linux-compatible process tracing interface
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PTRACE_H_
#define INCLUDE_PTRACE_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>

typedef struct process       process_t;
typedef struct task          task_t;
typedef struct syscall_frame syscall_frame_t;

/* Linux x86-64 ptrace requests. */
#define PTRACE_TRACEME              0
#define PTRACE_PEEKTEXT             1
#define PTRACE_PEEKDATA             2
#define PTRACE_PEEKUSR              3
#define PTRACE_POKETEXT             4
#define PTRACE_POKEDATA             5
#define PTRACE_POKEUSR              6
#define PTRACE_CONT                 7
#define PTRACE_KILL                 8
#define PTRACE_SINGLESTEP           9
#define PTRACE_GETREGS              12
#define PTRACE_SETREGS              13
#define PTRACE_GETFPREGS            14
#define PTRACE_SETFPREGS            15
#define PTRACE_ATTACH               16
#define PTRACE_DETACH               17
#define PTRACE_GETFPXREGS           18
#define PTRACE_SETFPXREGS           19
#define PTRACE_SYSCALL              24
#define PTRACE_SETOPTIONS           0x4200
#define PTRACE_GETEVENTMSG          0x4201
#define PTRACE_GETSIGINFO           0x4202
#define PTRACE_SETSIGINFO           0x4203
#define PTRACE_GETREGSET            0x4204
#define PTRACE_SETREGSET            0x4205
#define PTRACE_SEIZE                0x4206
#define PTRACE_INTERRUPT            0x4207
#define PTRACE_LISTEN               0x4208
#define PTRACE_PEEKSIGINFO          0x4209
#define PTRACE_GETSIGMASK           0x420a
#define PTRACE_SETSIGMASK           0x420b
#define PTRACE_SECCOMP_GET_FILTER   0x420c
#define PTRACE_SECCOMP_GET_METADATA 0x420d
#define PTRACE_GET_SYSCALL_INFO     0x420e

#define PTRACE_O_TRACESYSGOOD    0x00000001
#define PTRACE_O_TRACEFORK       0x00000002
#define PTRACE_O_TRACEVFORK      0x00000004
#define PTRACE_O_TRACECLONE      0x00000008
#define PTRACE_O_TRACEEXEC       0x00000010
#define PTRACE_O_TRACEVFORKDONE  0x00000020
#define PTRACE_O_TRACEEXIT       0x00000040
#define PTRACE_O_TRACESECCOMP    0x00000080
#define PTRACE_O_EXITKILL        0x00100000
#define PTRACE_O_SUSPEND_SECCOMP 0x00200000
#define PTRACE_O_MASK                                                                                                                      \
    (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEVFORKDONE \
     | PTRACE_O_TRACEEXIT | PTRACE_O_TRACESECCOMP | PTRACE_O_EXITKILL | PTRACE_O_SUSPEND_SECCOMP)

#define PTRACE_EVENT_FORK       1
#define PTRACE_EVENT_VFORK      2
#define PTRACE_EVENT_CLONE      3
#define PTRACE_EVENT_EXEC       4
#define PTRACE_EVENT_VFORK_DONE 5
#define PTRACE_EVENT_EXIT       6
#define PTRACE_EVENT_SECCOMP    7
#define PTRACE_EVENT_STOP       128

#define PTRACE_SYSCALL_INFO_NONE  0
#define PTRACE_SYSCALL_INFO_ENTRY 1
#define PTRACE_SYSCALL_INFO_EXIT  2

#define PTRACE_PEEKSIGINFO_SHARED 1
#define NT_PRSTATUS               1
#define NT_FPREGSET               2

#define PTRACE_EFLAGS_FIXED         0x2ULL
#define PTRACE_EFLAGS_USER_MASK     0x0000000000254fd7ULL
#define PTRACE_EFLAGS_TF            0x100ULL
#define PTRACE_USER_DEBUGREG_OFFSET 848
#define PTRACE_USER_AREA_SIZE       912

typedef struct ptrace_user_regs {
        uint64_t r15, r14, r13, r12, rbp, rbx;
        uint64_t r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi;
        uint64_t orig_rax;
        uint64_t rip, cs, eflags, rsp, ss;
        uint64_t fs_base, gs_base;
        uint64_t ds, es, fs, gs;
} ptrace_user_regs_t;

typedef struct ptrace_iovec {
        void  *base;
        size_t len;
} ptrace_iovec_t;

typedef struct ptrace_peeksiginfo_args {
        uint64_t off;
        uint32_t flags;
        int32_t  nr;
} ptrace_peeksiginfo_args_t;

typedef struct ptrace_syscall_info {
        uint8_t  op;
        uint8_t  pad[3];
        uint32_t arch;
        uint64_t instruction_pointer;
        uint64_t stack_pointer;
        union {
                struct {
                        uint64_t nr;
                        uint64_t args[6];
                } entry;
                struct {
                        int64_t rval;
                        uint8_t is_error;
                } exit;
        } data;
} ptrace_syscall_info_t;

typedef enum {
    PTRACE_RUN_CONT,
    PTRACE_RUN_SYSCALL,
    PTRACE_RUN_SINGLESTEP,
    PTRACE_RUN_LISTEN,
} ptrace_run_mode_t;

typedef enum {
    PTRACE_STOP_NONE,
    PTRACE_STOP_SIGNAL,
    PTRACE_STOP_SYSCALL_ENTRY,
    PTRACE_STOP_SYSCALL_EXIT,
    PTRACE_STOP_EVENT,
    PTRACE_STOP_GROUP,
} ptrace_stop_reason_t;

typedef struct ptrace_state {
        spinlock_t           lock;
        int64_t              tracer_pid;
        uint32_t             options;
        ptrace_run_mode_t    mode;
        ptrace_stop_reason_t stop_reason;
        int                  stop_signal;
        int                  resume_signal;
        uint32_t             event;
        uint64_t             event_msg;
        uint64_t             syscall_nr;
        bool                 seized;
        bool                 stopped;
        bool                 wait_pending;
        bool                 regs_valid;
        bool                 interrupt_requested;
        bool                 final_exit;
        int                  wait_status;
        syscall_frame_t     *active_frame;
        siginfo_t            siginfo;
        ptrace_user_regs_t   regs;
        uint64_t             debug_regs[8];
        uint8_t              fpregs[512] __attribute__((aligned(16)));
} ptrace_state_t;

/* State and register helpers. */
void ptrace_state_init(ptrace_state_t *state);
void ptrace_regs_from_frame(ptrace_user_regs_t *regs, const syscall_frame_t *frame, uint64_t orig_rax);
void ptrace_regs_to_frame(syscall_frame_t *frame, const ptrace_user_regs_t *regs);
int  ptrace_wait_status(int sig, uint32_t event);
int  ptrace_syscall_stop_signal(uint32_t options);

/* Syscall entry and tracer event hooks. */
int64_t sys_ptrace(int request, int64_t pid, uintptr_t addr, uintptr_t data);
int64_t ptrace_wait_event(int64_t pid, int *status, int options);
int     ptrace_signal_delivery(syscall_frame_t *frame, int sig, siginfo_t *info);
void    ptrace_syscall_enter(syscall_frame_t *frame, uint64_t syscall_nr);
void    ptrace_syscall_exit(syscall_frame_t *frame, int64_t result);
void    ptrace_exec_event(syscall_frame_t *frame);
void    ptrace_exit_event(int exit_code);
void    ptrace_exit_notify(int exit_code);
bool    ptrace_fork_child(task_t *parent, task_t *child, uint32_t event);
void    ptrace_fork_event(syscall_frame_t *frame, uint32_t event, uint64_t child_pid);
void    ptrace_tracer_exit(int64_t tracer_pid);
int64_t ptrace_tracer_pid(const task_t *task);
void    ptrace_arch_switch(task_t *previous, task_t *next);

#endif // INCLUDE_PTRACE_H_
