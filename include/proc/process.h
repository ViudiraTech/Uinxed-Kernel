/*
 *
 *      process.h
 *      Process management header file
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PROCESS_H_
#define INCLUDE_PROCESS_H_

#include <fs/core/vfs.h>
#include <libs/glist/singly_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/page.h>
#include <proc/task.h>
#include <sync/signal.h>

typedef struct tty_core tty_core_t;

typedef struct syscall_frame syscall_frame_t;

typedef int64_t pid_t;

#define PROCESS_NAME_LEN     32
#define PROCESS_MAX_MMAP     256
#define PROCESS_MAX_ARGV     64
#define PROCESS_MAX_ENVP     64
#define PROCESS_MAX_CHILDREN 128
#ifndef PROCESS_MAX_FD
#    define PROCESS_MAX_FD 64
#endif
#ifndef PROCESS_KERNEL_STACK
#    define PROCESS_KERNEL_STACK 0x10000
#endif
#ifndef PROCESS_STACK_SIZE
#    define PROCESS_STACK_SIZE (4 * 1024 * 1024)
#endif
#define PROCESS_HEAP_START 0x100000
#define PROCESS_HEAP_MAX   0x7ff00000
#define PROCESS_STACK_BASE 0x7ffffffff000

#define PROCESS_USER_CODE_MIN  0x0000000000400000
#define PROCESS_USER_CODE_MAX  0x00007fffffe00000
#define PROCESS_USER_STACK_TOP PROCESS_STACK_BASE

typedef enum {
    VM_READ   = 0x1,
    VM_WRITE  = 0x2,
    VM_EXEC   = 0x4,
    VM_SHARED = 0x8,
} vm_flags_t;

typedef enum {
    PROCESS_LOADING,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_SLEEPING,
    PROCESS_ZOMBIE,
    PROCESS_DEAD,
} process_state_t;

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
        vfs_node_t       vm_file;         /* owning file (NULL for anonymous) */
        uint64_t         vm_pgoff;        /* page offset within file */
        void            *vm_private_data; /* driver-private per-VMA data */
        bool             vm_pagecache;    /* VMA pins a regular-file cache mapping */
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
        void                  *private_data; /* per-open-instance driver-private data */
        bool                   file_opened;  /* file_open succeeded and requires release */
        bool                   descriptors_closed;
        struct vfs_poll_source close_source;
} process_file_t;

typedef struct process_fd_stat {
        uint64_t dev;
        uint64_t inode;
        uint32_t mode;
        uint16_t type;
        uint64_t rdev;
        uint64_t size;
        uint64_t blksz;
} process_fd_stat_t;

#define PROCESS_AT_FDCWD -100

typedef struct process {
        task_t           *task;
        page_directory_t *user_page_dir;
        page_directory_t *kernel_page_dir;
        vm_area_t        *mmap_list;
        spinlock_t        mmap_lock;
        uintptr_t         heap_brk;
        uintptr_t         stack_brk;
        struct process   *parent;
        slist_t           children;
        int               exit_code;
        uint32_t          uid;
        uint32_t          gid;
        uint8_t          *kernel_stack;
        process_file_t   *fds[PROCESS_MAX_FD];
        spinlock_t        fd_lock;
        signal_state_t    signal;
        uint32_t          refcount;
        uint32_t          thread_count;
        ilist_node_t      threads;
        pid_t             pgid;
        pid_t             sid;
        tty_core_t       *controlling_tty;
        char              name[PROCESS_NAME_LEN];
        char              root[VFS_PATH_MAX]; /* chroot path */
        char              cwd[VFS_PATH_MAX];  /* current working directory */
} process_t;

/* Initialize the process management subsystem */
void process_init(void);

/* Create a new user process skeleton */
process_t *process_create(const char *name, void (*entry)(void *), void *arg);

/* Set up user page directory for a process (used by ELF loader) */
int setup_process_page_dir(process_t *proc);

/* Create a kernel process (task with no user address space) */
process_t *process_create_kernel(const char *name, void (*entry)(void *), void *arg);

/* Terminate the current process and release its resources */
void process_exit(int exit_code);

/* Reap a zombie child process and collect its exit status */
int process_wait(pid_t pid, int *exit_code);

/* Send a signal to terminate the given process */
int process_kill(pid_t pid);

/* Find the process structure for the given pid, or NULL if not found */
process_t *process_find(pid_t pid);

/* Iterate all processes. Set *pos = 0 to start, returns NULL when done */
process_t *process_iterate(size_t *pos);

/* Pinned process-table access. Call process_put() on non-NULL results. */
process_t *process_find_get(pid_t pid);
process_t *process_iterate_get(size_t *pos);
process_t *process_group_iterate_get(size_t *pos, pid_t pgid, pid_t sid);
task_t    *process_task_find_get(pid_t pid, process_t **owner);
void       process_put(process_t *proc);

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

/* Clone the current process (fork semantics) */
process_t *process_fork(void);

/* Clone the current process and preserve the allocation/admission errno. */
process_t *process_fork_status(int *error);

/* Fork while propagating a Linux ptrace creation event before enqueue. */
process_t *process_fork_status_event(int *error, uint32_t ptrace_event);

/* Clone the current process and make the child return from the syscall frame */
process_t *process_fork_from_syscall(syscall_frame_t *frame);

/* Create a pthread-style task sharing the current process resources. */
task_t *process_clone_thread(syscall_frame_t *frame, uintptr_t child_stack, uintptr_t parent_tid, uintptr_t child_set_tid,
                             uintptr_t child_clear_tid, uintptr_t tls, int *error);

/* Return the next available pid */
pid_t process_next_pid(void);

/* Allocate a new vm_area struct */
vm_area_t *vm_area_alloc(uintptr_t start, uintptr_t end, vm_flags_t flags);

/* Insert a VMA into the process's sorted mmap list */
int vm_area_insert(process_t *proc, vm_area_t *vma);

/* Allocate a new virtual memory area in the given process */
int process_mmap(process_t *proc, uintptr_t addr, size_t length, vm_flags_t flags);

/* Unmap a virtual memory area in the given process */
int process_munmap(process_t *proc, uintptr_t addr, size_t length);

/* Unmap a range whose overlapping VMAs are wholly contained in the range. */
int process_unmap_complete_range(process_t *proc, uintptr_t addr, size_t length);

/* Drop all VMA metadata when replacing a process image. */
void process_mmap_clear(process_t *proc);

/* Attach an opened VFS node to a file descriptor table */
int process_fd_install(process_t *proc, vfs_node_t node, uint64_t flags);

/* Close a file descriptor */
int process_fd_close(process_t *proc, int fd);

/* Duplicate a file descriptor into the lowest available slot */
int process_fd_dup(process_t *proc, int oldfd);

/* Duplicate a file descriptor into a specific slot */
int process_fd_dup2(process_t *proc, int oldfd, int newfd);

/* Read/write through a file descriptor and update the shared offset */
int64_t process_fd_read(process_t *proc, int fd, void *buf, size_t size);
int64_t process_fd_write(process_t *proc, int fd, const void *buf, size_t size);

/* Move the shared file offset */
int64_t process_fd_seek(process_t *proc, int fd, int64_t offset, int whence);

/* Forward descriptor specific operations to the VFS */
int process_fd_ioctl(process_t *proc, int fd, size_t req, void *arg);
int process_fd_poll(process_t *proc, int fd, size_t events);

/* Snapshot metadata from an opened file descriptor */
int process_fd_stat(process_t *proc, int fd, process_fd_stat_t *stat);

/* Decrement reference count on a file, freeing it when it reaches zero */
void            process_file_put(process_file_t *file);
void            process_file_get(process_file_t *file);
process_file_t *process_fd_get(process_t *proc, int fd);
int             process_file_poll(process_file_t *file, size_t events);

/* Resolve a pathname against cwd or a directory descriptor. */
int process_resolve_path_at(process_t *proc, int dirfd, const char *path, char *resolved, size_t size);

#endif /* INCLUDE_PROCESS_H_ */
