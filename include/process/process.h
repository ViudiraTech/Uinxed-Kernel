/*
 *
 *      process.h
 *      Process management header file
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PROCESS_H_
#define INCLUDE_PROCESS_H_

#include <fs/core/vfs.h>
#include <libs/list/singly_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/page.h>
#include <process/kthread.h>
#include <process/task.h>
#include <sync/signal.h>

typedef struct tty_core      tty_core_t;
typedef struct syscall_frame syscall_frame_t;
typedef int64_t              pid_t;

extern process_t *init_process;

#define PROCESS_NAME_LEN     32
#define PROCESS_MAX_MMAP     256
#define PROCESS_MAX_ARGV     4096
#define PROCESS_MAX_ENVP     4096
#define PROCESS_MAX_CHILDREN 128
#ifndef PROCESS_MAX_FD
#    define PROCESS_MAX_FD 1024
#endif
#ifndef PROCESS_KERNEL_STACK
#    define PROCESS_KERNEL_STACK 0x10000
#endif
#if PROCESS_KERNEL_STACK < 0x10000
#    error "PROCESS_KERNEL_STACK must be at least 64 KiB"
#endif
#ifndef PROCESS_STACK_SIZE
#    define PROCESS_STACK_SIZE (4 * 1024 * 1024)
#endif
#define PROCESS_HEAP_START 0x100000
#define PROCESS_HEAP_MAX   0x7ff00000
#define PROCESS_STACK_BASE 0x7ffffffff000
#define PROCESS_MMAP_BASE  0x00007f0000000000ULL

#define PROCESS_USER_CODE_MIN  0x0000000000400000
#define PROCESS_USER_CODE_MAX  0x00007fffffe00000
#define PROCESS_USER_STACK_TOP PROCESS_STACK_BASE

/*
 * Linux resource-limit ABI indices.  Keep the table process-owned so limits
 * are shared by threads, inherited by fork, and preserved across exec.
 */
#define PROCESS_RLIMIT_CPU        0
#define PROCESS_RLIMIT_FSIZE      1
#define PROCESS_RLIMIT_DATA       2
#define PROCESS_RLIMIT_STACK      3
#define PROCESS_RLIMIT_CORE       4
#define PROCESS_RLIMIT_RSS        5
#define PROCESS_RLIMIT_NPROC      6
#define PROCESS_RLIMIT_NOFILE     7
#define PROCESS_RLIMIT_MEMLOCK    8
#define PROCESS_RLIMIT_AS         9
#define PROCESS_RLIMIT_LOCKS      10
#define PROCESS_RLIMIT_SIGPENDING 11
#define PROCESS_RLIMIT_MSGQUEUE   12
#define PROCESS_RLIMIT_NICE       13
#define PROCESS_RLIMIT_RTPRIO     14
#define PROCESS_RLIMIT_RTTIME     15
#define PROCESS_RLIMIT_COUNT      16
#define PROCESS_RLIM_INFINITY     UINT64_MAX

typedef struct process_rlimit {
        uint64_t current;
        uint64_t maximum;
} process_rlimit_t;

typedef enum {
    VM_READ   = 0x1,
    VM_WRITE  = 0x2,
    VM_EXEC   = 0x4,
    VM_SHARED = 0x8,
    VM_LAZY   = 0x10,
} vm_flags_t;

typedef enum {
    VM_REGION_CODE,
    VM_REGION_DATA,
    VM_REGION_HEAP,
    VM_REGION_STACK,
    VM_REGION_MMAP,
    VM_REGION_SHM,
    VM_REGION_VDSO,
} vm_region_type_t;

typedef struct vm_area {
        uintptr_t        start;
        uintptr_t        end;
        vm_flags_t       flags;
        vm_region_type_t type;
        struct vm_area  *next;
        vfs_node_t       vm_file;           // owning file (NULL for anonymous)
        uint64_t         vm_pgoff;          // page offset within file
        void            *vm_private_data;   // driver-private per-VMA data
        void (*vm_private_put)(void *data); // release hook for vm_private_data
        void (*vm_private_get)(void *data); // fork-copy hook for vm_private_data
        bool vm_pagecache;                  // VMA pins a regular-file cache mapping
} vm_area_t;

typedef struct process_file {
        vfs_node_t             node;
        size_t                 offset;
        uint64_t               flags;
        uint32_t               refcount;
        uint32_t               fd_refcount;
        spinlock_t             lock;
        wait_queue_t           io_wait;
        bool                   io_busy;
        void                  *private_data; // per-open-instance driver-private data
        bool                   file_opened;  // file_open succeeded and requires release
        bool                   descriptors_closed;
        struct vfs_poll_source close_source;
} process_file_t;

typedef struct process_fd_stat {
        uint64_t dev;
        uint64_t inode;
        uint32_t nlink;
        uint32_t mode;
        uint16_t type;
        uint64_t rdev;
        uint64_t size;
        uint64_t blksz;
        int64_t  atime;
        int64_t  mtime;
        int64_t  ctime;
} process_fd_stat_t;

#define PROCESS_AT_FDCWD   (-100)
#define PROCESS_MAX_GROUPS 64

typedef struct process {
        task_t           *task;
        page_directory_t *user_page_dir;
        page_directory_t *kernel_page_dir;
        vm_area_t        *mmap_list;
        spinlock_t        mmap_lock;
        spinlock_t        brk_lock;
        uintptr_t         start_brk;
        uintptr_t         heap_brk;
        uintptr_t         stack_brk;
        struct process   *parent;
        slist_t           children;
        wait_queue_t      child_wait; // fork/exit/wait condition queue

        /* Persistent queue for pause/sigsuspend; never points into a syscall stack. */
        wait_queue_t signal_wait;

        /*
         * vfork completion is separate from child exit: a successful exec
         * releases the parent while the child remains alive.  The condition
         * and waiter list are both protected by vfork_wait.lock.
         */
        wait_queue_t    vfork_wait;
        bool            vfork_done;
        int             exit_code;
        int             wait_stop_signal;
        bool            wait_stop_pending;
        bool            wait_continue_pending;
        uint32_t        uid;
        uint32_t        gid;
        uint32_t        fsuid;
        uint32_t        fsgid;
        uint32_t        supplementary_groups[PROCESS_MAX_GROUPS];
        uint16_t        supplementary_group_count;
        uint16_t        umask;
        uint8_t        *kernel_stack;
        process_file_t *fds[PROCESS_MAX_FD];

        /*
         * Descriptor flags belong to an fd table entry, not to the shared
         * open-file description.  At present Linux defines FD_CLOEXEC as the
         * sole descriptor flag; keep a full byte per slot for ABI growth.
         */
        uint8_t          fd_flags[PROCESS_MAX_FD];
        spinlock_t       fd_lock;
        process_rlimit_t rlimits[PROCESS_RLIMIT_COUNT];
        spinlock_t       rlimit_lock;
        signal_state_t   signal;

        /*
         * Linux interval timers are process-wide.  Element 0 stores an
         * absolute scheduler deadline for ITIMER_REAL; elements 1 and 2
         * store the remaining user/total CPU ticks for VIRTUAL and PROF.
         * The global interval-timer lock protects these fields and the
         * intrusive REAL-timer list.
         */
        uint64_t        itimer_value[3];
        uint64_t        itimer_interval[3];
        struct process *itimer_real_next;
        bool            itimer_real_linked;
        uint32_t        refcount;
        uint32_t        thread_count;
        ilist_node_t    threads;
        /* Serializes thread-list publication with seccomp TSYNC. */
        spinlock_t      seccomp_lock;
        pid_t           pgid;
        pid_t           sid;
        tty_core_t     *controlling_tty;
        char            name[PROCESS_NAME_LEN];
        char            root[VFS_PATH_MAX];     // chroot path
        char            cwd[VFS_PATH_MAX];      // current working directory
        char            exe_path[VFS_PATH_MAX]; // executable path (procfs /proc/<pid>/exe)
} process_t;

typedef struct process_stats {
        uint64_t user_ticks;
        uint64_t system_ticks;
        uint64_t start_tick;
        uint64_t voluntary_switches;
        uint64_t involuntary_switches;
        uint32_t threads;
        uint32_t running;
        uint32_t blocked;
} process_stats_t;

/*
 * Linux is_global_init(): the global init is the thread-group leader whose PID
 * is 1.  It carries SIGNAL_UNKILLABLE semantics and is never reparented.
 */
static inline bool is_global_init(const process_t *proc)
{
    return proc && proc->task && proc->task->pid == 1;
}

bool process_in_group(const process_t *proc, uint32_t gid);

/* Initialize the process management subsystem */
void process_init(void);

/* Create a new user process skeleton */
process_t *process_create(const char *name);

/* Set up user page directory for a process (used by ELF loader) */
int setup_process_page_dir(process_t *proc);

/*
 * Wrap an already-allocated task in a minimal kernel-thread process bundle.
 * The task must have been created by task_alloc(); its kernel stack and entry
 * trampoline are the caller's responsibility (see kernel/process/kthread.c).
 * Sets task->flags |= PF_KTHREAD, parents the process under kthreadd, and
 * publishes it in the process table.
 */
process_t *process_create_kthread(task_t *task, const char *name);

/* Terminate the current process and release its resources */
__attribute__((noreturn)) void process_exit(int exit_code);

/* Terminate every thread in the current process with one shared status. */
__attribute__((noreturn)) void process_exit_group(int exit_code);

/* Reap a zombie child process and collect its exit status */
int process_wait(pid_t pid, int *exit_code);

#define PROCESS_WAIT_NOHANG    0x00000001U
#define PROCESS_WAIT_STOPPED   0x00000002U
#define PROCESS_WAIT_CONTINUED 0x00000004U

/*
 * Linux waitpid/wait4 selector semantics.  Returns 0 with *waited_pid == 0
 * for a successful nonblocking poll, or a negative errno.
 */
int  process_wait_select(pid_t selector, int *wait_status, uint32_t options, pid_t *waited_pid);
void process_child_stopped(process_t *child, int signal);
void process_child_continued(process_t *child);

/* Find the process structure for the given pid, or NULL if not found */
process_t *process_find(pid_t pid);

/* Pinned process-table access. Call process_put() on non-NULL results. */
process_t *process_find_get(pid_t pid);
process_t *process_iterate_get(size_t *pos);
size_t     process_snapshot_pids(pid_t *pids, size_t capacity);
process_t *process_group_iterate_get(size_t *pos, pid_t pgid, pid_t sid);
task_t    *process_task_find_get(pid_t pid, process_t **owner);
void       process_put(process_t *proc);

void process_wake_threads(process_t *proc, bool resume_stopped);
void process_stop_threads(process_t *proc);
void process_get_stats(process_t *proc, process_stats_t *stats);
void process_count_task_states(uint64_t *running, uint64_t *blocked);

/* Get information about the current process */
process_t *process_current(void);

/* Controlling terminal associations own a tty reference. */
tty_core_t *process_ctty_get(process_t *proc);
int         process_ctty_set(process_t *proc, tty_core_t *tty);
void        process_ctty_clear(process_t *proc);
void        process_ctty_clear_session(tty_core_t *tty, pid_t sid);
void        process_ctty_clear_all(tty_core_t *tty);
void        process_ctty_inherit(process_t *child, process_t *parent);
bool        process_pgrp_in_session(pid_t pgid, pid_t sid);
int         process_ctty_set_foreground(tty_core_t *tty, pid_t sid, pid_t pgid);
int         process_ctty_acquire(process_t *proc, tty_core_t *tty, bool force, pid_t *old_sid, pid_t *old_pgid);
pid_t       process_ctty_disassociate(tty_core_t *tty, pid_t sid);

/* Process-group/session operations serialized by the process table lock. */
int  process_setpgid(process_t *caller, pid_t pid, pid_t pgid);
int  process_setsid(process_t *proc, pid_t *sid);
bool process_pgrp_is_orphaned(pid_t pgid, pid_t sid);

/*
 * Fork with a pre-published vfork completion state.  The state must be set
 * before the child is runnable, otherwise a fast exec/exit can be lost.
 */
process_t *process_fork_status_event_mode(int *error, uint32_t ptrace_event, bool vfork);
void       process_fork_publish(process_t *child);
void       process_fork_discard(process_t *child);

/* Suspend/release the vfork parent around child exec or exit. */
void process_vfork_wait(process_t *child);
void process_vfork_complete(process_t *proc);

/* Create a pthread-style task sharing the current process resources. */
task_t *process_clone_thread(syscall_frame_t *frame, uintptr_t child_stack, uintptr_t parent_tid, uintptr_t child_set_tid, uintptr_t child_clear_tid, uintptr_t tls, int *error);

/* Allocate a new vm_area struct */
vm_area_t *vm_area_alloc(uintptr_t start, uintptr_t end, vm_flags_t flags);

/* Insert a VMA into the process's sorted mmap list */
int vm_area_insert(process_t *proc, vm_area_t *vma);

/* Find a page-aligned VMA gap without modifying the process address space. */
uintptr_t process_find_free_vma_range(process_t *proc, size_t length);

/* Allocate a new virtual memory area in the given process */
int process_mmap(process_t *proc, uintptr_t addr, size_t length, vm_flags_t flags);

/*
 * Demand-fault a page inside a VM_LAZY mapping: resolve the VMA covering
 * addr, validate permissions, then allocate and map the physical page.
 */
int process_demand_fault(process_t *proc, uintptr_t addr, int write, int exec);

/* Unmap a virtual memory area in the given process */
int process_munmap(process_t *proc, uintptr_t addr, size_t length);

/* Unmap a range whose overlapping VMAs are wholly contained in the range. */
int process_unmap_complete_range(process_t *proc, uintptr_t addr, size_t length);

/* Drop all VMA metadata when replacing a process image. */
void process_mmap_clear(process_t *proc);

/* Atomically replace a process VMA list and return the detached old list. */
vm_area_t *process_mmap_replace(process_t *proc, vm_area_t *replacement);

/* Release a detached VMA list, including its file/shared-memory references. */
void process_mmap_destroy_detached(process_t *proc, vm_area_t *list);

/* Attach an opened VFS node to a file descriptor table */
int process_fd_install(process_t *proc, vfs_node_t node, uint64_t flags);

/* Install another reference to an existing open-file description. */
int process_fd_install_file(process_t *proc, process_file_t *file, uint64_t flags);

/* Install a shared open-file description at an exact descriptor number. */
int process_fd_install_file_at(process_t *proc, process_file_t *file, int newfd, uint64_t flags, bool replace);

/* Close a file descriptor */
int process_fd_close(process_t *proc, int fd);

/* Duplicate a file descriptor into the lowest available slot */
int process_fd_dup(process_t *proc, int oldfd);

/* Duplicate a file descriptor into a specific slot */
int process_fd_dup2(process_t *proc, int oldfd, int newfd);

/* Read/write through a file descriptor and update the shared offset */
int64_t process_fd_read(process_t *proc, int fd, void *buf, size_t size);
int64_t process_fd_write(process_t *proc, int fd, const void *buf, size_t size);
int64_t process_fd_read_user(process_t *proc, int fd, void *buf, size_t size);
int64_t process_fd_write_user(process_t *proc, int fd, const void *buf, size_t size);
int64_t process_fd_pread_user(process_t *proc, int fd, void *buf, size_t size, uint64_t offset);
int64_t process_fd_pwrite_user(process_t *proc, int fd, const void *buf, size_t size, uint64_t offset);

/* Move the shared file offset */
int64_t process_fd_seek(process_t *proc, int fd, int64_t offset, int whence);

/* Forward descriptor specific operations to the VFS */
int process_fd_ioctl(process_t *proc, int fd, size_t req, void *arg);
int process_fd_poll(process_t *proc, int fd, size_t events);

/* Snapshot metadata from an opened file descriptor */
int process_fd_stat(process_t *proc, int fd, process_fd_stat_t *stat);

/* Effective descriptor table ceiling after applying RLIMIT_NOFILE. */
uint32_t process_fd_limit(process_t *proc);

/* Decrement reference count on a file, freeing it when it reaches zero */
void            process_file_put(process_file_t *file);
void            process_file_get(process_file_t *file);
process_file_t *process_fd_get(process_t *proc, int fd);
/* Descriptor-like references used while an SCM_RIGHTS fd is in flight. */
process_file_t *process_fd_get_for_transfer(process_t *proc, int fd);
void            process_file_put_transfer(process_file_t *file);
int             process_file_poll(process_file_t *file, size_t events);

/* Resolve a pathname against cwd or a directory descriptor. */
int process_resolve_path_at(process_t *proc, int dirfd, const char *path, char *resolved, size_t size);

#endif // INCLUDE_PROCESS_H_
