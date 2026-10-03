/*
 *
 *      seccomp.h
 *      Secure-computing ABI.
 *
 *      2026/8/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SECCOMP_H_
#define INCLUDE_SECCOMP_H_

#include <kernel/ioctl.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Classic BPF instruction encoding. */
#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD          0x00
#define BPF_LDX         0x01
#define BPF_ST          0x02
#define BPF_STX         0x03
#define BPF_ALU         0x04
#define BPF_JMP         0x05
#define BPF_RET         0x06
#define BPF_MISC        0x07

#define BPF_SIZE(code) ((code) & 0x18)
#define BPF_W          0x00
#define BPF_H          0x08
#define BPF_B          0x10
#define BPF_MODE(code) ((code) & 0xe0)
#define BPF_IMM        0x00
#define BPF_ABS        0x20
#define BPF_IND        0x40
#define BPF_MEM        0x60
#define BPF_LEN        0x80
#define BPF_MSH        0xa0

#define BPF_OP(code) ((code) & 0xf0)
#define BPF_ADD      0x00
#define BPF_SUB      0x10
#define BPF_MUL      0x20
#define BPF_DIV      0x30
#define BPF_OR       0x40
#define BPF_AND      0x50
#define BPF_LSH      0x60
#define BPF_RSH      0x70
#define BPF_NEG      0x80
#define BPF_MOD      0x90
#define BPF_XOR      0xa0

#define BPF_JA           0x00
#define BPF_JEQ          0x10
#define BPF_JGT          0x20
#define BPF_JGE          0x30
#define BPF_JSET         0x40
#define BPF_SRC(code)    ((code) & 0x08)
#define BPF_K            0x00
#define BPF_X            0x08
#define BPF_RVAL(code)   ((code) & 0x18)
#define BPF_A            0x10
#define BPF_MISCOP(code) ((code) & 0xf8)
#define BPF_TAX          0x00
#define BPF_TXA          0x80

#define BPF_MEMWORDS 16U
#define BPF_MAXINSNS 4096U

#define BPF_STMT(code, k)                     \
    {                                         \
        (uint16_t)(code), 0, 0, (uint32_t)(k) \
    }
#define BPF_JUMP(code, k, jt, jf)                                     \
    {                                                                 \
        (uint16_t)(code), (uint8_t)(jt), (uint8_t)(jf), (uint32_t)(k) \
    }

#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2

#define SECCOMP_SET_MODE_STRICT  0
#define SECCOMP_SET_MODE_FILTER  1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_GET_NOTIF_SIZES  3

#define SECCOMP_FILTER_FLAG_TSYNC              (1UL << 0)
#define SECCOMP_FILTER_FLAG_LOG                (1UL << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW         (1UL << 2)
#define SECCOMP_FILTER_FLAG_NEW_LISTENER       (1UL << 3)
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH        (1UL << 4)
#define SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV (1UL << 5)

#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL         SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_USER_NOTIF   0x7fc00000U
#define SECCOMP_RET_TRACE        0x7ff00000U
#define SECCOMP_RET_LOG          0x7ffc0000U
#define SECCOMP_RET_ALLOW        0x7fff0000U

#define SECCOMP_RET_ACTION_FULL 0xffff0000U
#define SECCOMP_RET_ACTION      0x7fff0000U
#define SECCOMP_RET_DATA        0x0000ffffU

#define SECCOMP_USER_NOTIF_FLAG_CONTINUE   (1UL << 0)
#define SECCOMP_ADDFD_FLAG_SETFD           (1UL << 0)
#define SECCOMP_ADDFD_FLAG_SEND            (1UL << 1)
#define SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP (1UL << 0)

#define AUDIT_ARCH_64BIT  0x80000000U
#define AUDIT_ARCH_LE     0x40000000U
#define AUDIT_ARCH_X86_64 (AUDIT_ARCH_64BIT | AUDIT_ARCH_LE | 62U)

#define SECCOMP_DATA_NR_OFFSET   ((uint32_t)offsetof(struct seccomp_data, nr))
#define SECCOMP_DATA_ARCH_OFFSET ((uint32_t)offsetof(struct seccomp_data, arch))
#define SECCOMP_DATA_IP_OFFSET   ((uint32_t)offsetof(struct seccomp_data, instruction_pointer))
#define SECCOMP_DATA_ARGS_OFFSET ((uint32_t)offsetof(struct seccomp_data, args))

#define SECCOMP_IOC_MAGIC             '!'
#define SECCOMP_IO(nr)                _IO(SECCOMP_IOC_MAGIC, nr)
#define SECCOMP_IOR(nr, data_type)    _IOR(SECCOMP_IOC_MAGIC, nr, data_type)
#define SECCOMP_IOW(nr, data_type)    _IOW(SECCOMP_IOC_MAGIC, nr, data_type)
#define SECCOMP_IOWR(nr, data_type)   _IOWR(SECCOMP_IOC_MAGIC, nr, data_type)
#define SECCOMP_IOCTL_NOTIF_RECV      _IOWR(SECCOMP_IOC_MAGIC, 0, struct seccomp_notif)
#define SECCOMP_IOCTL_NOTIF_SEND      _IOWR(SECCOMP_IOC_MAGIC, 1, struct seccomp_notif_resp)
#define SECCOMP_IOCTL_NOTIF_ID_VALID  _IOW(SECCOMP_IOC_MAGIC, 2, uint64_t)
#define SECCOMP_IOCTL_NOTIF_ADDFD     _IOW(SECCOMP_IOC_MAGIC, 3, struct seccomp_notif_addfd)
#define SECCOMP_IOCTL_NOTIF_SET_FLAGS _IOW(SECCOMP_IOC_MAGIC, 4, uint64_t)
#define SECCOMP_MAX_INSNS_PER_PATH    ((1U << 18) / (uint32_t)sizeof(struct sock_filter))
#define SECCOMP_FILTER_CHAIN_PENALTY  4U

struct sock_filter {
        uint16_t code;
        uint8_t  jt;
        uint8_t  jf;
        uint32_t k;
};

struct sock_fprog {
        uint16_t            len;
        struct sock_filter *filter;
};

struct seccomp_data {
        int32_t  nr;
        uint32_t arch;
        uint64_t instruction_pointer;
        uint64_t args[6];
};

struct seccomp_notif_sizes {
        uint16_t seccomp_notif;
        uint16_t seccomp_notif_resp;
        uint16_t seccomp_data;
};

struct seccomp_notif {
        uint64_t            id;
        uint32_t            pid;
        uint32_t            flags;
        struct seccomp_data data;
};

struct seccomp_notif_resp {
        uint64_t id;
        int64_t  val;
        int32_t  error;
        uint32_t flags;
};

struct seccomp_notif_addfd {
        uint64_t id;
        uint32_t flags;
        uint32_t srcfd;
        uint32_t newfd;
        uint32_t newfd_flags;
};

struct seccomp_metadata {
        uint64_t filter_off;
        uint64_t flags;
};

typedef struct task          task_t;
typedef struct syscall_frame syscall_frame_t;
struct seccomp_filter;

/* Validate a classic-BPF filter program before running it. */
int seccomp_bpf_validate(const struct sock_filter *program, size_t length);

/* Run a validated classic-BPF filter against a seccomp_data snapshot. */
uint32_t seccomp_bpf_run(const struct sock_filter *program, size_t length, const struct seccomp_data *data);

/* Register the seccomp listener filesystem. */
void seccomp_init(void);

/* seccomp(2) syscall entry point. */
int64_t sys_seccomp(uint64_t operation, uint64_t flags, uint64_t user_args, uint64_t unused3, uint64_t unused4, uint64_t unused5);

/* prctl(PR_SET_SECCOMP) entry point. */
int64_t seccomp_prctl_set(uint64_t mode, uint64_t user_filter);

/* prctl(PR_GET_SECCOMP) entry point. */
int64_t seccomp_prctl_get(void);

/* prctl(PR_SET_NO_NEW_PRIVS) entry point. */
int64_t seccomp_set_no_new_privs(uint64_t value, uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* prctl(PR_GET_NO_NEW_PRIVS) entry point. */
int64_t seccomp_get_no_new_privs(uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* Evaluate a syscall against the task's filters; false indicates the syscall was denied. */
bool seccomp_enforce(syscall_frame_t *frame, uint64_t *syscall_nr, int64_t *result);

/* Copy the parent's seccomp state into a newly forked task. */
void seccomp_task_inherit(task_t *child, const task_t *parent);

/* Drop a task's seccomp state at exit. */
void seccomp_task_release(task_t *task);

/* Report a task's seccomp status (mode, no_new_privs, filter count). */
void seccomp_task_get_status(const task_t *task, bool *no_new_privs, uint8_t *mode, uint32_t *filter_count);

/* PTRACE_SECCOMP_GET_FILTER: copy a filter program to user space. */
int64_t seccomp_ptrace_get_filter(task_t *target, uint64_t filter_offset, void *user_program);

/* PTRACE_SECCOMP_GET_METADATA: report a filter's flags. */
int64_t seccomp_ptrace_get_metadata(task_t *target, size_t size, void *user_metadata);

#endif // INCLUDE_SECCOMP_H_
