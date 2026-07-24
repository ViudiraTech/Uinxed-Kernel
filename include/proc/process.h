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

#include <libs/glist/singly_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/page.h>
#include <proc/task.h>
#include <sync/signal.h>

typedef struct vfs_node *vfs_node_t;

typedef struct syscall_frame syscall_frame_t;

typedef int64_t pid_t;

#define PROCESS_NAME_LEN     32
#define PROCESS_MAX_MMAP     256
#define PROCESS_MAX_ARGV     64
#define PROCESS_MAX_ENVP     64
#define PROCESS_MAX_CHILDREN 128
#define PROCESS_MAX_FD       64
#define PROCESS_KERNEL_STACK 0x10000
#define PROCESS_STACK_SIZE   (4 * 1024 * 1024)
#define PROCESS_HEAP_START   0x100000
#define PROCESS_HEAP_MAX     0x7ff00000
#define PROCESS_STACK_BASE   0x7ffffffff000

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
} vm_area_t;

typedef struct process_file {
        vfs_node_t node;
        size_t     offset;
        uint64_t   flags;
        uint32_t   refcount;
        spinlock_t lock;
        void      *private_data; /* per-open-instance driver-private data */
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
        pid_t             pgid;
        pid_t             sid;
        char              name[PROCESS_NAME_LEN];
        char              root[256];       /* chroot path */
        char              cwd[256];        /* current working directory */
        uint64_t          clear_child_tid; /* set_tid_address target */
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

/* Get information about the current process */
process_t *process_current(void);

/* Clone the current process (fork semantics) */
process_t *process_fork(void);

/* Clone the current process and make the child return from the syscall frame */
process_t *process_fork_from_syscall(syscall_frame_t *frame);

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
process_file_t *process_fd_get(process_t *proc, int fd);

#endif /* INCLUDE_PROCESS_H_ */
