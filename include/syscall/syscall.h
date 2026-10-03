/*
 *
 *      syscall.h
 *      System call interface
 *
 *      2026/7/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_H_
#define INCLUDE_SYSCALL_H_

#include <fs/core/vfs.h>
#include <syscall/abi.h>
#include <syscall/fcntl.h>

#define SYSCALL_VECTOR 0x80

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define CLONE_VM               0x00000100ULL
#define CLONE_FS               0x00000200ULL
#define CLONE_FILES            0x00000400ULL
#define CLONE_SIGHAND          0x00000800ULL
#define CLONE_VFORK            0x00004000ULL
#define CLONE_SYSVSEM          0x00040000ULL
#define CLONE_THREAD           0x00010000ULL
#define CLONE_SETTLS           0x00080000ULL
#define CLONE_PARENT_SETTID    0x00100000ULL
#define CLONE_CHILD_CLEARTID   0x00200000ULL
#define CLONE_DETACHED         0x00400000ULL
#define CLONE_CHILD_SETTID     0x01000000ULL
#define CLONE_NEWNS            0x00020000ULL // Mount namespace
#define CLONE_NEWCGROUP        0x02000000ULL // Cgroup namespace
#define CLONE_NEWUTS           0x04000000ULL // UTS namespace
#define CLONE_NEWIPC           0x08000000ULL // IPC namespace
#define CLONE_NEWUSER          0x10000000ULL // User namespace
#define CLONE_NEWPID           0x20000000ULL // PID namespace
#define CLONE_NEWNET           0x40000000ULL // Network namespace
#define CLONE_PARENT           0x00008000ULL
#define CLONE_UNTRACED         0x00800000ULL
#define CLONE_PIDFD            0x00001000ULL
#define CLONE_PTHREAD_REQUIRED (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM)
#define CLONE_PTHREAD_ALLOWED  (CLONE_PTHREAD_REQUIRED | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED)

/* Linux UAPI struct rseq (restartable sequences), exactly 32 bytes. */
typedef struct rseq_layout {
        uint32_t cpu_id_start;
        uint32_t cpu_id;
        uint64_t rseq_cs;
        uint32_t flags;
        uint32_t node_id;
        uint32_t mm_cid;
        uint8_t  padding[4];
} __attribute__((packed)) rseq_layout_t;

_Static_assert(sizeof(rseq_layout_t) == 32, "Linux rseq ABI size");

typedef struct syscall_frame {
        uint64_t r15;
        uint64_t r14;
        uint64_t r13;
        uint64_t r12;
        uint64_t r11;
        uint64_t r10;
        uint64_t r9;
        uint64_t r8;
        uint64_t rdi;
        uint64_t rsi;
        uint64_t rbp;
        uint64_t rdx;
        uint64_t rcx;
        uint64_t rbx;
        uint64_t rax;
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
} syscall_frame_t;

typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* Syscall subsystem lifecycle and entry path. */
void syscall_init(void);
void syscall_init_cpu(void);
void pidfd_init(void);
void syscall_entry(void);
void syscall_return(void);
int  syscall_dispatch(syscall_frame_t *frame);

/* PID file descriptor helpers. The returned process pointer is borrowed. */
struct process *pidfd_get_target(vfs_node_t node);
int64_t         pidfd_get_pid(vfs_node_t node);

/* Internal path helpers shared between syscall.c and syscall_basic.c. */
int         copy_path_from_user(uint64_t upath, char path[CONFIG_VFS_PATH_MAX]);
const char *path_basename(const char *path);

/* Create a filesystem node at an already-resolved path, dispatching on mode. */
int64_t mknod_create_node(char *resolved, uint64_t mode, uint64_t dev);

#endif // INCLUDE_SYSCALL_H_
