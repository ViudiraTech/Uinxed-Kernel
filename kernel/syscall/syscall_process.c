/*
 *
 *      syscall_process.c
 *      Process, thread, signal, exec and pidfd syscalls
 *
 *      2026/11/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/eflags.h>
#include <arch/cpu/fpu.h>
#include <arch/cpu/smp.h>
#include <arch/misc/common.h>
#include <fs/core/vfs.h>
#include <fs/core/vfs_stub.h>
#include <kernel/abi.h>
#include <kernel/debug/printk.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/elf_loader.h>
#include <process/namespace.h>
#include <process/process.h>
#include <process/ptrace.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <security/seccomp.h>
#include <sync/signal.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>

#define EXEC_STRING_MAX (CONFIG_PROCESS_STACK_SIZE / 2)
#define AT_EXECVE_CHECK 0x10000

/* wait flags */
#define WNOHANG     0x00000001
#define WUNTRACED   0x00000002
#define WSTOPPED    0x00000002
#define WEXITED     0x00000004
#define WCONTINUED  0x00000008
#define WNOWAIT     0x01000000
#define __WNOTHREAD 0x20000000
#define __WCLONE    0x80000000
#define __WALL      0x40000000

/* waitid id types */
#define P_ALL   0
#define P_PID   1
#define P_PGID  2
#define P_PIDFD 3

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

/* pidfd_open flags */
#define PIDFD_NONBLOCK 0x800ULL

/* prctl */
#define PR_SET_PDEATHSIG       1
#define PR_GET_PDEATHSIG       2
#define PR_GET_DUMPABLE        3
#define PR_SET_DUMPABLE        4
#define PR_GET_KEEPCAPS        7
#define PR_SET_KEEPCAPS        8
#define PR_SET_NAME            15
#define PR_GET_NAME            16
#define PR_GET_SECCOMP         21
#define PR_SET_SECCOMP         22
#define PR_CAPBSET_READ        23
#define PR_CAPBSET_DROP        24
#define PR_GET_SECUREBITS      27
#define PR_SET_SECUREBITS      28
#define PR_SET_TIMERSLACK      29
#define PR_GET_TIMERSLACK      30
#define PR_SET_MM              35
#define PR_SET_MM_ARG_START    8
#define PR_SET_MM_ARG_END      9
#define PR_SET_MM_ENV_START    10
#define PR_SET_MM_ENV_END      11
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37
#define PR_SET_NO_NEW_PRIVS    38
#define PR_GET_NO_NEW_PRIVS    39
#define PR_GET_TID_ADDRESS     40

/* exit syscall: terminate the calling thread */
int64_t sys_exit(uint64_t status, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_exit((int)status);
    return 0;
}

/* System call handler for `getpid`. */
int64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    task_t *task = current_task();
    return task ? (int64_t)task->tgid : -ESRCH;
}

/* System call handler for `sched_yield`. */
int64_t sys_sched_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* wait4 syscall: wait for a child to change state */
int64_t sys_wait4(uint64_t pid, uint64_t exit_code, uint64_t options, uint64_t rusage, uint64_t arg4, uint64_t arg5)
{
    (void)rusage;
    (void)arg4;
    (void)arg5;

    int flags  = (int)options;
    int status = 0;
    if (flags & ~(WNOHANG | WUNTRACED | WCONTINUED)) return -EINVAL;

    int64_t traced = ptrace_wait_event((pid_t)pid, &status, flags);
    if (traced != -ECHILD) {
        if (traced < 0) return traced;
        if (traced && exit_code && copy_to_user((void *)exit_code, &status, sizeof(status))) return -EFAULT;
        return traced;
    }

    uint32_t wait_options = (flags & WNOHANG) ? PROCESS_WAIT_NOHANG : 0;
    if (flags & WUNTRACED) wait_options |= PROCESS_WAIT_STOPPED;
    if (flags & WCONTINUED) wait_options |= PROCESS_WAIT_CONTINUED;

    pid_t waited_pid = 0;
    int   ret        = process_wait_select((pid_t)pid, &status, wait_options, &waited_pid);
    if (ret < 0) return ret;
    if (!waited_pid) return 0;
    if (exit_code && copy_to_user((void *)exit_code, &status, sizeof(status))) return -EFAULT;

    return (int64_t)waited_pid;
}

/* System call handler for `kill`. */
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_kill_impl((pid_t)pid, (int)sig);
}

/* arch_prctl syscall: set or get FS/GS base */
int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    task_t *task = current_task();

    switch (code) {
        case 0x1001 : // ARCH_SET_GS
            if (addr >= PROCESS_USER_STACK_TOP) return -EPERM;

            /* In kernel mode IA32_GS_BASE is the per-CPU base; the user GS lives in KERNEL_GS_BASE. */
            set_user_gs_base(addr);
            if (task) task->thread.gs_base = addr;
            return 0;
        case 0x1002 : // ARCH_SET_FS
            if (addr >= PROCESS_USER_STACK_TOP) return -EPERM;
            wrmsr(0xC0000100, addr);
            if (task) task->thread.fs_base = addr;
            return 0;
        case 0x1003 : { // ARCH_GET_FS
            uint64_t fs = task ? task->thread.fs_base : 0;
            return copy_to_user((void *)addr, &fs, sizeof(fs));
        }
        case 0x1004 : { // ARCH_GET_GS
            uint64_t gs = task ? task->thread.gs_base : 0;
            return copy_to_user((void *)addr, &gs, sizeof(gs));
        }
        default :
            return -EINVAL;
    }
}

/* System call handler for `exit_group`. */
int64_t sys_exit_group(uint64_t status, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_exit_group((int)status);
}

/* System call handler for `getppid`. */
int64_t sys_getppid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    return proc && proc->parent && proc->parent->task ? (int64_t)proc->parent->task->tgid : 0;
}

/* System call handler for `gettid`. */
int64_t sys_gettid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* System call handler for `ptrace`. */
int64_t sys_ptrace_wrap(uint64_t request, uint64_t pid, uint64_t addr, uint64_t data, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_ptrace((int)request, (int64_t)pid, (uintptr_t)addr, (uintptr_t)data);
}

/* getrusage syscall: report resource usage */
int64_t sys_getrusage_impl(uint64_t who, uint64_t usage, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!usage) return -EFAULT;
    if ((int)who != RUSAGE_SELF && (int)who != RUSAGE_CHILDREN) return -EINVAL;

    linux_rusage_t ru;
    memset(&ru, 0, sizeof(ru));

    /* Return some approximate usage values */
    uint64_t ns      = timer_monotonic_ns();
    ru.ru_utime_sec  = ns / TIMER_NSEC_PER_SEC;
    ru.ru_utime_usec = (ns % TIMER_NSEC_PER_SEC) / 1000;
    ru.ru_minflt     = 0;
    ru.ru_majflt     = 0;

    if (copy_to_user((void *)usage, &ru, sizeof(ru))) return -EFAULT;
    return 0;
}

/* restart_syscall syscall: restart a blocking syscall */
int64_t sys_restart_syscall(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    /* Restart is handled by the dispatcher adjusting RIP. */
    return 0;
}

/* Read a resource limit from a process */
static int process_rlimit_snapshot(process_t *target, uint64_t resource, linux_rlimit64_t *limit)
{
    if (!target || !limit) return -EINVAL;
    if (resource >= PROCESS_RLIMIT_COUNT) return -EINVAL;

    spin_lock(&target->rlimit_lock);
    limit->rlim_cur = target->rlimits[resource].current;
    limit->rlim_max = target->rlimits[resource].maximum;
    spin_unlock(&target->rlimit_lock);

    return EOK;
}

/* Update a process resource limit, enforcing privileges */
static int process_rlimit_update(process_t *caller, process_t *target, uint64_t resource, const linux_rlimit64_t *limit)
{
    if (!caller || !target || !limit) return -EINVAL;
    if (resource >= PROCESS_RLIMIT_COUNT || limit->rlim_cur > limit->rlim_max) return -EINVAL;

    /*
     * The descriptor table is currently a fixed 1024-entry array.  Userland
     * daemons such as dbus still legitimately request a larger soft/hard
     * RLIMIT_NOFILE during startup; clamp that request to the real kernel
     * ceiling instead of returning EPERM to a root process.
     */
    uint64_t current = limit->rlim_cur;
    uint64_t maximum = limit->rlim_max;

    if (resource == PROCESS_RLIMIT_NOFILE) {
        if (current > CONFIG_PROCESS_MAX_FD) current = CONFIG_PROCESS_MAX_FD;
        if (maximum > CONFIG_PROCESS_MAX_FD) maximum = CONFIG_PROCESS_MAX_FD;
    }

    spin_lock(&target->rlimit_lock);
    uint64_t old_max = target->rlimits[resource].maximum;

    if (caller->uid != 0 && maximum > old_max) {
        spin_unlock(&target->rlimit_lock);
        return -EPERM;
    }

    target->rlimits[resource].current = current;
    target->rlimits[resource].maximum = maximum;
    spin_unlock(&target->rlimit_lock);
    return EOK;
}

/* getrlimit syscall: read a resource limit */
int64_t sys_getrlimit_impl(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!rlim) return -EFAULT;
    linux_rlimit64_t rl;
    int              result = process_rlimit_snapshot(process_current(), resource, &rl);

    if (result != EOK) return result;
    return copy_to_user((void *)rlim, &rl, sizeof(rl)) ? -EFAULT : EOK;
}

/* setrlimit syscall: set a resource limit */
int64_t sys_setrlimit_impl(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!rlim) return -EFAULT;
    linux_rlimit64_t limit;

    if (copy_from_user(&limit, (const void *)rlim, sizeof(limit))) return -EFAULT;
    process_t *proc = process_current();

    return process_rlimit_update(proc, proc, resource, &limit);
}

/* prlimit64 syscall: read and/or set a resource limit */
int64_t sys_prlimit64_impl(uint64_t pid, uint64_t resource, uint64_t new_rlim, uint64_t old_rlim, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (resource >= PROCESS_RLIMIT_COUNT) return -EINVAL;
    process_t *caller = process_current();

    if (!caller) return -ESRCH;
    process_t *target = pid == 0 ? caller : process_find_get((pid_t)pid);
    bool       pinned = pid != 0;

    if (!target) return -ESRCH;
    if (caller->uid != 0 && caller->uid != target->uid) {
        if (pinned) process_put(target);
        return -EPERM;
    }

    linux_rlimit64_t old_limit;
    int              result = process_rlimit_snapshot(target, resource, &old_limit);
    if (result == EOK && old_rlim && copy_to_user((void *)old_rlim, &old_limit, sizeof(old_limit))) result = -EFAULT;
    if (result == EOK && new_rlim) {
        linux_rlimit64_t new_limit;
        if (copy_from_user(&new_limit, (const void *)new_rlim, sizeof(new_limit))) {
            result = -EFAULT;
        } else {
            result = process_rlimit_update(caller, target, resource, &new_limit);
        }
    }

    if (pinned) process_put(target);
    return result;
}

/* execve implementation with the path already resolved. */
static int64_t do_execve_resolved(const char *path, vfs_node_t initial_node, char *const argv[], char *const envp[], syscall_frame_t *frame);

/* execveat implementation: exec a program by dirfd+path, with exec-only checks */
int64_t do_execveat(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, syscall_frame_t *frame)
{
    if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_EXECVE_CHECK)) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char kpath[CONFIG_VFS_PATH_MAX];
    char input[CONFIG_VFS_PATH_MAX];
    int  ret;

    ret = copy_path_from_user(path, input);
    if (ret != EOK) return ret;
    if (!input[0] && !(flags & AT_EMPTY_PATH)) return -ENOENT;
    if (!input[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        process_file_t *pf = process_fd_get(proc, (int)dirfd);

        if (!pf) return -EBADF;
        vfs_node_t node = vfs_node_retain(pf->node);

        if (!node) {
            process_file_put(pf);
            return -EBADF;
        }
        process_file_put(pf);

        if (vfs_node_path(node, kpath, sizeof(kpath)) != EOK) (void)snprintf(kpath, sizeof(kpath), "/dev/fd/%d", (int)dirfd);
        if (flags & AT_EXECVE_CHECK) {
            int check = (node->type & file_dir) || vfs_access_check_process(node, VFS_ACCESS_X, proc) ? -EACCES : EOK;
            vfs_close(node);
            return check;
        }
        return do_execve_resolved(kpath, node, (char *const *)argv, (char *const *)envp, frame);
    }

    ret = process_resolve_path_at(proc, (int)dirfd, input, kpath, sizeof(kpath));
    if (ret != EOK) return ret;
    if (flags & AT_EXECVE_CHECK) {
        /* AT_EXECVE_CHECK: just check if the file is executable, don't actually exec. */
        int        lookup_error = EOK;
        vfs_node_t node         = vfs_open_checked(kpath, &lookup_error);

        if (!node) return lookup_error;
        int check = (node->type & file_dir) || vfs_access_check_process(node, VFS_ACCESS_X, proc) ? -EACCES : EOK;

        vfs_close(node);
        return check;
    }

    if (flags & AT_SYMLINK_NOFOLLOW) {
        int        lookup_error = EOK;
        vfs_node_t node         = vfs_open_nofollow_checked(kpath, &lookup_error);
        if (!node) return lookup_error;

        bool symlink = (node->type & file_symlink) != 0;
        vfs_close(node);
        if (symlink) return -ELOOP;
    }
    return do_execve_resolved(kpath, NULL, (char *const *)argv, (char *const *)envp, frame);
}

/* System call handler for `execveat_wrap`. */
int64_t sys_execveat_wrap(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    return do_execveat(dirfd, path, argv, envp, flags, NULL);
}

/* System call handler for `membarrier_wrap`. */
int64_t sys_membarrier_wrap(uint64_t cmd, uint64_t flags, uint64_t cpu_id, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)cpu_id;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    /* MEMBARRIER_CMD_QUERY = 0, MEMBARRIER_CMD_GLOBAL = 1 */
    if (flags) return -EINVAL;
    switch (cmd) {
        case 0 :
            return 1; // MEMBARRIER_CMD_GLOBAL
        case 1 :
            /* The synchronous shootdown provides a cross-CPU rendezvous. */
            flush_tlb_all();
            return 0;
        default :
            return -EINVAL;
    }
}

/* rseq syscall: register a restartable sequence area */
int64_t sys_rseq_impl(uint64_t rseq_base, uint64_t rseq_len, uint64_t flags, uint64_t sig, uint64_t arg4, uint64_t arg5)
{
    (void)sig;
    (void)arg4;
    (void)arg5;

    if (flags & ~0ULL) return -EINVAL;
    if (rseq_len != sizeof(rseq_layout_t)) return -EINVAL;
    if (!rseq_base) return -EINVAL;

    /* Write cpu_id to the rseq area */
    task_t *task = current_task();
    int     cpu  = task ? (int)task->cpu_id : 0;
    if (copy_to_user((void *)(rseq_base + offsetof(rseq_layout_t, cpu_id_start)), &cpu, sizeof(cpu))) return -EFAULT;
    if (copy_to_user((void *)(rseq_base + offsetof(rseq_layout_t, cpu_id)), &cpu, sizeof(cpu))) return -EFAULT;

    return 0;
}

/* pidfd filesystem id */
static int pidfd_fsid = -1;

/* VFS close callback: drop the process reference */
static void pidfd_vfs_close(void *current)
{
    process_t *target = (process_t *)current;
    if (target) process_put(target);
}

/* Return the process pinned by a pidfd node. The node owns the reference. */
process_t *pidfd_get_target(vfs_node_t node)
{
    if (!node || pidfd_fsid < 0 || node->fsid != pidfd_fsid || !node->handle) return NULL;
    return (process_t *)node->handle;
}

/* Resolve a pidfd to the PID exposed through /proc/<pid>/fdinfo/<fd>. */
int64_t pidfd_get_pid(vfs_node_t node)
{
    process_t *target = pidfd_get_target(node);
    if (!target || !target->task) return -ESRCH;
    return (int64_t)target->task->tgid;
}

/* Register pidfs during boot, before userspace can issue concurrent opens. */
void pidfd_init(void)
{
    if (pidfd_fsid >= 0) return;
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("pidfd: Failed to allocate VFS callbacks.\n");
        return;
    }

    /*
     * pidfds expose no file contents, so reads and writes fail.  read(2)/write(2)
     * reach the per-open slots while vfs_read()/vfs_write() only see the legacy
     * ones, so implementing pidfd I/O means filling the per-open slots below rather
     * than read/write.
     */
    cb->unmount    = vfs_stub_unmount;
    cb->open       = vfs_stub_open;
    cb->close      = pidfd_vfs_close;
    cb->read       = vfs_stub_read;
    cb->write      = vfs_stub_write;
    cb->readlink   = vfs_stub_readlink;
    cb->mkdir      = vfs_stub_mk;
    cb->mkfile     = vfs_stub_mk;
    cb->link       = vfs_stub_mk;
    cb->symlink    = vfs_stub_mk;
    cb->stat       = vfs_stub_stat;
    cb->ioctl      = vfs_stub_ioctl;
    cb->dup        = vfs_stub_dup;
    cb->delete     = vfs_stub_del;
    cb->rename     = vfs_stub_rename;
    cb->file_read  = vfs_stub_file_read;
    cb->file_write = vfs_stub_file_write;

    pidfd_fsid = vfs_regist_fs("pidfd", cb);
    free(cb);
    if (pidfd_fsid < 0) {
        plogk("pidfd: Failed to register VFS callbacks (%d)\n", pidfd_fsid);
    } else {
        plogk("pidfd: Filesystem registered (fsid=%d)\n", pidfd_fsid);
    }
}

/* pidfd_open syscall: open a process descriptor */
int64_t sys_pidfd_open_impl(uint64_t pid_raw, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (flags & ~(uint64_t)PIDFD_NONBLOCK) return -EINVAL;
    if ((int64_t)pid_raw <= 0) return -EINVAL;

    process_t *target = process_find_get((pid_t)pid_raw);
    if (!target) return -ESRCH;

    process_t *proc = process_current();
    if (!proc) {
        process_put(target);
        return -ESRCH;
    }
    if (pidfd_fsid < 0) {
        process_put(target);
        return -ENODEV;
    }

    vfs_node_t node = vfs_node_alloc(NULL, "[pidfd]");
    if (!node) {
        process_put(target);
        return -ENOMEM;
    }

    node->type   = file_stream;
    node->handle = target; // caller must process_put in close
    node->fsid   = pidfd_fsid;
    node->size   = 0;
    node->mode   = O_RDWR;

    /* Linux creates pidfds read/write and close-on-exec. */
    uint64_t fd_flags = O_RDWR | O_CLOEXEC;
    if (flags & PIDFD_NONBLOCK) fd_flags |= O_NONBLOCK;

    int fd = process_fd_install(proc, node, fd_flags);
    if (fd < 0) {
        vfs_close(node);
        return fd;
    }
    return fd;
}

/* clone3 syscall: create a child process or thread */
int64_t sys_clone3_impl(syscall_frame_t *frame, uint64_t cl_args, uint64_t size, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    /* Linux clone_args version 0 ends at tls (64 bytes). */
    if (size < 64) return -EINVAL;
    if (!cl_args || cl_args > UINT64_MAX - size) return -EFAULT;

    /* Future-sized structures are accepted only when their unknown tail is zero. */
    if (size > sizeof(clone3_args_t)) {
        uint8_t  tail[64];
        uint64_t offset = sizeof(clone3_args_t);
        while (offset < size) {
            size_t chunk = size - offset < sizeof(tail) ? (size_t)(size - offset) : sizeof(tail);
            if (copy_from_user(tail, (const void *)(uintptr_t)(cl_args + offset), chunk)) return -EFAULT;
            for (size_t i = 0; i < chunk; i++)
                if (tail[i]) return -E2BIG;
            offset += chunk;
        }
        size = sizeof(clone3_args_t);
    }

    clone3_args_t args;
    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void *)cl_args, (size_t)size)) return -EFAULT;

    uint64_t flags       = args.flags;
    uint64_t exit_signal = args.exit_signal;
    bool     is_thread   = (flags & CLONE_THREAD) != 0;
    bool     is_vfork    = (flags & CLONE_VFORK) != 0;

    if ((args.stack && !args.stack_size) || (!args.stack && args.stack_size)) return -EINVAL;
    if (args.stack && UINT64_MAX - args.stack < args.stack_size) return -EINVAL;

    /* Be permissive for systemd's sandboxing: allow pidfd/set_tid/cgroup but handle pidfd if requested */
    if (args.set_tid_size > 1) return -EINVAL;
    if (is_thread) {
        if ((flags & CLONE_PTHREAD_REQUIRED) != CLONE_PTHREAD_REQUIRED || (flags & ~CLONE_PTHREAD_ALLOWED) || exit_signal || !args.stack || !args.stack_size) return -EINVAL;
        if (((flags & CLONE_PARENT_SETTID) && !args.parent_tid) || ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !args.child_tid)) return -EFAULT;
        process_t *proc = process_current();

        if (!proc) return -ESRCH;
        uintptr_t child_stack = (uintptr_t)(args.stack + args.stack_size);
        int       error       = EOK;
        task_t   *child       = process_clone_thread(frame, child_stack, (flags & CLONE_PARENT_SETTID) ? args.parent_tid : 0, (flags & CLONE_CHILD_SETTID) ? args.child_tid : 0,
                                             (flags & CLONE_CHILD_CLEARTID) ? args.child_tid : 0, (flags & CLONE_SETTLS) ? args.tls : current_task()->thread.fs_base, &error);

        if (!child) return error;
        ptrace_fork_event(frame, PTRACE_EVENT_CLONE, child->pid);
        return (int64_t)child->pid;
    }

    /* Fork / vfork - be permissive for systemd's namespace sandboxing (mkdcreds etc) */
    if ((is_vfork && !(flags & CLONE_VM)) || (exit_signal && exit_signal != SIGCHLD && exit_signal != 0)) return -EINVAL;
    if (flags & CLONE_THREAD) return -EINVAL; // thread creation handled above
    if (((flags & CLONE_PARENT_SETTID) && !args.parent_tid) || ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !args.child_tid)) return -EFAULT;

    int        error = EOK;
    uint32_t   event = is_vfork ? PTRACE_EVENT_VFORK : PTRACE_EVENT_FORK;
    process_t *child = process_fork_status_event_mode(&error, event, is_vfork);
    if (!child) return error;

    /*
     * Seed the child's resume frame on its own kernel stack: it returns from
     * the fork/clone3 syscall with rax=0, sharing the parent's user stack
     * unless a caller-provided stack was given.
     */
    task_t *ct = child->task;
    if (ct) {
        syscall_frame_t child_frame = *frame;
        child_frame.rax             = 0;
        if (args.stack && args.stack_size) {
            uintptr_t child_stack = (uintptr_t)(args.stack + args.stack_size);
            if (!user_access_ok((void *)(child_stack - 1), 1, 1)) {
                process_fork_discard(child);
                return -EFAULT;
            }
            child_frame.rsp = child_stack;
        }

        uint64_t  kstack_top = (uint64_t)(child->kernel_stack + CONFIG_PROCESS_KERNEL_STACK);
        uint64_t *kstack     = (uint64_t *)ALIGN_DOWN(kstack_top, 16ULL);

        kstack -= sizeof(syscall_frame_t) / sizeof(uint64_t);
        memcpy(kstack, &child_frame, sizeof(child_frame));
        *(--kstack)     = (uint64_t)syscall_return;
        ct->context.rsp = (uint64_t)kstack;

        uint32_t tid       = (uint32_t)ct->pid;
        int      tid_error = 0;

        if ((flags & CLONE_PARENT_SETTID) && copy_to_user((void *)args.parent_tid, &tid, sizeof(tid))) tid_error = -EFAULT;
        if (!tid_error && (flags & CLONE_CHILD_SETTID)
            && (!user_access_ok_process(child, (void *)args.child_tid, sizeof(tid), 1) || copy_to_user_process_nofault(child, (void *)args.child_tid, &tid, sizeof(tid))))
            tid_error = -EFAULT;
        if (tid_error) {
            process_fork_discard(child);
            return tid_error;
        }
        if (flags & CLONE_CHILD_CLEARTID) ct->clear_child_tid = args.child_tid;
    }

    process_fork_publish(child);
    ptrace_fork_event(frame, event, child->task->pid);
    if (is_vfork) {
        process_vfork_wait(child);
        ptrace_fork_event(frame, PTRACE_EVENT_VFORK_DONE, child->task->pid);
    }

    /* Handle pidfd if requested (systemd's mkdcreds etc. uses it) */
    if (args.pidfd) {
        process_t *proc = process_current();
        if (!proc) {
            /* Child already published; just return pid, pidfd will be invalid */
        } else {
            /* Create pidfd for child */
            __atomic_fetch_add(&child->refcount, 1, __ATOMIC_RELAXED);
            vfs_node_t node  = vfs_node_alloc(NULL, "[pidfd]");
            int        pidfd = -1;
            if (node) {
                node->type   = file_stream;
                node->handle = child;
                node->fsid   = pidfd_fsid;
                node->size   = 0;
                node->mode   = O_RDWR;
                pidfd        = process_fd_install(proc, node, O_RDWR | O_CLOEXEC);
                if (pidfd < 0) {
                    vfs_close(node);
                    process_put(child);
                } else {
                    if (copy_to_user((void *)args.pidfd, &pidfd, sizeof(pidfd))) {
                        process_fd_close(proc, pidfd);
                        return -EFAULT;
                    }
                }
            } else {
                process_put(child);
            }
        }
    }
    return (int64_t)child->task->pid;
}

/* Signal syscall wrappers */
int64_t sys_rt_sigaction_wrap(uint64_t sig, uint64_t act, uint64_t oact, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_sigaction((int)sig, (const sigaction_t *)act, (sigaction_t *)oact, (size_t)sigsetsize);
}

/* System call handler for `rt_sigprocmask`. */
int64_t sys_rt_sigprocmask_wrap(uint64_t how, uint64_t set, uint64_t oset, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_sigprocmask((int)how, (const sigset_t *)set, (sigset_t *)oset, (size_t)sigsetsize);
}

/* System call handler for `rt_sigreturn`. */
int64_t sys_rt_sigreturn_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigreturn();
}

/* System call handler for `rt_sigpending`. */
int64_t sys_rt_sigpending_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigpending((sigset_t *)set, (size_t)sigsetsize);
}

/* System call handler for `rt_sigtimedwait`. */
int64_t sys_rt_sigtimedwait_wrap(uint64_t set, uint64_t info, uint64_t timeout, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_sigtimedwait((const sigset_t *)set, (siginfo_t *)info, (const void *)timeout, (size_t)sigsetsize);
}

/* System call handler for `rt_sigqueueinfo`. */
int64_t sys_rt_sigqueueinfo_wrap(uint64_t pid, uint64_t sig, uint64_t info, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigqueueinfo((pid_t)pid, (int)sig, (siginfo_t *)info);
}

/* System call handler for `rt_sigsuspend`. */
int64_t sys_rt_sigsuspend_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigsuspend((const sigset_t *)set, (size_t)sigsetsize);
}

/* System call handler for `sigaltstack`. */
int64_t sys_sigaltstack_wrap(uint64_t ss, uint64_t oss, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_sigaltstack((const stack_t *)ss, (stack_t *)oss);
}

/* System call handler for `pause`. */
int64_t sys_pause_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_pause();
}

/* System call handler for `tgkill`. */
int64_t sys_tgkill_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_tgkill((pid_t)tgid, (pid_t)tid, (int)sig);
}

/* System call handler for `rt_tgsigqueueinfo`. */
int64_t sys_rt_tgsigqueueinfo_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t info, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_tgsigqueueinfo((pid_t)tgid, (pid_t)tid, (int)sig, (siginfo_t *)info);
}

/* System call handler for `setpgid`. */
int64_t sys_setpgid_wrap(uint64_t pid, uint64_t pgid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_setpgid((pid_t)pid, (pid_t)pgid);
}

/* System call handler for `getpgrp`. */
int64_t sys_getpgrp_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getpgrp();
}

/* System call handler for `setsid`. */
int64_t sys_setsid_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_setsid();
}

/* System call handler for `getsid`. */
int64_t sys_getsid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getsid((pid_t)pid);
}

/* System call handler for `getpgid`. */
int64_t sys_getpgid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getpgid((pid_t)pid);
}

/* waitid syscall: wait for a child by selector */
int64_t sys_waitid_impl(uint64_t which, uint64_t upid, uint64_t infop, uint64_t options, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    pid_t selector;
    int   flags = (int)options;

    if (!(flags & (WEXITED | WSTOPPED | WCONTINUED)) || (flags & ~(WNOHANG | WNOWAIT | WEXITED | WSTOPPED | WCONTINUED | __WNOTHREAD | __WCLONE | __WALL))) return -EINVAL;

    switch ((int)which) {
        case P_PID :
            selector = (pid_t)upid;
            if (selector <= 0) return -EINVAL;
            break;
        case P_PGID :
            if (upid > INT64_MAX) return -EINVAL;
            selector = upid ? -(pid_t)upid : 0;
            break;
        case P_ALL :
            selector = -1;
            break;
        case 3 : // P_PIDFD
        {
            process_t *proc = process_current();
            if (!proc) return -ESRCH;
            if (upid < CONFIG_PROCESS_MAX_FD) {
                process_file_t *pf = process_fd_get(proc, (int)upid);
                if (pf) {
                    process_t *target = pidfd_get_target(pf->node);
                    if (target && target->task) {
                        selector = (pid_t)target->task->tgid;
                        process_file_put(pf);
                        break;
                    }
                    process_file_put(pf);
                }
            }
            if ((int64_t)upid <= 0 || (int64_t)upid > 4194304) return -EINVAL;
            selector = (pid_t)upid;
            break;
        }
        default :
            return -EINVAL;
    }

    int     status = 0;
    int64_t ret;

    if ((int)which == P_PID) {
        ret = ptrace_wait_event(selector, &status, flags);
        if (ret != -ECHILD) {
            if (ret < 0) return ret;
            if (!ret) return 0;
            if (infop) {
                siginfo_t info;
                memset(&info, 0, sizeof(info));
                info.si_signo  = SIGCHLD;
                info.si_code   = CLD_TRAPPED;
                info.si_pid    = selector;
                info.si_status = (status >> 8) & 0xff;
                if (copy_to_user((void *)infop, &info, sizeof(info))) return -EFAULT;
            }
            return 0;
        }
    }

    pid_t    waited_pid   = 0;
    uint32_t wait_options = (flags & WNOHANG) ? PROCESS_WAIT_NOHANG : 0;
    if (flags & WNOWAIT) wait_options |= PROCESS_WAIT_KEEPEVENT;
    if (flags & WSTOPPED) wait_options |= PROCESS_WAIT_STOPPED;
    if (flags & WCONTINUED) wait_options |= PROCESS_WAIT_CONTINUED;

    ret = process_wait_select(selector, &status, wait_options, &waited_pid);
    if (ret < 0) return ret;

    /* Populate siginfo_t if requested */
    if (infop) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        if (waited_pid) {
            info.si_signo = SIGCHLD;
            info.si_pid   = waited_pid;
            info.si_uid   = 0;
            if (status == 0xffff) {
                info.si_code   = CLD_CONTINUED;
                info.si_status = SIGCONT;
            } else if ((status & 0xff) == 0x7f) {
                info.si_code   = CLD_STOPPED;
                info.si_status = (status >> 8) & 0xff;
            } else if (status & 0x7f) {
                info.si_code   = CLD_KILLED;
                info.si_status = status & 0x7f;
            } else {
                info.si_code   = CLD_EXITED;
                info.si_status = (status >> 8) & 0xff;
            }
        }
        if (copy_to_user((void *)infop, &info, sizeof(info))) return -EFAULT;
    }
    return 0;
}

/* Copy an argv/envp vector from user space */
static char **copy_argv_from_user(const char *const *uargv, int max_entries, int *out_count, size_t *total_bytes, int *out_error)
{
    *out_count = -1;
    *out_error = -EFAULT;
    if (!uargv) {
        *out_count = 0;
        return NULL;
    }

    char **kargv = calloc((size_t)max_entries + 1, sizeof(char *));
    if (!kargv) {
        *out_error = -ENOMEM;
        return NULL;
    }

    for (int i = 0; i <= max_entries; i++) {
        char *ustr;
        int   pointer_error = copy_from_user(&ustr, (const void *)&uargv[i], sizeof(ustr));
        if (pointer_error) {
            *out_error = pointer_error;
            goto fail;
        }
        if (!ustr) {
            if (!i) {
                free(kargv);
                kargv = NULL;
            }
            *out_count = i;
            return kargv;
        }
        if (i == max_entries || *total_bytes >= EXEC_STRING_MAX) {
            *out_error = -E2BIG;
            goto fail;
        }

        int length = strnlen_user(ustr, EXEC_STRING_MAX - *total_bytes);
        if (length < 1) {
            *out_error = length == -ENAMETOOLONG ? -E2BIG : length;
            goto fail;
        }

        kargv[i] = malloc((size_t)length);
        if (!kargv[i]) {
            *out_error = -ENOMEM;
            goto fail;
        }

        int string_error = copy_from_user(kargv[i], ustr, (size_t)length);
        if (string_error) {
            *out_error = string_error;
            goto fail;
        }
        *total_bytes += (size_t)length;
    }
fail:
    for (int i = 0; i < max_entries; i++)
        if (kargv[i]) free(kargv[i]);
    free(kargv);
    return NULL;
}

/* Free a NULL-terminated string array */
static void free_string_array(char **arr)
{
    if (!arr) return;
    for (int i = 0; arr[i]; i++) free(arr[i]);
    free(arr);
}

/* Load and execute a resolved program image */
static int64_t do_execve_resolved(const char *path, vfs_node_t initial_node, char *const argv[], char *const envp[], syscall_frame_t *frame)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char   kpath[CONFIG_VFS_PATH_MAX];
    size_t path_len = path ? strlen(path) : 0;
    if (!path_len) {
        if (initial_node) vfs_close(initial_node);
        return -ENOENT;
    }
    if (path_len >= sizeof(kpath)) {
        if (initial_node) vfs_close(initial_node);
        return -ENAMETOOLONG;
    }

    memcpy(kpath, path, path_len + 1);
    char exec_name[PROCESS_NAME_LEN];
    strncpy(exec_name, path_basename(kpath), sizeof(exec_name) - 1);
    exec_name[sizeof(exec_name) - 1] = '\0';

    int    argc = 0, envc = 0;
    int    copy_error     = -EFAULT;
    size_t argument_bytes = 0;
    char **kargv          = copy_argv_from_user((const char *const *)argv, CONFIG_PROCESS_MAX_ARGV, &argc, &argument_bytes, &copy_error);
    if (argc < 0) {
        if (initial_node) vfs_close(initial_node);
        free_string_array(kargv);
        return copy_error;
    }

    char **kenvp = copy_argv_from_user((const char *const *)envp, CONFIG_PROCESS_MAX_ENVP, &envc, &argument_bytes, &copy_error);
    if (envc < 0) {
        if (initial_node) vfs_close(initial_node);
        free_string_array(kargv);
        free_string_array(kenvp);
        return copy_error;
    }
    (void)envc;

    uint8_t   *elf_data   = NULL;
    size_t     total      = 0;
    size_t     image_size = 0;
    vfs_node_t exec_node  = NULL;

    /* Replace argv[0] with the interpreter, then the script path; resolve at most four nested interpreters. */
    for (int depth = 0;; depth++) {
        int        lookup_error = EOK;
        vfs_node_t node;
        if (initial_node) {
            node         = initial_node;
            initial_node = NULL;
        } else {
            node = vfs_open_checked(kpath, &lookup_error);
        }
        if (!node) {
            free_string_array(kargv);
            free_string_array(kenvp);
            return lookup_error;
        }
        if ((node->type & file_dir) || vfs_access_check_process(node, VFS_ACCESS_X, proc) != EOK) {
            vfs_close(node);
            free_string_array(kargv);
            free_string_array(kenvp);
            return -EACCES;
        }

        size_t expected_size = node->size;
        if (node->size == 0 || node->size > 0x4000000) {
            vfs_close(node);
            free_string_array(kargv);
            free_string_array(kenvp);
            return -ENOEXEC;
        }

        size_t header_size = node->size < 256 ? (size_t)node->size : 256;
        elf_data           = malloc(header_size);
        if (!elf_data) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("syscall: Exec of %s failed (header allocation, %zu bytes)\n", kpath, header_size);
            vfs_close(node);
            free_string_array(kargv);
            free_string_array(kenvp);
            return -ENOMEM;
        }

        total = 0;
        while (total < header_size) {
            size_t remaining = header_size - total;
            size_t n         = vfs_read(node, elf_data + total, total, remaining);
            if (!n || n > remaining) break;
            total += n;
        }

        if (total >= 2 && elf_data[0] == '#' && elf_data[1] == '!') {
            if (depth >= 4) {
                vfs_close(node);
                free(elf_data);
                free_string_array(kargv);
                free_string_array(kenvp);
                return -ELOOP;
            }

            size_t limit = total < 256 ? total : 256;
            size_t end   = 2;
            while (end < limit && elf_data[end] != '\n') end++;
            if (end == limit && (end == total || elf_data[end - 1] != '\n')) {
                vfs_close(node);
                free(elf_data);
                free_string_array(kargv);
                free_string_array(kenvp);
                return -ENOEXEC;
            }

            while (end > 2 && (elf_data[end - 1] == ' ' || elf_data[end - 1] == '\t' || elf_data[end - 1] == '\r')) end--;
            size_t start = 2;

            while (start < end && (elf_data[start] == ' ' || elf_data[start] == '\t')) start++;
            size_t interp_end = start;

            while (interp_end < end && elf_data[interp_end] != ' ' && elf_data[interp_end] != '\t') interp_end++;
            if (interp_end == start || interp_end - start >= 256) {
                vfs_close(node);
                free(elf_data);
                free_string_array(kargv);
                free_string_array(kenvp);
                return -ENOEXEC;
            }

            char interpreter[256];
            memcpy(interpreter, elf_data + start, interp_end - start);
            interpreter[interp_end - start] = '\0';
            size_t optional                 = interp_end;
            while (optional < end && (elf_data[optional] == ' ' || elf_data[optional] == '\t')) optional++;

            int    new_argc = 1 + (optional < end ? 1 : 0) + 1 + (argc > 0 ? argc - 1 : 0);
            char **new_argv = calloc((size_t)new_argc + 1, sizeof(char *));
            if (!new_argv) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("syscall: Exec of %s failed (shebang argv allocation)\n", kpath);
                vfs_close(node);
                free(elf_data);
                free_string_array(kargv);
                free_string_array(kenvp);
                return -ENOMEM;
            }

            int out         = 0;
            new_argv[out++] = strdup(interpreter);
            if (!new_argv[out - 1]) goto shebang_oom;
            if (optional < end) {
                size_t optional_len = end - optional;
                if (optional_len == SIZE_MAX) goto shebang_oom;

                new_argv[out] = malloc(optional_len + 1);
                if (!new_argv[out]) goto shebang_oom;

                memcpy(new_argv[out], elf_data + optional, optional_len);
                new_argv[out][optional_len] = '\0';
                out++;
            }

            new_argv[out++] = strdup(kpath);
            if (!new_argv[out - 1]) goto shebang_oom;
            for (int i = 1; i < argc; i++) {
                new_argv[out++] = strdup(kargv[i]);
                if (!new_argv[out - 1]) goto shebang_oom;
            }

            bool allocation_failed = false;
            for (int i = 0; i < new_argc; i++)
                if (!new_argv[i]) allocation_failed = true;
            if (false) {
shebang_oom:
                allocation_failed = true;
            }
            if (allocation_failed) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("syscall: Exec of %s failed (shebang argv element allocation)\n", kpath);
                vfs_close(node);
                free(elf_data);
                free_string_array(new_argv);
                free_string_array(kargv);
                free_string_array(kenvp);
                return -ENOMEM;
            }

            int resolve_status = process_resolve_path_at(proc, PROCESS_AT_FDCWD, interpreter, kpath, sizeof(kpath));
            if (resolve_status != EOK) {
                vfs_close(node);
                free(elf_data);
                free_string_array(new_argv);
                free_string_array(kargv);
                free_string_array(kenvp);
                return resolve_status;
            }

            vfs_close(node);
            free(elf_data);
            elf_data = NULL;
            free_string_array(kargv);
            kargv = new_argv;
            argc  = new_argc;
            continue;
        }

        if (total < sizeof(uint32_t)) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("syscall: Exec of %s read only %zu bytes (expected %zu)\n", kpath, total, expected_size);
            vfs_close(node);
            free(elf_data);
            free_string_array(kargv);
            free_string_array(kenvp);
            return -ENOEXEC;
        }

        exec_node  = node;
        image_size = expected_size;
        break;
    }

    page_directory_t *old_dir       = proc->user_page_dir;
    vm_area_t        *old_mmaps     = process_mmap_replace(proc, NULL);
    uintptr_t         old_start_brk = proc->start_brk;
    uintptr_t         old_heap_brk  = proc->heap_brk;
    uintptr_t         old_stack_brk = proc->stack_brk;
    uint64_t          old_fs_base   = proc->task->thread.fs_base;
    uint64_t          old_gs_base   = proc->task->thread.gs_base;
    task_context_t    old_context   = proc->task->context;

    /* Build the replacement image against a clean, private set of VMAs. */
    proc->start_brk = CONFIG_PROCESS_HEAP_START;
    proc->heap_brk  = CONFIG_PROCESS_HEAP_START;
    proc->stack_brk = PROCESS_USER_STACK_TOP - (long)CONFIG_PROCESS_STACK_SIZE;

    if (setup_process_page_dir(proc)) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("syscall: Exec of %s failed (page directory setup)\n", kpath);
        (void)process_mmap_replace(proc, old_mmaps);
        proc->start_brk = old_start_brk;
        proc->heap_brk  = old_heap_brk;
        proc->stack_brk = old_stack_brk;
        vfs_close(exec_node);
        free(elf_data);
        free_string_array(kargv);
        free_string_array(kenvp);
        return -ENOMEM;
    }

    /*
     * Load the ELF into the new page directory BEFORE destroying the old one, so a
     * failed load can restore the old address space.
     */
    uintptr_t entry       = 0;
    uintptr_t rsp         = 0;
    uint32_t  image_magic = total >= sizeof(uint32_t) ? *(const uint32_t *)elf_data : 0U;
    int       ret         = elf_loader_load_user_node(proc, exec_node, kargv, kenvp, &entry, &rsp);
    vfs_close(exec_node);
    free(elf_data);
    free_string_array(kargv);
    free_string_array(kenvp);

    if (ret) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("syscall: Exec of %s rejected by ELF loader (errno %d, %zu bytes, magic=%u)\n", kpath, ret, image_size, image_magic);

        /* Loading failed. Destroy the new (incomplete) page directory and its VMAs, then restore the old address space. */
        vm_area_t *failed_mmaps = process_mmap_replace(proc, old_mmaps);
        page_destroy_user_space(proc->user_page_dir);
        free(proc->user_page_dir);
        process_mmap_destroy_detached(proc, failed_mmaps);
        proc->user_page_dir        = old_dir;
        proc->task->page_directory = old_dir;
        proc->start_brk            = old_start_brk;
        proc->heap_brk             = old_heap_brk;
        proc->stack_brk            = old_stack_brk;
        proc->task->thread.fs_base = old_fs_base;
        proc->task->thread.gs_base = old_gs_base;
        proc->task->context        = old_context;
        return ret;
    }

    /*
     * Loading succeeded: from this point onward exec cannot fail.  Apply
     * close-on-exec only after the replacement image is known to be valid.
     */
    for (int i = 0; i < CONFIG_PROCESS_MAX_FD; i++) {
        spin_lock(&proc->fd_lock);
        if (proc->fds[i] && (proc->fd_flags[i] & FD_CLOEXEC)) {
            spin_unlock(&proc->fd_lock);
            process_fd_close(proc, i);
            continue;
        }
        spin_unlock(&proc->fd_lock);
    }

    proc->task->clear_child_tid = 0;
    proc->task->robust_list     = 0;
    switch_page_directory(proc->user_page_dir);

    proc->task->thread.fs_base = 0;
    proc->task->thread.gs_base = 0;
    wrmsr(0xC0000100, 0);

    /* User GS is parked in KERNEL_GS_BASE while in kernel mode. */
    set_user_gs_base(0);

    if (old_dir) {
        page_destroy_user_space(old_dir);
        free(old_dir);
    }
    process_mmap_destroy_detached(proc, old_mmaps);

    /* Reset signal state for new program image (per POSIX) */
    signal_exec_reset(proc);
    fpu_task_reset(proc->task);
    task_name_copy(proc->task, exec_name);
    strncpy(proc->name, exec_name, sizeof(proc->name) - 1);
    proc->name[sizeof(proc->name) - 1] = '\0';
    strncpy(proc->exe_path, kpath, sizeof(proc->exe_path) - 1);
    proc->exe_path[sizeof(proc->exe_path) - 1] = '\0';

    /*
     * Publish exec completion only after CLOEXEC descriptors and the old
     * address space have been retired.  This is the release point required
     * by vfork/posix_spawn.
     */
    process_vfork_complete(proc);

    if (frame) {
        memset(frame, 0, sizeof(*frame));
        frame->rip    = entry;
        frame->cs     = 0x1B;
        frame->rflags = X86_EFLAGS_IF | X86_EFLAGS_FIXED;
        frame->rsp    = rsp;
        frame->ss     = 0x23;
    }
    return 0;
}

/* Resolve a path and execute the program */
int64_t do_execve(const char *path, char *const argv[], char *const envp[], syscall_frame_t *frame)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char kpath[CONFIG_VFS_PATH_MAX];
    int  path_ret = copy_resolved_path_at(proc, AT_FDCWD, (uint64_t)path, kpath);
    if (path_ret != EOK) return path_ret;
    return do_execve_resolved(kpath, NULL, argv, envp, frame);
}

/* execve syscall: replace the process image */
int64_t sys_execve_wrap(uint64_t path, uint64_t argv, uint64_t envp, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return do_execve((const char *)path, (char *const *)argv, (char *const *)envp, NULL);
}

/* prctl syscall: process control operations */
int64_t sys_prctl_impl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg6;

    switch ((int)option) {
        case PR_SET_PDEATHSIG :
            /* arg2 is the signal to send on parent death */
            if ((int)arg2 > 64) return -EINVAL;
            return 0;
        case PR_GET_PDEATHSIG :
            /* Return 0 (no parent death signal) */
            if (arg2 && copy_to_user((void *)arg2, &(int) {0}, sizeof(int))) return -EFAULT;
            return 0;
        case PR_GET_DUMPABLE :
            /* Return dumpable=1 */
            if (arg2 && copy_to_user((void *)arg2, &(int) {1}, sizeof(int))) return -EFAULT;
            return 0;
        case PR_SET_DUMPABLE :
        case PR_GET_KEEPCAPS :
        case PR_SET_KEEPCAPS :
            /* Accept any value */
            return 0;
        case PR_SET_NAME : {
            /* Set process name - copy up to 15 bytes */
            if (arg2) {
                process_t *proc = process_current();
                if (!proc) return -ESRCH;
                char name[16];
                if (copy_from_user(name, (const void *)arg2, 16)) return -EFAULT;
                name[15] = '\0';
                strncpy(proc->name, name, sizeof(proc->name) - 1);
            }
            return 0;
        }
        case PR_GET_NAME : {
            /* Get process name */
            if (arg2) {
                process_t *proc = process_current();
                if (!proc) return -ESRCH;
                if (copy_to_user((void *)arg2, proc->name, 16)) return -EFAULT;
            }
            return 0;
        }
        case PR_SET_SECCOMP :
            return seccomp_prctl_set(arg2, arg3);
        case PR_GET_SECCOMP :
            return (arg2 || arg3 || arg4 || arg5) ? -EINVAL : seccomp_prctl_get();
        case PR_SET_TIMERSLACK :
        case PR_GET_TIMERSLACK : {
            /* Return default timer slack = 50000 ns */
            if (option == PR_GET_TIMERSLACK && arg2) {
                uint64_t slack = 50000;
                if (copy_to_user((void *)arg2, &slack, sizeof(slack))) return -EFAULT;
            }
            return 0;
        }
        case PR_SET_NO_NEW_PRIVS :
            return seccomp_set_no_new_privs(arg2, arg3, arg4, arg5);
        case PR_GET_NO_NEW_PRIVS :
            return seccomp_get_no_new_privs(arg2, arg3, arg4, arg5);
        case PR_CAPBSET_READ :
            /* Report all capability bounding set bits as present. */
            if (arg2 > 63) return -EINVAL;
            return 1;
        case PR_CAPBSET_DROP :
            return 0;
        case PR_SET_SECUREBITS : {
            /*
             * systemd-executor sets SECBIT_NO_SECUREBITS etc. before
             * spawning services; without root privilege escalation on
             * this kernel, accept and record the requested mask.
             */
            task_t *task = current_task();
            if (!task) return -ESRCH;
            if (arg2 & ~0xffULL) return -EINVAL;
            task->securebits = (uint8_t)arg2;
            return 0;
        }
        case PR_GET_SECUREBITS : {
            task_t *task = current_task();
            if (!task) return -ESRCH;
            return (int64_t)task->securebits;
        }
        case PR_SET_MM :
            /*
             * systemd-executor rewrites /proc/self/cmdline bounds
             * (ARG_START/ARG_END/ENV_START/ENV_END).  This kernel does not
             * expose a writable /proc/self/cmdline yet, so the operation is
             * a no-op; accept for privileged callers.
             */
            return 0;
        case PR_SET_CHILD_SUBREAPER : {
            process_t *proc = process_current();
            if (!proc) return -ESRCH;
            proc->is_child_subreaper = !!arg2;
            return 0;
        }
        case PR_GET_CHILD_SUBREAPER : {
            process_t *proc = process_current();
            if (!proc) return -ESRCH;
            if (!arg2) return -EFAULT;

            int val = proc->is_child_subreaper ? 1 : 0;
            if (copy_to_user((void *)arg2, &val, sizeof(val))) return -EFAULT;
            return 0;
        }
        case PR_GET_TID_ADDRESS : {
            if (!arg2) return -EFAULT;
            task_t *task = current_task();
            if (!task) return -ESRCH;

            /* clear_child_tid holds the thread's tid address when set via clone */
            uintptr_t addr = (uintptr_t)task->clear_child_tid;
            if (copy_to_user((void *)arg2, &addr, sizeof(addr))) return -EFAULT;
            return 0;
        }
        default :
            /* Be permissive for systemd's optional features (ambient caps, etc.) */
            return 0;
    }
}

/* set_robust_list syscall */
int64_t sys_set_robust_list_impl(uint64_t head, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (len != 24) return -EINVAL; // sizeof(struct robust_list_head)
    task_t *task = current_task();

    /*
     * The robust-list head is unrelated to the CLONE_CHILD_CLEARTID address.
     * glibc registers the list right after a thread starts; storing it in
     * clear_child_tid redirected the exit-time "store 0 and futex-wake" to
     * the robust list, so pthread_join() waited on the real tid word forever.
     */
    if (task) task->robust_list = head;
    return 0;
}

/* get_robust_list syscall */
int64_t sys_get_robust_list_impl(uint64_t pid, uint64_t head_ptr, uint64_t len_ptr, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_t *target = proc;
    if (pid != 0 && pid != proc->task->pid) {
        target = process_find_get((pid_t)pid);
        if (!target) return -ESRCH;
    }

    uint64_t head = target->task->robust_list;
    if (head_ptr && copy_to_user((void *)head_ptr, &head, sizeof(head))) {
        if (target != proc) process_put(target);
        return -EFAULT;
    }
    if (len_ptr) {
        uint64_t sz = 24;
        if (copy_to_user((void *)len_ptr, &sz, sizeof(sz))) {
            if (target != proc) process_put(target);
            return -EFAULT;
        }
    }
    if (target != proc) process_put(target);
    return 0;
}

/* tkill syscall: send a signal to a thread */
int64_t sys_tkill_wrap(uint64_t tid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_tkill_impl((int64_t)tid, (int)sig);
}

/* set_tid_address syscall */
int64_t sys_set_tid_address_impl(uint64_t tidptr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    task_t *task = current_task();
    if (task) task->clear_child_tid = tidptr;
    return (int64_t)(task ? task->pid : 0);
}

/* pidfd_send_signal syscall */
int64_t sys_pidfd_send_signal_impl(uint64_t pidfd, uint64_t sig, uint64_t info, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)info;
    (void)arg4;
    (void)arg5;

    if (flags) return -EINVAL;
    if (pidfd >= CONFIG_PROCESS_MAX_FD) return -EBADF;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf = process_fd_get(proc, (int)pidfd);
    if (!pf) return -EBADF;

    process_t *target = pidfd_get_target(pf->node);
    if (!target) {
        process_file_put(pf);
        return -EBADF;
    }

    int ret = signal_send(target, (int)sig, NULL);
    process_file_put(pf);
    return ret;
}

/* process_vm_readv syscall: unsupported */
int64_t sys_process_vm_readv_impl(uint64_t pid, uint64_t local_iov, uint64_t local_iovcnt, uint64_t remote_iov, uint64_t remote_iovcnt, uint64_t flags)
{
    (void)pid;
    (void)local_iov;
    (void)local_iovcnt;
    (void)remote_iov;
    (void)remote_iovcnt;
    if (flags) return -EINVAL;
    return -ENOSYS;
}

/* process_vm_writev syscall: unsupported */
int64_t sys_process_vm_writev_impl(uint64_t pid, uint64_t local_iov, uint64_t local_iovcnt, uint64_t remote_iov, uint64_t remote_iovcnt, uint64_t flags)
{
    (void)pid;
    (void)local_iov;
    (void)local_iovcnt;
    (void)remote_iov;
    (void)remote_iovcnt;
    if (flags) return -EINVAL;
    return -ENOSYS;
}

/* unshare syscall */
int64_t sys_unshare_impl(uint64_t unshare_flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return namespace_unshare(unshare_flags);
}

/* setns syscall */
int64_t sys_setns_impl(uint64_t fd, uint64_t nstype, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return namespace_setns((int)fd, (int)nstype);
}

/* pidfd_getfd (438) Get a duplicate of another process's file descriptor via pidfd. */
int64_t sys_pidfd_getfd_impl(uint64_t pidfd, uint64_t targetfd, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (flags) return -EINVAL;
    if (pidfd >= CONFIG_PROCESS_MAX_FD || targetfd >= CONFIG_PROCESS_MAX_FD) return -EBADF;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf = process_fd_get(proc, (int)pidfd);
    if (!pf) return -EBADF;

    process_t *target = pidfd_get_target(pf->node);
    if (!target) {
        process_file_put(pf);
        return -EBADF;
    }

    /* Get the target's fd */
    process_file_t *tf = process_fd_get(target, (int)targetfd);
    if (!tf) {
        process_file_put(pf);
        return -EBADF;
    }

    /* Duplicate the same open-file description and set close-on-exec. */
    int newfd = process_fd_install_file(proc, tf, O_CLOEXEC);
    process_file_put(tf);
    process_file_put(pf);

    return newfd;
}
