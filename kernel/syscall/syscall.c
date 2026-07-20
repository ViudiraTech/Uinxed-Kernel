/*
 *
 *      syscall.c
 *      System call dispatch
 *
 *      2026/7/20 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <interrupt.h>
#include <printk.h>
#include <process.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall.h>
#include <task.h>
#include <uaccess.h>
#include <vfs.h>

#define SYSCALL_PATH_MAX 256
#define SYSCALL_IO_CHUNK 4096

static int64_t sys_exit(uint64_t status, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_exit((int)status);
    return 0;
}

static int64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    task_t *task = current_task();
    return task ? (int64_t)task->pid : -ESRCH;
}

static int64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    sched_yield();
    return 0;
}

static int64_t sys_sleep(uint64_t ticks, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    task_sleep_ticks(ticks);
    return 0;
}

static int64_t sys_wait(uint64_t pid, uint64_t exit_code, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    int status = 0;
    int ret    = process_wait((pid_t)pid, exit_code ? &status : NULL);
    if (ret) return -ECHILD;
    if (exit_code) *(int *)exit_code = status;
    return 0;
}

static int64_t sys_kill(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    return process_kill((pid_t)pid) ? -ESRCH : 0;
}

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_mmap(proc, (uintptr_t)addr, (size_t)length, (vm_flags_t)flags) ? -ENOMEM : (int64_t)addr;
}

static int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_munmap(proc, (uintptr_t)addr, (size_t)length) ? -EINVAL : 0;
}

static int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)mode;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!path) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[SYSCALL_PATH_MAX];
    int  copied = strncpy_from_user(name, (const char *)path, sizeof(name));
    if (copied < 0) return copied;

    vfs_node_t node = vfs_open(name);
    if (!node && (flags & O_CREAT)) {
        int ret = vfs_mkfile(name);
        if (ret != EOK && ret != -EEXIST) return ret;
        node = vfs_open(name);
    }
    if (!node) return -ENOENT;

    int fd = process_fd_install(proc, node, flags);
    if (fd < 0) vfs_close(node);
    return fd;
}

static int64_t sys_close(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_close(proc, (int)fd);
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!buf && size) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint8_t tmp[SYSCALL_IO_CHUNK];
    size_t  done = 0;
    while (done < size) {
        size_t  chunk = (size - done) < sizeof(tmp) ? (size - done) : sizeof(tmp);
        int64_t ret   = process_fd_read(proc, (int)fd, tmp, chunk);
        if (ret < 0) return done ? (int64_t)done : ret;
        if (!ret) break;
        if (copy_to_user((void *)(buf + done), tmp, (size_t)ret)) return done ? (int64_t)done : -EFAULT;
        done += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    return (int64_t)done;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!buf && size) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint8_t tmp[SYSCALL_IO_CHUNK];
    size_t  done = 0;
    while (done < size) {
        size_t chunk = (size - done) < sizeof(tmp) ? (size - done) : sizeof(tmp);
        if (copy_from_user(tmp, (const void *)(buf + done), chunk)) return done ? (int64_t)done : -EFAULT;
        int64_t ret = process_fd_write(proc, (int)fd, tmp, chunk);
        if (ret < 0) return done ? (int64_t)done : ret;
        if (!ret) break;
        done += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    return (int64_t)done;
}

static int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_seek(proc, (int)fd, (int64_t)offset, (int)whence);
}

static int64_t sys_dup(uint64_t oldfd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_dup(proc, (int)oldfd);
}

static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_dup2(proc, (int)oldfd, (int)newfd);
}

static int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_ioctl(proc, (int)fd, (size_t)req, (void *)arg);
}

static int64_t sys_poll(uint64_t fd, uint64_t events, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_poll(proc, (int)fd, (size_t)events);
}

typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static syscall_fn_t syscall_table[SYS_MAX] = {
    [SYS_EXIT] = sys_exit,   [SYS_GETPID] = sys_getpid, [SYS_YIELD] = sys_yield,   [SYS_SLEEP] = sys_sleep, [SYS_WAIT] = sys_wait,
    [SYS_KILL] = sys_kill,   [SYS_MMAP] = sys_mmap,     [SYS_MUNMAP] = sys_munmap, [SYS_OPEN] = sys_open,   [SYS_CLOSE] = sys_close,
    [SYS_READ] = sys_read,   [SYS_WRITE] = sys_write,   [SYS_LSEEK] = sys_lseek,   [SYS_DUP] = sys_dup,     [SYS_DUP2] = sys_dup2,
    [SYS_IOCTL] = sys_ioctl, [SYS_POLL] = sys_poll,
};

void syscall_dispatch(syscall_frame_t *frame)
{
    uint64_t num = frame->rax;

    if (num == SYS_FORK) {
        process_t *child = process_fork_from_syscall(frame);
        frame->rax       = child ? child->task->pid : (uint64_t)-ENOMEM;
        return;
    }

    if (num >= SYS_MAX || !syscall_table[num]) {
        frame->rax = (uint64_t)-ENOSYS;
        return;
    }

    frame->rax = (uint64_t)syscall_table[num](frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
}

__attribute__((naked)) void syscall_return(void)
{
    __asm__ volatile("popq %r15\n\t"
                     "popq %r14\n\t"
                     "popq %r13\n\t"
                     "popq %r12\n\t"
                     "popq %r11\n\t"
                     "popq %r10\n\t"
                     "popq %r9\n\t"
                     "popq %r8\n\t"
                     "popq %rdi\n\t"
                     "popq %rsi\n\t"
                     "popq %rbp\n\t"
                     "popq %rdx\n\t"
                     "popq %rcx\n\t"
                     "popq %rbx\n\t"
                     "popq %rax\n\t"
                     "iretq\n\t");
}

__attribute__((naked)) void syscall_entry(void)
{
    __asm__ volatile("cld\n\t"
                     "pushq %rax\n\t"
                     "pushq %rbx\n\t"
                     "pushq %rcx\n\t"
                     "pushq %rdx\n\t"
                     "pushq %rbp\n\t"
                     "pushq %rsi\n\t"
                     "pushq %rdi\n\t"
                     "pushq %r8\n\t"
                     "pushq %r9\n\t"
                     "pushq %r10\n\t"
                     "pushq %r11\n\t"
                     "pushq %r12\n\t"
                     "pushq %r13\n\t"
                     "pushq %r14\n\t"
                     "pushq %r15\n\t"
                     "movq %rsp, %rdi\n\t"
                     "call syscall_dispatch\n\t"
                     "jmp syscall_return\n\t");
}

void syscall_init(void)
{
    register_interrupt_handler(SYSCALL_VECTOR, (void *)syscall_entry, 0, 0xee);
    plogk("syscall: int 0x%02x interface initialized.\n", SYSCALL_VECTOR);
}
