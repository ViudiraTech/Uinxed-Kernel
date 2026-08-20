/*
 *
 *      syscall.c
 *      System call dispatch
 *
 *      2026/7/20 By Rainy101112 & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/cpuid.h>
#include <arch/fpu.h>
#include <arch/smp.h>
#include <arch/tss.h>
#include <drivers/base/device.h>
#include <drivers/firmware/acpi.h>
#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <ipc/epoll.h>
#include <ipc/futex.h>
#include <ipc/pipe.h>
#include <ipc/posix_mq.h>
#include <ipc/sysv_ipc.h>
#include <kernel/debug/debug.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/module/module.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/page.h>
#include <mem/swap.h>
#include <net/socket.h>
#include <process/elf_loader.h>
#include <process/process.h>
#include <process/ptrace.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <security/seccomp.h>
#include <sync/signal.h>
#include <syscall/eventfd.h>
#include <syscall/fcntl.h>
#include <syscall/memfd.h>
#include <syscall/mmap.h>
#include <syscall/poll.h>
#include <syscall/signalfd.h>
#include <syscall/syscall.h>
#include <syscall/syscall_basic.h>
#include <syscall/syscall_table.h>
#include <syscall/timerfd.h>

#define SYSCALL_IO_CHUNK 16384
#define EXEC_STRING_MAX  (PROCESS_STACK_SIZE / 2)

#define SYSCALL_STRINGIFY_INNER(value) #value
#define SYSCALL_STRINGIFY(value)       SYSCALL_STRINGIFY_INNER(value)

_Static_assert(offsetof(syscall_frame_t, r15) == 0, "syscall frame first register");
_Static_assert(offsetof(syscall_frame_t, rip) == 15 * sizeof(uint64_t), "syscall frame RIP offset");
_Static_assert(offsetof(syscall_frame_t, rsp) == 18 * sizeof(uint64_t), "syscall frame RSP offset");
_Static_assert(sizeof(syscall_frame_t) == 20 * sizeof(uint64_t), "syscall frame size");

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
#define CLONE_PTHREAD_REQUIRED (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM)
#define CLONE_PTHREAD_ALLOWED  (CLONE_PTHREAD_REQUIRED | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED)

#define AT_FDCWD              PROCESS_AT_FDCWD
#define STATX_BASIC_STATS     0x000007ffU
#define STATX_MNT_ID          0x00001000U
#define STATX_ATTR_MOUNT_ROOT 0x00002000ULL
#define PIDFD_NONBLOCK        0x800ULL
#define CLOSE_RANGE_UNSHARE   (1U << 1)
#define CLOSE_RANGE_CLOEXEC   (1U << 2)

typedef struct {
        uint64_t st_dev;
        uint64_t st_ino;
        uint64_t st_nlink;
        uint32_t st_mode;
        uint32_t st_uid;
        uint32_t st_gid;
        uint32_t __pad0;
        uint64_t st_rdev;
        int64_t  st_size;
        int64_t  st_blksize;
        int64_t  st_blocks;
        int64_t  st_atime;
        int64_t  st_atime_nsec;
        int64_t  st_mtime;
        int64_t  st_mtime_nsec;
        int64_t  st_ctime;
        int64_t  st_ctime_nsec;
        int64_t  __unused[3];
} linux_stat_t;

typedef struct {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
} linux_utsname_t;

typedef struct {
        int64_t  tv_sec;
        uint32_t tv_nsec;
        int32_t  __reserved;
} linux_statx_timestamp_t;

typedef struct {
        uint32_t                stx_mask;
        uint32_t                stx_blksize;
        uint64_t                stx_attributes;
        uint32_t                stx_nlink;
        uint32_t                stx_uid;
        uint32_t                stx_gid;
        uint16_t                stx_mode;
        uint16_t                __spare0;
        uint64_t                stx_ino;
        uint64_t                stx_size;
        uint64_t                stx_blocks;
        uint64_t                stx_attributes_mask;
        linux_statx_timestamp_t stx_atime;
        linux_statx_timestamp_t stx_btime;
        linux_statx_timestamp_t stx_ctime;
        linux_statx_timestamp_t stx_mtime;
        uint32_t                stx_rdev_major;
        uint32_t                stx_rdev_minor;
        uint32_t                stx_dev_major;
        uint32_t                stx_dev_minor;
        uint64_t                stx_mnt_id;
        uint32_t                stx_dio_mem_align;
        uint32_t                stx_dio_offset_align;
        uint64_t                stx_subvol;
        uint32_t                stx_atomic_write_unit_min;
        uint32_t                stx_atomic_write_unit_max;
        uint32_t                stx_atomic_write_segments_max;
        uint32_t                stx_dio_read_offset_align;
        uint64_t                __spare3[9];
} linux_statx_t;

/* Copy a path string from user space into a kernel buffer */
int copy_path_from_user(uint64_t upath, char path[SYSCALL_PATH_MAX])
{
    if (!upath) return -EFAULT;
    int ret = strncpy_from_user(path, (const char *)upath, SYSCALL_PATH_MAX);
    return ret < 0 ? ret : EOK;
}

/* Copy a path and resolve it relative to dirfd */
static int copy_resolved_path_at(process_t *proc, int dirfd, uint64_t upath, char path[SYSCALL_PATH_MAX])
{
    char input[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(upath, input);
    if (ret != EOK) return ret;
    if (!input[0]) return -ENOENT;
    return process_resolve_path_at(proc, dirfd, input, path, SYSCALL_PATH_MAX);
}

/* Resolve and open a path relative to dirfd */
static vfs_node_t open_path_at(process_t *proc, int dirfd, uint64_t upath, bool nofollow, int *error)
{
    char path[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, dirfd, upath, path);
    if (ret != EOK) {
        *error = ret;
        return NULL;
    }
    vfs_node_t node = nofollow ? vfs_open_nofollow_checked(path, error) : vfs_open_checked(path, error);
    return node;
}

/* Open the node referenced by an empty path */
static vfs_node_t open_empty_path_at(process_t *proc, int dirfd, int *error)
{
    if (dirfd == AT_FDCWD) {
        const char *cwd  = proc->cwd[0] ? proc->cwd : "/";
        vfs_node_t  node = vfs_open(cwd);
        *error           = node ? EOK : -ENOENT;
        return node;
    }
    process_file_t *file = process_fd_get(proc, dirfd);
    if (!file) {
        *error = -EBADF;
        return NULL;
    }
    vfs_node_t node = vfs_node_retain(file->node);
    process_file_put(file);
    *error = node ? EOK : -ENOENT;
    return node;
}

/* Encode a VFS node type into the mode field */
static uint32_t linux_mode_from_type(uint16_t type, uint32_t mode)
{
    uint32_t file_type = 0100000;

    if (type & file_dir)
        file_type = 0040000;
    else if (type & file_symlink)
        file_type = 0120000;
    else if (type & file_block)
        file_type = 0060000;
    else if (type & (file_stream | file_keyboard | file_mouse | file_fbdev | file_audio | file_ptmx | file_pts))
        file_type = 0020000;
    else if (type & file_pipe)
        file_type = 0010000;
    else if (type & file_socket)
        file_type = 0140000;

    return file_type | (mode & 07777);
}

/* Encode a device number in the dev_t layout */
static uint64_t linux_encode_dev(uint64_t dev)
{
    uint32_t major = MAJOR(dev);
    uint32_t minor = MINOR(dev);
    return (minor & 0xffU) | ((uint64_t)major << 8) | ((uint64_t)(minor & ~0xffU) << 12);
}

/* Fill a stat structure from a VFS node snapshot */
static void fill_linux_stat(linux_stat_t *st, uint64_t uid, uint64_t gid, const process_fd_stat_t *src)
{
    memset(st, 0, sizeof(*st));
    st->st_dev     = linux_encode_dev(src->dev);
    st->st_ino     = src->inode;
    st->st_nlink   = src->nlink ? src->nlink : 1;
    st->st_mode    = linux_mode_from_type(src->type, src->mode);
    st->st_uid     = (uint32_t)uid;
    st->st_gid     = (uint32_t)gid;
    st->st_rdev    = linux_encode_dev(src->rdev);
    st->st_size    = (int64_t)src->size;
    st->st_blksize = src->blksz ? (int64_t)src->blksz : 4096;
    st->st_blocks  = (st->st_size + 511) / 512;
    st->st_atime   = (int64_t)src->atime;
    st->st_mtime   = (int64_t)src->mtime;
    st->st_ctime   = (int64_t)src->ctime;
}

/* Find the nearest mount point at or above a node */
static vfs_node_t vfs_containing_mount(vfs_node_t node)
{
    for (vfs_node_t current = node; current; current = current->parent) {
        if (current->is_mount) return current;
        if (current == current->parent) break;
    }
    return NULL;
}

/* Fill a statx structure from a VFS node snapshot */
static void fill_linux_statx(linux_statx_t *stx, uint64_t uid, uint64_t gid, const process_fd_stat_t *src, vfs_node_t node)
{
    memset(stx, 0, sizeof(*stx));
    stx->stx_mask         = STATX_BASIC_STATS;
    stx->stx_blksize      = src->blksz ? (uint32_t)src->blksz : 4096;
    stx->stx_nlink        = src->nlink ? src->nlink : 1;
    stx->stx_uid          = (uint32_t)uid;
    stx->stx_gid          = (uint32_t)gid;
    stx->stx_mode         = (uint16_t)linux_mode_from_type(src->type, src->mode);
    stx->stx_ino          = src->inode;
    stx->stx_size         = src->size;
    stx->stx_blocks       = (src->size + 511) / 512;
    stx->stx_rdev_major   = MAJOR(src->rdev);
    stx->stx_rdev_minor   = MINOR(src->rdev);
    stx->stx_dev_major    = MAJOR(src->dev);
    stx->stx_dev_minor    = MINOR(src->dev);
    stx->stx_atime.tv_sec = (int64_t)src->atime;
    stx->stx_mtime.tv_sec = (int64_t)src->mtime;
    stx->stx_ctime.tv_sec = (int64_t)src->ctime;
    vfs_node_t mount      = vfs_containing_mount(node);
    if (mount) {
        stx->stx_mask |= STATX_MNT_ID;
        stx->stx_mnt_id = mount->mount_id;
    }
    stx->stx_attributes_mask |= STATX_ATTR_MOUNT_ROOT;
    if (node->is_mount || node == rootdir) stx->stx_attributes |= STATX_ATTR_MOUNT_ROOT;
}

/* stat a path and copy the result to user space */
static int64_t stat_path_to_user(uint64_t upath, uint64_t ubuf)
{
    char path[SYSCALL_PATH_MAX];
    if (!ubuf) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int ret = copy_resolved_path_at(proc, AT_FDCWD, upath, path);
    if (ret != EOK) return ret;

    vfs_node_t node = vfs_open_checked(path, &ret);
    if (!node) return ret;
    vfs_update(node);

    process_fd_stat_t src = {
        .dev   = node->dev,
        .inode = node->inode,
        .nlink = node->nlink,
        .mode  = node->mode,
        .type  = node->type,
        .rdev  = node->rdev,
        .size  = node->size,
        .blksz = node->blksz,
        .atime = node->readtime,
        .mtime = node->writetime,
        .ctime = node->createtime,
    };
    linux_stat_t st;
    fill_linux_stat(&st, node->owner, node->group, &src);
    vfs_close(node);
    return copy_to_user((void *)ubuf, &st, sizeof(st)) ? -EFAULT : EOK;
}

/* stat a node and copy the result to user space */
static int64_t stat_node_to_user(vfs_node_t node, uint64_t ubuf)
{
    if (!ubuf) return -EFAULT;
    vfs_update(node);
    process_fd_stat_t src = {
        .dev   = node->dev,
        .inode = node->inode,
        .nlink = node->nlink,
        .mode  = node->mode,
        .type  = node->type,
        .rdev  = node->rdev,
        .size  = node->size,
        .blksz = node->blksz,
        .atime = node->readtime,
        .mtime = node->writetime,
        .ctime = node->createtime,
    };
    linux_stat_t st;
    fill_linux_stat(&st, node->owner, node->group, &src);
    return copy_to_user((void *)ubuf, &st, sizeof(st)) ? -EFAULT : EOK;
}

/* statx a node and copy the result to user space */
static int64_t statx_node_to_user(vfs_node_t node, uint64_t ubuf)
{
    if (!ubuf) return -EFAULT;
    vfs_update(node);

    process_fd_stat_t src = {
        .dev   = node->dev,
        .inode = node->inode,
        .nlink = node->nlink,
        .mode  = node->mode,
        .type  = node->type,
        .rdev  = node->rdev,
        .size  = node->size,
        .blksz = node->blksz,
        .atime = node->readtime,
        .mtime = node->writetime,
        .ctime = node->createtime,
    };
    linux_statx_t stx;
    fill_linux_statx(&stx, node->owner, node->group, &src, node);
    return copy_to_user((void *)ubuf, &stx, sizeof(stx)) ? -EFAULT : EOK;
}

/* Return the basename of a path */
const char *path_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

/* Open the parent directory of a path */
vfs_node_t vfs_open_parent_of(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return vfs_open("/");
    *slash = '\0';
    return vfs_open(path);
}

/* exit syscall: terminate the calling thread */
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
    return task ? (int64_t)task->tgid : -ESRCH;
}

static int64_t sys_sched_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* Current time in nanoseconds for the requested clock */
static uint64_t clock_sleep_now_ns(int clockid)
{
    uint64_t uptime_ns = timer_monotonic_ns();
    if (clockid != TIMER_CLOCK_REALTIME) return uptime_ns;

    int64_t realtime_ns = timer_realtime_ns();
    return realtime_ns > 0 ? (uint64_t)realtime_ns : 0;
}

/* Check whether a signal is pending for the current process */
static bool clock_sleep_signal_pending(void)
{
    process_t *proc = process_current();
    return proc && signal_has_pending(&proc->signal);
}

/* Sleep until a deadline or signal delivery */
static int64_t clock_sleep(uint64_t clockid, uint64_t flags, uint64_t req, uint64_t rem)
{
    if (!timer_clock_sleep_supported(clockid, flags)) return -EINVAL;
    if (!req) return -EFAULT;

    timer_timespec_t request;
    if (copy_from_user(&request, (const void *)req, sizeof(request))) return -EFAULT;

    uint64_t now_ns = clock_sleep_now_ns((int)clockid);
    uint64_t duration_ns;
    uint64_t sleep_ticks;
    if (!timer_sleep_duration(&request, now_ns, flags == TIMER_ABSTIME, &duration_ns, &sleep_ticks)) return -EINVAL;
    if (!sleep_ticks) return EOK;

    uint64_t     start_tick    = sched_ticks();
    uint64_t     deadline_tick = UINT64_MAX - start_tick < sleep_ticks ? UINT64_MAX : start_tick + sleep_ticks;
    wait_queue_t sleep_queue;
    wait_queue_init(&sleep_queue);

    for (;;) {
        uint64_t now_tick = sched_ticks();
        if (now_tick >= deadline_tick) return EOK;

        wait_queue_prepare(&sleep_queue);
        now_tick = sched_ticks();
        if (clock_sleep_signal_pending()) {
            wait_queue_wake_one(&sleep_queue);
            if (!(flags & TIMER_ABSTIME) && rem) {
                uint64_t         elapsed_ticks = now_tick - start_tick;
                uint64_t         elapsed_ns    = elapsed_ticks > UINT64_MAX / TIMER_TICK_NS ? UINT64_MAX : elapsed_ticks * TIMER_TICK_NS;
                uint64_t         remaining_ns  = elapsed_ns < duration_ns ? duration_ns - elapsed_ns : 0;
                timer_timespec_t remaining     = timer_ns_to_timespec(remaining_ns);
                if (copy_to_user((void *)rem, &remaining, sizeof(remaining))) return -EFAULT;
            }
            return -EINTR;
        }

        if (now_tick >= deadline_tick) {
            wait_queue_wake_one(&sleep_queue);
            return EOK;
        }
        wait_queue_wait_timed(&sleep_queue, deadline_tick);
    }
}

static int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    return clock_sleep(TIMER_CLOCK_MONOTONIC, 0, req, rem);
}

#define WNOHANG    0x00000001
#define WUNTRACED  0x00000002
#define WCONTINUED 0x00000008

/* wait4 syscall: wait for a child to change state */
static int64_t sys_wait4(uint64_t pid, uint64_t exit_code, uint64_t options, uint64_t rusage, uint64_t arg4, uint64_t arg5)
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

static int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_kill_impl((pid_t)pid, (int)sig);
}

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t pgoff)
{
    return sys_mmap_pgoff(addr, length, prot, flags, fd, pgoff);
}

static int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_munmap_full(addr, length);
}

/* brk syscall: adjust the program break */
static int64_t sys_brk(uint64_t addr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return 0;
    spin_lock(&proc->brk_lock);
    uintptr_t old_brk = proc->heap_brk;
    if (!addr) {
        spin_unlock(&proc->brk_lock);
        return (int64_t)old_brk;
    }
    if (addr < proc->start_brk || addr > PROCESS_HEAP_MAX) {
        spin_unlock(&proc->brk_lock);
        return (int64_t)old_brk;
    }

    if (addr > old_brk) {
        uintptr_t start = ALIGN_UP(old_brk, PAGE_4K_SIZE);
        uintptr_t end   = ALIGN_UP(addr, PAGE_4K_SIZE);
        if (end > start && process_mmap(proc, start, end - start, VM_READ | VM_WRITE | VM_LAZY)) {
            spin_unlock(&proc->brk_lock);
            return (int64_t)old_brk;
        }
    } else if (addr < old_brk) {
        uintptr_t start = ALIGN_UP(addr, PAGE_4K_SIZE);
        uintptr_t end   = ALIGN_UP(old_brk, PAGE_4K_SIZE);
        if (end > start && sys_munmap_full(start, end - start) != EOK) {
            spin_unlock(&proc->brk_lock);
            return (int64_t)old_brk;
        }
    }
    proc->heap_brk = addr;
    spin_unlock(&proc->brk_lock);
    return (int64_t)addr;
}

/* open syscall: open or create a file */
static int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!path) return -EFAULT;
    if ((flags & O_TMPFILE) == O_TMPFILE) return -EOPNOTSUPP;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[SYSCALL_PATH_MAX];
    int  copied = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (copied != EOK) return copied;

    int        lookup_error = EOK;
    vfs_node_t node         = (flags & O_NOFOLLOW) ? vfs_open_nofollow_checked(name, &lookup_error) : vfs_open_checked(name, &lookup_error);
    if (node && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        vfs_close(node);
        return -EEXIST;
    }
    if (!node && (flags & O_CREAT)) {
        int ret = vfs_mkfile_mode(name, (uint16_t)(mode & 07777U & ~proc->umask));
        if (ret != EOK && ret != -EEXIST) return ret;
        node = vfs_open_checked(name, &lookup_error);
    }
    if (!node) return lookup_error;
    if ((node->type & file_symlink) && (flags & O_NOFOLLOW) && !(flags & O_PATH)) {
        vfs_close(node);
        return -ELOOP;
    }
    if ((flags & O_DIRECTORY) && !(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }

    if (!(flags & O_PATH)) {
        uint32_t access_mask = 0;
        if ((flags & O_ACCMODE) == O_RDONLY || (flags & O_ACCMODE) == O_RDWR) access_mask |= VFS_ACCESS_R;
        if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR) access_mask |= VFS_ACCESS_W;
        if (vfs_access_check(node, access_mask)) {
            vfs_close(node);
            return -EACCES;
        }
    }

    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY && (node->type & ~file_delete) == file_none) {
        if (vfs_mount_is_readonly(node)) {
            vfs_close(node);
            return -EROFS;
        }
        int result = vfs_truncate(node, 0);
        if (result) {
            vfs_close(node);
            return result;
        }
    }

    int fd = process_fd_install(proc, node, flags);
    if (fd < 0) vfs_close(node);
    return fd;
}

/* openat syscall: open or create a file by dirfd+path */
static int64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    if (!path) return -EFAULT;
    if ((flags & O_TMPFILE) == O_TMPFILE) return -EOPNOTSUPP;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)dirfd, path, name);
    if (ret != EOK) return ret;
    int        lookup_error = EOK;
    vfs_node_t node         = (flags & O_NOFOLLOW) ? vfs_open_nofollow_checked(name, &lookup_error) : vfs_open_checked(name, &lookup_error);
    if (node && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        vfs_close(node);
        return -EEXIST;
    }
    if (!node && (flags & O_CREAT)) {
        ret = vfs_mkfile_mode(name, (uint16_t)(mode & 07777U & ~proc->umask));
        if (ret != EOK && ret != -EEXIST) return ret;
        node = vfs_open_checked(name, &lookup_error);
    }
    if (!node) return lookup_error;
    if ((node->type & file_symlink) && (flags & O_NOFOLLOW) && !(flags & O_PATH)) {
        vfs_close(node);
        return -ELOOP;
    }
    if ((flags & O_DIRECTORY) && !(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }

    if (!(flags & O_PATH)) {
        uint32_t access = 0;
        if ((flags & O_ACCMODE) == O_RDONLY || (flags & O_ACCMODE) == O_RDWR) access |= VFS_ACCESS_R;
        if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR) access |= VFS_ACCESS_W;
        if (vfs_access_check(node, access)) {
            vfs_close(node);
            return -EACCES;
        }
    }
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY && (node->type & ~file_delete) == file_none) {
        if (vfs_mount_is_readonly(node)) {
            vfs_close(node);
            return -EROFS;
        }
        ret = vfs_truncate(node, 0);
        if (ret) {
            vfs_close(node);
            return ret;
        }
    }
    int fd = process_fd_install(proc, node, flags);
    if (fd < 0) vfs_close(node);
    return fd;
}

/* close_range syscall: close or mark a range of descriptors */
static int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (first > last) return -EINVAL;
    if (flags & ~(uint64_t)(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) return -EINVAL;
    if (flags & CLOSE_RANGE_UNSHARE) return -EINVAL;

    if (first >= PROCESS_MAX_FD) return EOK;
    uint64_t end = last < (uint64_t)(PROCESS_MAX_FD - 1) ? last : (uint64_t)(PROCESS_MAX_FD - 1);

    if (flags & CLOSE_RANGE_CLOEXEC) {
        spin_lock(&proc->fd_lock);
        for (uint64_t fd = first; fd <= end; fd++) {
            if (proc->fds[fd]) proc->fd_flags[fd] |= FD_CLOEXEC;
        }
        spin_unlock(&proc->fd_lock);
        return EOK;
    }

    int status = 0;
    for (uint64_t fd = first; fd <= end; fd++) {
        int ret = process_fd_close(proc, (int)fd);
        if (ret != EOK && ret != -EBADF) status = ret;
    }
    return status;
}

static int64_t sys_creat(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_open(path, O_CREAT | O_WRONLY, mode, 0, 0, 0);
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

static int64_t sys_read_task(task_t *task, uint64_t fd, uint64_t buf, uint64_t size)
{
    if (!buf && size) return -EFAULT;
    process_t *proc = task ? task->process : NULL;
    if (!proc) return -ESRCH;

    return process_fd_read_user(proc, (int)fd, (void *)buf, (size_t)size);
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_read_task(current_task(), fd, buf, size);
}

static int64_t sys_write_task(task_t *task, uint64_t fd, uint64_t buf, uint64_t size)
{
    if (!buf && size) return -EFAULT;
    process_t *proc = task ? task->process : NULL;
    if (!proc) return -ESRCH;

    /*
     * The VFS fallback still invokes zero-length writes, preserving empty
     * datagram semantics without forcing every ordinary write through a
     * syscall-layer bounce buffer.
     */
    return process_fd_write_user(proc, (int)fd, (const void *)buf, (size_t)size);
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_write_task(current_task(), fd, buf, size);
}

/* arch_prctl syscall: set or get FS/GS base */
static int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    task_t *task = current_task();

    switch (code) {
        case 0x1002 : // ARCH_SET_FS
            wrmsr(0xC0000100, addr);
            if (task) task->thread.fs_base = addr;
            return 0;
        case 0x1003 : { // ARCH_GET_FS
            uint64_t fs = task ? task->thread.fs_base : 0;
            return copy_to_user((void *)addr, &fs, sizeof(fs));
        }
        case 0x1004 : // ARCH_SET_GS
            /* In kernel mode IA32_GS_BASE is the per-CPU base; the user GS lives in KERNEL_GS_BASE. */
            set_user_gs_base(addr);
            if (task) task->thread.gs_base = addr;
            return 0;
        case 0x1005 : { // ARCH_GET_GS
            uint64_t gs = task ? task->thread.gs_base : 0;
            return copy_to_user((void *)addr, &gs, sizeof(gs));
        }
        default :
            return -EINVAL;
    }
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

static int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (flags & ~(uint64_t)O_CLOEXEC) return -EINVAL;
    if (oldfd == newfd) return -EINVAL;
    int64_t result = sys_dup2(oldfd, newfd, 0, 0, 0, 0);
    if (result < 0 || !(flags & O_CLOEXEC)) return result;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    spin_lock(&proc->fd_lock);
    if (newfd < PROCESS_MAX_FD && proc->fds[newfd])
        proc->fd_flags[newfd] = FD_CLOEXEC;
    else
        result = -EBADF;
    spin_unlock(&proc->fd_lock);
    return result;
}

/* ioctl syscall: issue a device control command */
static int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* Commands may arrive sign-extended when libc exposes them as int; truncate at the ABI boundary. */
    return process_fd_ioctl(proc, (int)fd, (size_t)(uint32_t)req, (void *)arg);
}

/* fstat syscall: stat an open descriptor */
static int64_t sys_fstat(uint64_t fd, uint64_t statbuf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!statbuf) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    vfs_update(file->node);
    process_fd_stat_t fdst = {
        .dev   = file->node->dev,
        .inode = file->node->inode,
        .nlink = file->node->nlink,
        .mode  = file->node->mode,
        .type  = file->node->type,
        .rdev  = file->node->rdev,
        .size  = file->node->size,
        .blksz = file->node->blksz,
        .atime = file->node->readtime,
        .mtime = file->node->writetime,
        .ctime = file->node->createtime,
    };
    uint32_t owner = file->node->owner;
    uint32_t group = file->node->group;
    process_file_put(file);

    linux_stat_t st;
    fill_linux_stat(&st, owner, group, &fdst);
    return copy_to_user((void *)statbuf, &st, sizeof(st)) ? -EFAULT : EOK;
}

/* statx syscall: extended file status */
static int64_t sys_statx(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mask, uint64_t statbuf, uint64_t arg5)
{
    (void)arg5;
    if (!statbuf) return -EFAULT;
    if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE)) return -EINVAL;
    if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE) return -EINVAL;
    (void)mask;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[SYSCALL_PATH_MAX];
    int  ret;
    if (!path) {
        if (!(flags & AT_EMPTY_PATH)) return -EFAULT;
        input[0] = '\0';
    } else {
        ret = copy_path_from_user(path, input);
        if (ret != EOK) return ret;
    }

    vfs_node_t node = NULL;
    if (!input[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        node = open_empty_path_at(proc, (int)dirfd, &ret);
    } else {
        char resolved[SYSCALL_PATH_MAX];
        ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
        if (ret == EOK) node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow(resolved) : vfs_open(resolved);
        if (!node && ret == EOK) ret = -ENOENT;
    }
    if (!node) return ret;
    ret = (int)statx_node_to_user(node, statbuf);
    vfs_close(node);
    return ret;
}

static int64_t sys_gettimeofday(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tz;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tv) return 0;

    int64_t         uptime_ns = timer_realtime_ns();
    linux_timeval_t now       = {
              .tv_sec  = uptime_ns / 1000000000LL,
              .tv_usec = (uptime_ns % 1000000000LL) / 1000,
    };
    return copy_to_user((void *)tv, &now, sizeof(now)) ? -EFAULT : 0;
}

static int64_t sys_time(uint64_t tloc, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    int64_t now = timer_realtime_ns() / 1000000000LL;
    if (tloc && copy_to_user((void *)tloc, &now, sizeof(now))) return -EFAULT;
    return now;
}

static int64_t sys_exit_group(uint64_t status, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_exit_group((int)status);
}

static int64_t sys_stat(uint64_t path, uint64_t statbuf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return stat_path_to_user(path, statbuf);
}

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t statbuf, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
    if (!path) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret != EOK) return ret;
    vfs_node_t node;
    if (!input[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        node = open_empty_path_at(proc, (int)dirfd, &ret);
    } else {
        node = open_path_at(proc, (int)dirfd, path, (flags & AT_SYMLINK_NOFOLLOW) != 0, &ret);
    }
    if (!node) return ret;
    ret = (int)stat_node_to_user(node, statbuf);
    vfs_close(node);
    return ret;
}

/* uname syscall: report kernel identity */
static int64_t sys_uname(uint64_t name, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!name) return -EFAULT;

    linux_utsname_t uts;
    memset(&uts, 0, sizeof(uts));
    strncpy(uts.sysname, KERNEL_NAME, sizeof(uts.sysname) - 1);
    strncpy(uts.nodename, "localhost", sizeof(uts.nodename) - 1);
    strncpy(uts.release, KERNEL_VERSION, sizeof(uts.release) - 1);
    strncpy(uts.version, BUILD_DATE " " BUILD_TIME, sizeof(uts.version) - 1);
    strncpy(uts.machine, "x86_64", sizeof(uts.machine) - 1);
    strncpy(uts.domainname, "localdomain", sizeof(uts.domainname) - 1);
    return copy_to_user((void *)name, &uts, sizeof(uts)) ? -EFAULT : EOK;
}

static int64_t sys_getuid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    return proc ? proc->uid : 0;
}

static int64_t sys_getgid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    return proc ? proc->gid : 0;
}

static int64_t sys_getppid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

static int64_t sys_gettid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

static int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    return ret != EOK ? ret : vfs_mkdir_mode(name, (uint16_t)(mode & 07777U & ~proc->umask));
}

static int64_t sys_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)dirfd, path, name);
    return ret == EOK ? vfs_mkdir_mode(name, (uint16_t)(mode & 07777U & ~proc->umask)) : ret;
}

static int64_t sys_unlink(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int        ret;
    vfs_node_t node = open_path_at(proc, AT_FDCWD, path, true, &ret);
    if (!node) return ret;
    if (node->type & file_dir) {
        vfs_close(node);
        return -EISDIR;
    }
    ret = vfs_delete(node);
    vfs_close(node);
    return ret;
}

static int64_t sys_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (flags & ~AT_REMOVEDIR) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int        ret;
    vfs_node_t node = open_path_at(proc, (int)dirfd, path, true, &ret);
    if (!node) return ret;
    if ((flags & AT_REMOVEDIR) && !(node->type & file_dir))
        ret = -ENOTDIR;
    else if (!(flags & AT_REMOVEDIR) && (node->type & file_dir))
        ret = -EISDIR;
    else
        ret = vfs_delete(node);
    vfs_close(node);
    return ret;
}

static int64_t sys_rename(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char oldname[SYSCALL_PATH_MAX];
    char newname[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, oldpath, oldname);
    if (ret != EOK) return ret;
    ret = copy_resolved_path_at(proc, AT_FDCWD, newpath, newname);
    if (ret != EOK) return ret;
    vfs_node_t node = vfs_open_nofollow(oldname);
    if (!node) return -ENOENT;
    char oldparent[SYSCALL_PATH_MAX];
    char newparent[SYSCALL_PATH_MAX];
    memcpy(oldparent, oldname, sizeof(oldparent));
    memcpy(newparent, newname, sizeof(newparent));
    vfs_node_t olddir = vfs_open_parent_of(oldparent);
    vfs_node_t newdir = vfs_open_parent_of(newparent);
    if (!olddir || !newdir)
        ret = -ENOENT;
    else
        ret = vfs_rename(node, newdir, path_basename(newname), 0);
    if (olddir) vfs_close(olddir);
    if (newdir) vfs_close(newdir);
    vfs_close(node);
    return ret;
}

static int64_t sys_renameat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char oldname[SYSCALL_PATH_MAX];
    char newname[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)olddirfd, oldpath, oldname);
    if (ret != EOK) return ret;
    ret = copy_resolved_path_at(proc, (int)newdirfd, newpath, newname);
    if (ret != EOK) return ret;

    vfs_node_t node = vfs_open_nofollow(oldname);
    if (!node) return -ENOENT;
    char oldparent[SYSCALL_PATH_MAX];
    char newparent[SYSCALL_PATH_MAX];
    memcpy(oldparent, oldname, sizeof(oldparent));
    memcpy(newparent, newname, sizeof(newparent));
    vfs_node_t olddir = vfs_open_parent_of(oldparent);
    vfs_node_t newdir = vfs_open_parent_of(newparent);
    if (!olddir || !newdir)
        ret = -ENOENT;
    else
        ret = vfs_rename(node, newdir, path_basename(newname), 0);
    if (olddir) vfs_close(olddir);
    if (newdir) vfs_close(newdir);
    vfs_close(node);
    return ret;
}

static int64_t sys_link(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char oldname[SYSCALL_PATH_MAX];
    char newname[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, oldpath, oldname);
    if (ret != EOK) return ret;
    ret = copy_resolved_path_at(proc, AT_FDCWD, newpath, newname);
    if (ret != EOK) return ret;
    return vfs_link(newname, oldname);
}

static int64_t sys_linkat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char oldname[SYSCALL_PATH_MAX];
    char newname[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)olddirfd, oldpath, oldname);
    if (ret != EOK) return ret;
    ret = copy_resolved_path_at(proc, (int)newdirfd, newpath, newname);
    if (ret != EOK) return ret;
    return (flags & AT_SYMLINK_FOLLOW) ? vfs_link_follow(newname, oldname) : vfs_link(newname, oldname);
}

static int64_t sys_symlink(uint64_t target, uint64_t linkpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char target_name[SYSCALL_PATH_MAX];
    char link_name[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(target, target_name);
    if (ret != EOK) return ret;
    ret = copy_resolved_path_at(proc, AT_FDCWD, linkpath, link_name);
    if (ret != EOK) return ret;
    return vfs_symlink(link_name, target_name);
}

static int64_t sys_symlinkat(uint64_t target, uint64_t newdirfd, uint64_t linkpath, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char target_name[SYSCALL_PATH_MAX];
    char link_name[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(target, target_name);
    if (ret != EOK) return ret;
    ret = copy_resolved_path_at(proc, (int)newdirfd, linkpath, link_name);
    return ret == EOK ? vfs_symlink(link_name, target_name) : ret;
}

static int64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!buf && bufsiz) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;

    vfs_node_t node = vfs_open_nofollow(name);
    if (!node) return -ENOENT;
    if (!(node->type & file_symlink)) {
        vfs_close(node);
        return -EINVAL;
    }
    char   tmp[SYSCALL_PATH_MAX];
    size_t len = vfs_readlink(node, tmp, sizeof(tmp));
    vfs_close(node);
    if (len > bufsiz) len = bufsiz;
    if (len && copy_to_user((void *)buf, tmp, len)) return -EFAULT;
    return (int64_t)len;
}

static int64_t sys_readlinkat(uint64_t dirfd, uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    if (!buf && bufsiz) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char input[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret != EOK) return ret;
    vfs_node_t node = input[0] ? open_path_at(proc, (int)dirfd, path, true, &ret) : open_empty_path_at(proc, (int)dirfd, &ret);
    if (!node) return ret;
    if (!(node->type & file_symlink)) {
        vfs_close(node);
        return -EINVAL;
    }
    char   tmp[SYSCALL_PATH_MAX];
    size_t len = vfs_readlink(node, tmp, sizeof(tmp));
    vfs_close(node);
    if (len > bufsiz) len = bufsiz;
    if (len && copy_to_user((void *)buf, tmp, len)) return -EFAULT;
    return (int64_t)len;
}

static int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!buf) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    const char *cwd      = proc->cwd[0] ? proc->cwd : "/";
    const char *root     = proc->root[0] ? proc->root : "/";
    size_t      root_len = strlen(root);
    if (root_len != 1 && !strncmp(cwd, root, root_len) && (!cwd[root_len] || cwd[root_len] == '/')) cwd = cwd[root_len] ? cwd + root_len : "/";
    size_t len = strlen(cwd) + 1;
    if (len > size) return -ERANGE;
    return copy_to_user((void *)buf, cwd, len) ? -EFAULT : (int64_t)len;
}

/* eventfd, timerfd, signalfd wrappers */
static int64_t sys_eventfd_wrap(uint64_t initval, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_eventfd((unsigned int)initval, 0);
}

static int64_t sys_eventfd2_wrap(uint64_t initval, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_eventfd2((unsigned int)initval, (int)flags);
}

static int64_t sys_timerfd_create_wrap(uint64_t clockid, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_timerfd_create((int)clockid, (int)flags);
}

static int64_t sys_timerfd_settime_wrap(uint64_t fd, uint64_t flags, uint64_t new_value, uint64_t old_value, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_timerfd_settime((int)fd, (int)flags, (const void *)new_value, (void *)old_value);
}

static int64_t sys_timerfd_gettime_wrap(uint64_t fd, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_timerfd_gettime((int)fd, (void *)curr_value);
}

static int64_t sys_signalfd_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_signalfd4((int)fd, (const void *)mask, (size_t)sizemask, 0);
}

static int64_t sys_signalfd4_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_signalfd4((int)fd, (const void *)mask, (size_t)sizemask, (int)flags);
}

static int64_t sys_ptrace_wrap(uint64_t request, uint64_t pid, uint64_t addr, uint64_t data, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_ptrace((int)request, (int64_t)pid, (uintptr_t)addr, (uintptr_t)data);
}

static int64_t sys_inotify_init_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_init();
}

static int64_t sys_inotify_init1_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_init1((int)flags);
}

static int64_t sys_inotify_add_watch_wrap(uint64_t fd, uint64_t pathname, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_add_watch((int)fd, (const char *)pathname, (uint32_t)mask);
}

static int64_t sys_inotify_rm_watch_wrap(uint64_t fd, uint64_t wd, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_rm_watch((int)fd, (int)wd);
}

/* mount / umount2 */

#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT     32
#define MS_MANDLOCK    64
#define MS_DIRSYNC     128
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096
#define MS_MOVE        8192
#define MS_REC         16384
#define MS_SILENT      32768

#define MNT_FORCE  1
#define MNT_DETACH 2
#define MNT_EXPIRE 4

/* mount syscall: attach a filesystem */
static int64_t sys_mount(uint64_t source, uint64_t target, uint64_t fstype, uint64_t flags, uint64_t data, uint64_t arg5)
{
    (void)data;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;

    char src[SYSCALL_PATH_MAX] = {0};
    char tgt[SYSCALL_PATH_MAX] = {0};
    char fst[SYSCALL_PATH_MAX] = {0};

    if (!target) return -EFAULT;

    int path_ret = copy_resolved_path_at(proc, AT_FDCWD, target, tgt);
    if (path_ret != EOK) return path_ret;

    /* Copy source path (optional - can be NULL for virtual filesystems) */
    if (source)
        if (strncpy_from_user(src, (const char *)source, sizeof(src)) < 0) return -EFAULT;

    /* Copy filesystem type (optional - can be NULL to let VFS probe) */
    if (fstype)
        if (strncpy_from_user(fst, (const char *)fstype, sizeof(fst)) < 0) return -EFAULT;

    /* Open the target mount point */
    vfs_node_t node = vfs_open(tgt);
    if (!node) return -ENOENT;
    if (!(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }

    /* Handle MS_REMOUNT: change flags on an existing mount */
    if (flags & MS_REMOUNT) {
        if (!node->is_mount) {
            vfs_close(node);
            return -EINVAL;
        }
        node->flags &= ~(MOUNT_FLAG_RDONLY | MOUNT_FLAG_NOSUID | MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC);
        if (flags & MS_RDONLY) node->flags |= MOUNT_FLAG_RDONLY;
        if (flags & MS_NOSUID) node->flags |= MOUNT_FLAG_NOSUID;
        if (flags & MS_NODEV) node->flags |= MOUNT_FLAG_NODEV;
        if (flags & MS_NOEXEC) node->flags |= MOUNT_FLAG_NOEXEC;
        vfs_close(node);
        return EOK;
    }

    /* Handle MS_MOVE: move an existing mount to a new location */
    if (flags & MS_MOVE) {
        vfs_close(node);
        return -ENOSYS;
    }

    /* Perform the mount */
    int ret;
    if (fst[0]) {
        ret = vfs_mount_fs(fst, src[0] ? src : NULL, node);
    } else {
        ret = vfs_mount(src[0] ? src : NULL, node);
    }

    if (ret != EOK) {
        vfs_close(node);
        return ret;
    }

    /* Apply mount flags to the mount point node */
    node->flags &= ~(MOUNT_FLAG_RDONLY | MOUNT_FLAG_NOSUID | MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC);
    if (flags & MS_RDONLY) node->flags |= MOUNT_FLAG_RDONLY;
    if (flags & MS_NOSUID) node->flags |= MOUNT_FLAG_NOSUID;
    if (flags & MS_NODEV) node->flags |= MOUNT_FLAG_NODEV;
    if (flags & MS_NOEXEC) node->flags |= MOUNT_FLAG_NOEXEC;

    /* Mark the node as a mount point (if not already) */
    node->is_mount = 1;

    vfs_close(node);

    /* MS_REC: recursive - ignored for non-bind mounts */
    (void)(flags & MS_REC);

    return EOK;
}

/* umount2 syscall: detach a filesystem */
static int64_t sys_umount2(uint64_t target, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char tgt[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, target, tgt);

    if (ret != EOK) return ret;
    if (flags & ~(MNT_FORCE | MNT_DETACH | MNT_EXPIRE)) return -EINVAL;

    return vfs_umount(tgt);
}

/* Generic syscall stub: return -ENOSYS */
static int64_t sys_stub(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS;
}

/* Shared access/faccessat implementation */
static int64_t sys_access_common(int dirfd, uint64_t path, uint64_t mode, uint64_t flags)
{
    if (mode & ~7ULL) return -EINVAL;
    if (flags & ~(uint64_t)(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char input[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret != EOK) return ret;

    vfs_node_t node = NULL;
    if (!input[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        node = open_empty_path_at(proc, dirfd, &ret);
    } else {
        char name[SYSCALL_PATH_MAX];
        ret = process_resolve_path_at(proc, dirfd, input, name, sizeof(name));
        if (ret == EOK) node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow(name) : vfs_open(name);
        if (!node && ret == EOK) ret = -ENOENT;
    }
    if (!node) return ret;

    /* R_OK=4, W_OK=2, X_OK=1, F_OK=0 */
    if (mode & 4) { // R_OK
        if (vfs_access_check(node, VFS_ACCESS_R)) {
            vfs_close(node);
            return -EACCES;
        }
    }
    if (mode & 2) { // W_OK
        if (vfs_access_check(node, VFS_ACCESS_W)) {
            vfs_close(node);
            return -EACCES;
        }
    }
    if (mode & 1) { // X_OK
        if (vfs_access_check(node, VFS_ACCESS_X)) {
            vfs_close(node);
            return -EACCES;
        }
    }
    vfs_close(node);
    return EOK;
}

static int64_t sys_access_impl(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_access_common(AT_FDCWD, path, mode, 0);
}

static int64_t sys_faccessat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_access_common((int)dirfd, path, mode, 0);
}

static int64_t sys_faccessat2_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_access_common((int)dirfd, path, mode, flags);
}

static int64_t sys_clock_settime_impl(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tp) return -EFAULT;
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_REALTIME_COARSE && clockid != CLOCK_REALTIME_ALARM) return -EINVAL;

    linux_timespec_t ts;
    if (copy_from_user(&ts, (const void *)tp, sizeof(ts))) return -EFAULT;
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) return -EINVAL;

    timer_realtime_set_ns(ts.tv_sec * 1000000000LL + ts.tv_nsec);
    return EOK;
}

/* sysinfo */

typedef struct linux_sysinfo {
        int64_t  uptime;
        uint64_t loads[3];
        uint64_t totalram;
        uint64_t freeram;
        uint64_t sharedram;
        uint64_t bufferram;
        uint64_t totalswap;
        uint64_t freeswap;
        uint16_t procs;
        uint16_t pad;
        uint64_t totalhigh;
        uint64_t freehigh;
        uint32_t mem_unit;
        char     _f[20 - 2 * sizeof(uint64_t) - sizeof(uint32_t)];
} linux_sysinfo_t;

/* statfs */

typedef struct linux_statfs {
        int64_t  f_type;
        int64_t  f_bsize;
        uint64_t f_blocks;
        uint64_t f_bfree;
        uint64_t f_bavail;
        uint64_t f_files;
        uint64_t f_ffree;
        uint64_t f_fsid;
        int64_t  f_namelen;
        int64_t  f_frsize;
        int64_t  f_flags;
        int64_t  f_spare[4];
} linux_statfs_t;

#define TMPFS_MAGIC         0x01021994
#define SYSFS_MAGIC         0x62656572
#define PROC_SUPER_MAGIC    0x00009fa0
#define CGROUP2_SUPER_MAGIC 0x63677270
#define ISOFS_SUPER_MAGIC   0x00009660
#define EXT4_SUPER_MAGIC    0x0000ef53
#define MSDOS_SUPER_MAGIC   0x00004d44
#define NTFS_SB_MAGIC       0x5346544e
#define SOCKFS_MAGIC        0x534f434b
#define PIPEFS_MAGIC        0x50495045

/* personality */

#define PER_LINUX 0x0000

/* getrusage */

typedef struct linux_rusage {
        uint64_t ru_utime_sec;
        uint64_t ru_utime_usec;
        uint64_t ru_stime_sec;
        uint64_t ru_stime_usec;
        int64_t  ru_maxrss;
        int64_t  ru_ixrss;
        int64_t  ru_idrss;
        int64_t  ru_isrss;
        int64_t  ru_minflt;
        int64_t  ru_majflt;
        int64_t  ru_nswap;
        int64_t  ru_inblock;
        int64_t  ru_oublock;
        int64_t  ru_msgsnd;
        int64_t  ru_msgrcv;
        int64_t  ru_nsignals;
        int64_t  ru_nvcsw;
        int64_t  ru_nivcsw;
} linux_rusage_t;

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

/* getrusage syscall: report resource usage */
static int64_t sys_getrusage_impl(uint64_t who, uint64_t usage, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
static int64_t sys_restart_syscall(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* chmod / chown helpers */
static int64_t sys_chmod_common(const char *path, uint64_t mode)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int        lookup_error = EOK;
    vfs_node_t node         = vfs_open_checked(path, &lookup_error);
    if (!node) return lookup_error;
    int result = vfs_chmod_process(node, (uint16_t)mode, proc);
    vfs_close(node);
    return result;
}

static int64_t sys_chmod_impl(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;
    return sys_chmod_common(name, mode);
}

static int64_t sys_fchmod_impl(uint64_t fd, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    int result = vfs_chmod_process(file->node, (uint16_t)mode, proc);
    process_file_put(file);
    return result;
}

/* fchmodat2 syscall: fchmodat with an explicit flags argument */
static int64_t sys_fchmodat2_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (flags & ~(uint64_t)AT_SYMLINK_NOFOLLOW) return -EINVAL;

    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)dirfd, path, name);
    if (ret != EOK) return ret;

    int        lookup_error = EOK;
    vfs_node_t node         = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow_checked(name, &lookup_error) : vfs_open_checked(name, &lookup_error);
    if (!node) return lookup_error;
    int result = vfs_chmod_process(node, (uint16_t)mode, proc);
    vfs_close(node);
    return result;
}

static int64_t sys_chown_common(const char *path, uint64_t owner, uint64_t group, bool nofollow)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    vfs_node_t node = nofollow ? vfs_open_nofollow(path) : vfs_open(path);
    if (!node) return -ENOENT;
    int result = vfs_chown_process(node, (uint32_t)owner, (uint32_t)group, proc);
    vfs_close(node);
    return result;
}

static int64_t sys_chown_impl(uint64_t path, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;
    return sys_chown_common(name, owner, group, false);
}

static int64_t sys_lchown_impl(uint64_t path, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;
    return sys_chown_common(name, owner, group, true);
}

static int64_t sys_fchown_impl(uint64_t fd, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    int result = vfs_chown_process(file->node, (uint32_t)owner, (uint32_t)group, proc);
    process_file_put(file);
    return result;
}

/*
 * Create the node described by mode at an already-resolved path. Shared by
 * mknod (AT_FDCWD) and mknodat so the type dispatch cannot drift.
 */
int64_t mknod_create_node(char *resolved, uint64_t mode, uint64_t dev)
{
    switch (mode & 0170000) {
        case 0010000 : // FIFO
            return pipe_mknod(resolved, (uint16_t)mode, dev);
        case 0100000 : // regular file
            return vfs_mkfile_mode(resolved, (uint16_t)mode);
        case 0020000 : // character device
        case 0060000 : // block device: created by drivers via devtmpfs
            return -ENOSYS;
        default :
            return -EINVAL;
    }
}

static int64_t sys_mknod_impl(uint64_t path, uint64_t mode, uint64_t dev, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;
    return mknod_create_node(name, mode, dev);
}

/* reboot syscall: reboot, halt, or power off */
static int64_t sys_reboot_impl(uint64_t magic, uint64_t magic2, uint64_t cmd, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (magic != 0xfee1dead || (magic2 != 0x28121969 && magic2 != 0x05121996 && magic2 != 0x16041998)) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;

    switch (cmd) {
        case 0x00000000 : // RB_DISABLE_CAD
        case 0x89ABCDEF : // RB_ENABLE_CAD
            return EOK;
        case 0x01234567 : // RB_AUTOBOOT
        case 0xA1B2C3D4 : // RB_RESTART2
            plogk("syscall: Reboot requested.\n");
            disable_intr();
            power_reset();
            for (uint32_t i = 0; i < 100000; i++) {
                if (!(inb(0x64) & 0x02)) {
                    outb(0x64, 0xFE);
                    break;
                }
            }
            outb(0xCF9, 0x06);
            break;
        case 0x4321FEDC : // RB_POWER_OFF
            plogk("syscall: Power-off requested.\n");
            disable_intr();
            power_off();
            break;
        case 0xCDEF0123 : // RB_HALT_SYSTEM
            plogk("syscall: Halt requested.\n");
            disable_intr();
            break;
        case 0x45584543 : // RB_KEXEC
        case 0xD000FCE2 : // RB_SW_SUSPEND
            return -ENOSYS;
        default :
            return -EINVAL;
    }

    for (;;) __asm__ volatile("hlt");
}

/* personality syscall: return the fixed personality */
static int64_t sys_personality_impl(uint64_t persona, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)persona;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return PER_LINUX;
}

/* Map a filesystem to its statfs magic number */
static int64_t linux_statfs_type(vfs_node_t node)
{
    const char *name = node ? vfs_filesystem_name(node->fsid) : NULL;
    if (!name) {
        if (node && (node->type & file_socket)) return SOCKFS_MAGIC;
        if (node && (node->type & file_pipe)) return PIPEFS_MAGIC;
        return 0;
    }
    if (streq(name, "tmpfs") || streq(name, "devtmpfs")) return TMPFS_MAGIC;
    if (streq(name, "sysfs")) return SYSFS_MAGIC;
    if (streq(name, "proc")) return PROC_SUPER_MAGIC;
    if (streq(name, "cgroup2")) return CGROUP2_SUPER_MAGIC;
    if (streq(name, "isofs")) return ISOFS_SUPER_MAGIC;
    if (streq(name, "extfs")) return EXT4_SUPER_MAGIC;
    if (streq(name, "fatfs")) return MSDOS_SUPER_MAGIC;
    if (streq(name, "ntfs")) return NTFS_SB_MAGIC;
    return SOCKFS_MAGIC;
}

/* Build a statfs structure and copy it to user space */
static int64_t copy_statfs_to_user(vfs_node_t node, uint64_t buf)
{
    linux_statfs_t sf;
    memset(&sf, 0, sizeof(sf));
    sf.f_type        = linux_statfs_type(node);
    sf.f_bsize       = 4096;
    sf.f_blocks      = 65536;
    sf.f_bfree       = 32768;
    sf.f_bavail      = 32768;
    sf.f_files       = 0;
    sf.f_ffree       = 0;
    vfs_node_t mount = vfs_containing_mount(node);
    sf.f_fsid        = mount && mount->mount_id ? mount->mount_id : node->fsid;
    sf.f_namelen     = 255;
    sf.f_frsize      = 4096;
    if (mount) sf.f_flags = (int64_t)(mount->flags & (MOUNT_FLAG_RDONLY | MOUNT_FLAG_NOSUID | MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC));

    if (copy_to_user((void *)buf, &sf, sizeof(sf))) return -EFAULT;
    return 0;
}

/* statfs syscall: report filesystem statistics */
static int64_t sys_statfs_impl(uint64_t path, uint64_t buf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!path || !buf) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char resolved[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, resolved);
    if (ret != EOK) return ret;
    vfs_node_t node = vfs_open(resolved);
    if (!node) return -ENOENT;
    ret = (int)copy_statfs_to_user(node, buf);
    vfs_close(node);
    return ret;
}

/* fstatfs syscall: report filesystem statistics by fd */
static int64_t sys_fstatfs_impl(uint64_t fd, uint64_t buf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!buf) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    int ret = (int)copy_statfs_to_user(file->node, buf);
    process_file_put(file);
    return ret;
}

/* sysinfo syscall: report system statistics */
static int64_t sys_sysinfo_impl(uint64_t info, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!info) return -EFAULT;

    linux_sysinfo_t si;
    memset(&si, 0, sizeof(si));
    si.uptime   = (int64_t)(timer_monotonic_ns() / TIMER_NSEC_PER_SEC);
    si.mem_unit = 1;

    spin_lock(&frame_allocator.lock);
    si.totalram = frame_allocator.origin_frames * PAGE_4K_SIZE;
    si.freeram  = frame_allocator.usable_frames * PAGE_4K_SIZE;
    spin_unlock(&frame_allocator.lock);

    size_t     process_cursor = 0;
    process_t *process;
    while ((process = process_iterate_get(&process_cursor)) != NULL) {
        si.procs++;
        process_put(process);
    }

    swap_stats_t swap;
    swap_get_stats(&swap);
    si.totalswap = swap.total_pages * SWAP_PAGE_SIZE;
    si.freeswap  = swap.free_pages * SWAP_PAGE_SIZE;

    if (copy_to_user((void *)info, &si, sizeof(si))) return -EFAULT;
    return 0;
}

static int64_t sys_clock_nanosleep_impl(uint64_t clockid, uint64_t flags, uint64_t req, uint64_t rem, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return clock_sleep(clockid, flags, req, rem);
}

typedef struct linux_rlimit64 {
        uint64_t rlim_cur;
        uint64_t rlim_max;
} linux_rlimit64_t;

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
        if (current > PROCESS_MAX_FD) current = PROCESS_MAX_FD;
        if (maximum > PROCESS_MAX_FD) maximum = PROCESS_MAX_FD;
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
static int64_t sys_getrlimit_impl(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
static int64_t sys_setrlimit_impl(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

static int64_t sys_fsync_impl(uint64_t fd, uint64_t data_only, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)data_only;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    int result = vfs_fsync(file->node, 0);
    process_file_put(file);
    return result;
}

static int64_t sys_fdatasync_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    int result = vfs_fsync(file->node, 1);
    process_file_put(file);
    return result;
}

static int64_t sys_syncfs_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    process_file_put(file);
    return vfs_sync_all();
}

/* prlimit64 syscall: read and/or set a resource limit */
static int64_t sys_prlimit64_impl(uint64_t pid, uint64_t resource, uint64_t new_rlim, uint64_t old_rlim, uint64_t arg4, uint64_t arg5)
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
        if (copy_from_user(&new_limit, (const void *)new_rlim, sizeof(new_limit)))
            result = -EFAULT;
        else
            result = process_rlimit_update(caller, target, resource, &new_limit);
    }
    if (pinned) process_put(target);
    return result;
}

/* readahead syscall: prefetch file data */
static int64_t sys_readahead(uint64_t fd, uint64_t offset, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if ((int64_t)offset < 0 || offset > UINT64_MAX - count) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    if ((file->flags & O_ACCMODE) == O_WRONLY) {
        process_file_put(file);
        return -EBADF;
    }
    int result = vfs_readahead(file->node, offset, (size_t)count);
    process_file_put(file);
    return result;
}

/* fadvise64 syscall: apply a file access hint */
static int64_t sys_fadvise64(uint64_t fd, uint64_t offset, uint64_t len, uint64_t advice, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    if (advice > 5 || offset > UINT64_MAX - len) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;
    int result = EOK;
    if (advice == 3)
        result = vfs_readahead(file->node, offset, (size_t)len);
    else if (advice == 4) {
        uint64_t end = len ? offset + len - 1 : UINT64_MAX;
        result       = vfs_drop_pages(file->node, offset, end, 1);
    }
    process_file_put(file);
    return result == -EOPNOTSUPP ? EOK : result;
}

static int64_t do_execve(const char *path, char *const argv[], char *const envp[], syscall_frame_t *frame);
static int64_t do_execve_resolved(const char *path, vfs_node_t initial_node, char *const argv[], char *const envp[], syscall_frame_t *frame);

#define AT_EXECVE_CHECK 0x10000

static int64_t do_execveat(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, syscall_frame_t *frame)
{
    if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_EXECVE_CHECK)) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char kpath[SYSCALL_PATH_MAX];
    char input[SYSCALL_PATH_MAX];
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
        /*
         * AT_EXECVE_CHECK: just check if the file is executable,
         * don't actually exec.
         */
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

static int64_t sys_execveat_stub(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    return do_execveat(dirfd, path, argv, envp, flags, NULL);
}

static int64_t sys_membarrier_stub(uint64_t cmd, uint64_t flags, uint64_t cpu_id, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)cpu_id;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    /* MEMBARRIER_CMD_QUERY = 0, MEMBARRIER_CMD_GLOBAL = 1. */
    if (flags) return -EINVAL;
    switch (cmd) {
        case 0 :
            return 1; /* MEMBARRIER_CMD_GLOBAL */
        case 1 :
            /* The synchronous shootdown provides a cross-CPU rendezvous. */
            flush_tlb_all();
            return 0;
        default :
            return -EINVAL;
    }
}

static int64_t sys_copy_file_range_stub(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out, uint64_t len, uint64_t flags)
{
    if (flags) return -EINVAL;
    if (fd_in == fd_out) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf_in  = process_fd_get(proc, (int)fd_in);
    process_file_t *pf_out = process_fd_get(proc, (int)fd_out);
    if (!pf_in || !pf_out) {
        if (pf_in) process_file_put(pf_in);
        if (pf_out) process_file_put(pf_out);
        return -EBADF;
    }

    /* Save and restore offsets if off_in/off_out are NULL */
    bool    have_off_in  = (off_in != 0);
    bool    have_off_out = (off_out != 0);
    int64_t saved_off_in = 0, saved_off_out = 0;

    if (have_off_in) {
        if (copy_from_user(&saved_off_in, (const void *)off_in, sizeof(saved_off_in))) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return -EFAULT;
        }
    } else {
        saved_off_in = (int64_t)pf_in->offset;
    }

    if (have_off_out) {
        if (copy_from_user(&saved_off_out, (const void *)off_out, sizeof(saved_off_out))) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return -EFAULT;
        }
    } else {
        saved_off_out = (int64_t)pf_out->offset;
    }

    uint8_t buf[SYSCALL_IO_CHUNK];
    size_t  total_copied = 0;

    while (total_copied < len) {
        size_t chunk = len - total_copied;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        int64_t nread = process_fd_read(proc, (int)fd_in, buf, chunk);
        if (nread < 0) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return total_copied ? (int64_t)total_copied : nread;
        }
        if (nread == 0) break;

        int64_t nwritten = process_fd_write(proc, (int)fd_out, buf, (size_t)nread);
        if (nwritten < 0) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return total_copied ? (int64_t)total_copied : nwritten;
        }

        total_copied += (size_t)nwritten;
        if ((size_t)nread < chunk) break;
    }

    /* Restore offsets if caller didn't specify them */
    if (!have_off_in) process_fd_seek(proc, (int)fd_in, saved_off_in, SEEK_SET);
    if (!have_off_out) process_fd_seek(proc, (int)fd_out, saved_off_out, SEEK_SET);

    process_file_put(pf_in);
    process_file_put(pf_out);
    return (int64_t)total_copied;
}

static int64_t sys_mlock2_stub(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mlock(addr, length);
}

static int64_t sys_pkey_mprotect_stub(uint64_t addr, uint64_t len, uint64_t prot, uint64_t pkey, uint64_t arg4, uint64_t arg5)
{
    (void)pkey;
    (void)arg4;
    (void)arg5;
    return sys_mprotect(addr, len, prot);
}

/* rseq (restartable sequences) */

typedef struct rseq_layout {
        uint32_t cpu_id_start;
        uint32_t cpu_id;
        uint64_t rseq_cs;
        uint32_t flags;
        uint32_t node_id;
        uint32_t mm_cid;
        uint8_t  padding[36];
} __attribute__((packed)) rseq_layout_t;

_Static_assert(sizeof(rseq_layout_t) == 64, "rseq ABI size");

/* rseq syscall: register a restartable sequence area */
static int64_t sys_rseq_impl(uint64_t rseq_base, uint64_t rseq_len, uint64_t flags, uint64_t sig, uint64_t arg4, uint64_t arg5)
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

/* pidfd_open */

static int pidfd_fsid = -1;

/* VFS open callback (no-op) */
static void pidfd_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

/* VFS close callback: drop the process reference */
static void pidfd_vfs_close(void *current)
{
    process_t *target = (process_t *)current;
    if (target) process_put(target);
}

/* Unsupported read callback */
static size_t pidfd_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/* Unsupported write callback */
static size_t pidfd_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/* Unsupported stat callback */
static int pidfd_stub_stat(void *f, vfs_node_t n)
{
    (void)f;
    (void)n;
    return EOK;
}

/* Unsupported mkdir/mkfile/link/symlink callback */
static int pidfd_stub_mk(void *p, const char *nm, vfs_node_t n)
{
    (void)p;
    (void)nm;
    (void)n;
    return -ENOSYS;
}

/* Unsupported readlink callback */
static size_t pidfd_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}

/* Unsupported ioctl callback */
static int pidfd_stub_ioctl(void *f, size_t o, void *a)
{
    (void)f;
    (void)o;
    (void)a;
    return -ENOSYS;
}

/* Unsupported dup callback */
static vfs_node_t pidfd_stub_dup(vfs_node_t n)
{
    (void)n;
    return NULL;
}

/* Unsupported delete callback */
static int pidfd_stub_del(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -ENOSYS;
}

/* Unsupported rename callback */
static int pidfd_stub_rename(const vfs_rename_context_t *context)
{
    (void)context;
    return -ENOSYS;
}

/* Unsupported mount callback */
static int pidfd_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

/* Unsupported unmount callback */
static void pidfd_stub_unmount(void *root)
{
    (void)root;
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
    cb->mount    = pidfd_stub_mount;
    cb->unmount  = pidfd_stub_unmount;
    cb->open     = pidfd_vfs_open;
    cb->close    = pidfd_vfs_close;
    cb->read     = pidfd_vfs_read;
    cb->write    = pidfd_vfs_write;
    cb->readlink = pidfd_stub_readlink;
    cb->mkdir    = pidfd_stub_mk;
    cb->mkfile   = pidfd_stub_mk;
    cb->link     = pidfd_stub_mk;
    cb->symlink  = pidfd_stub_mk;
    cb->stat     = pidfd_stub_stat;
    cb->ioctl    = pidfd_stub_ioctl;
    cb->dup      = pidfd_stub_dup;
    cb->delete   = pidfd_stub_del;
    cb->rename   = pidfd_stub_rename;
    pidfd_fsid   = vfs_regist(cb);
    free(cb);
    if (pidfd_fsid < 0)
        plogk("pidfd: Failed to register VFS callbacks (%d)\n", pidfd_fsid);
    else
        plogk("pidfd: Filesystem registered (fsid=%d)\n", pidfd_fsid);
}

/* pidfd_open syscall: open a process descriptor */
static int64_t sys_pidfd_open_impl(uint64_t pid_raw, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (flags & ~(uint64_t)PIDFD_NONBLOCK) return -EINVAL;

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
    node->mode   = O_RDONLY;

    uint64_t fd_flags = O_RDONLY;
    if (flags & PIDFD_NONBLOCK) fd_flags |= O_NONBLOCK;

    int fd = process_fd_install(proc, node, fd_flags);
    if (fd < 0) {
        vfs_close(node);
        return fd;
    }
    return fd;
}

/* clone3 */

typedef struct clone3_args {
        uint64_t flags;
        uint64_t pidfd;
        uint64_t child_tid;
        uint64_t parent_tid;
        uint64_t exit_signal;
        uint64_t stack;
        uint64_t stack_size;
        uint64_t tls;
        uint64_t set_tid;
        uint64_t set_tid_size;
        uint64_t cgroup;
} clone3_args_t;

/* clone3 syscall: create a child process or thread */
static int64_t sys_clone3_impl(syscall_frame_t *frame, uint64_t cl_args, uint64_t size, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
    bool     is_thread        = (flags & CLONE_THREAD) != 0;
    bool     is_vfork         = (flags & CLONE_VFORK) != 0;

    if (args.pidfd || args.set_tid || args.set_tid_size || args.cgroup || (args.stack && !args.stack_size) || (!args.stack && args.stack_size)) return -EINVAL;
    if (args.stack && UINT64_MAX - args.stack < args.stack_size) return -EINVAL;

    if (is_thread) {
        if ((flags & CLONE_PTHREAD_REQUIRED) != CLONE_PTHREAD_REQUIRED || (flags & ~CLONE_PTHREAD_ALLOWED) || exit_signal || !args.stack || !args.stack_size) return -EINVAL;
        if (((flags & CLONE_PARENT_SETTID) && !args.parent_tid) || ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !args.child_tid)) return -EFAULT;
        process_t *proc = process_current();
        if (!proc) return -ESRCH;

        uintptr_t child_stack = (uintptr_t)(args.stack + args.stack_size);
        int     error = EOK;
        task_t *child = process_clone_thread(frame, child_stack, (flags & CLONE_PARENT_SETTID) ? args.parent_tid : 0, (flags & CLONE_CHILD_SETTID) ? args.child_tid : 0,
                                             (flags & CLONE_CHILD_CLEARTID) ? args.child_tid : 0, (flags & CLONE_SETTLS) ? args.tls : current_task()->thread.fs_base, &error);
        if (!child) return error;
        ptrace_fork_event(frame, PTRACE_EVENT_CLONE, child->pid);
        return (int64_t)child->pid;
    }

    /* Fork / vfork */
    uint64_t supported_fork = CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED;
    if (is_vfork) supported_fork |= CLONE_VM | CLONE_VFORK;
    if ((flags & ~supported_fork) || (is_vfork && !(flags & CLONE_VM)) || (exit_signal && exit_signal != SIGCHLD)) return -EINVAL;
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
        uint64_t  kstack_top = (uint64_t)(child->kernel_stack + PROCESS_KERNEL_STACK);
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
    return (int64_t)child->task->pid;
}

/* process_madvise syscall: apply advice to a process */
static int64_t sys_process_madvise_impl(uint64_t pidfd, uint64_t iovec, uint64_t vlen, uint64_t advice, uint64_t flags, uint64_t arg5)
{
    (void)flags;
    (void)arg5;

    if (flags) return -EINVAL;

    process_t *target = NULL;
    /* Treat pidfd as a raw pid. */
    target = process_find_get((pid_t)pidfd);
    if (!target) return -ESRCH;

    /* Read iovec entries */
    struct {
            void  *iov_base;
            size_t iov_len;
    } iov[16];

    if (vlen > 16) vlen = 16;
    if (copy_from_user(iov, (const void *)iovec, vlen * sizeof(iov[0]))) {
        process_put(target);
        return -EFAULT;
    }

    int result = 0;
    for (uint64_t i = 0; i < vlen; i++) {
        /* Validate advice per range. */
        switch (advice) {
            case 1 : // MADV_COLD
            case 2 : // MADV_PAGEOUT
                break;
            default :
                process_put(target);
                return -EINVAL;
        }
        result++;
    }

    process_put(target);
    return result;
}

/* epoll_pwait2 syscall: wait with a nanosecond timeout */
static int64_t sys_epoll_pwait2_impl(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t tsp, uint64_t sigmask, uint64_t sigsetsize)
{
    int timeout_ms = -1; // infinite

    if (tsp) {
        linux_timespec64_t ts;
        if (copy_from_user(&ts, (const void *)tsp, sizeof(ts))) return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || (uint64_t)ts.tv_nsec >= 1000000000ULL) return -EINVAL;

        /* Convert to milliseconds, rounding up */
        int64_t ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
        if (ts.tv_nsec % 1000000) ms++;
        if (ms > (int64_t)INT32_MAX) ms = INT32_MAX;
        timeout_ms = (int)ms;
    }

    return sys_epoll_pwait((int)epfd, (epoll_event_t *)events, (int)maxevents, timeout_ms, (const void *)sigmask, (size_t)sigsetsize);
}

/* mmap family wrappers (6-arg syscall -> actual function) */
static int64_t sys_mprotect_wrap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mprotect(addr, length, prot);
}

static int64_t sys_mremap_wrap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr, uint64_t arg5)
{
    (void)arg5;
    return sys_mremap(old_addr, old_len, new_len, flags, new_addr);
}

static int64_t sys_msync_wrap(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_msync(addr, length, flags);
}

static int64_t sys_mincore_wrap(uint64_t addr, uint64_t length, uint64_t vec, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mincore(addr, length, vec);
}

static int64_t sys_madvise_wrap(uint64_t addr, uint64_t length, uint64_t advice, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_madvise(addr, length, advice);
}

static int64_t sys_mlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mlock(addr, length);
}

static int64_t sys_munlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_munlock(addr, length);
}

static int64_t sys_mlockall_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mlockall(flags);
}

static int64_t sys_munlockall_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_munlockall();
}

/* Signal syscall wrappers */
static int64_t sys_rt_sigaction_wrap(uint64_t sig, uint64_t act, uint64_t oact, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_sigaction((int)sig, (const sigaction_t *)act, (sigaction_t *)oact, (size_t)sigsetsize);
}

static int64_t sys_rt_sigprocmask_wrap(uint64_t how, uint64_t set, uint64_t oset, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_sigprocmask((int)how, (const sigset_t *)set, (sigset_t *)oset, (size_t)sigsetsize);
}

static int64_t sys_rt_sigreturn_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigreturn();
}

static int64_t sys_rt_sigpending_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigpending((sigset_t *)set, (size_t)sigsetsize);
}

static int64_t sys_rt_sigtimedwait_wrap(uint64_t set, uint64_t info, uint64_t timeout, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_sigtimedwait((const sigset_t *)set, (siginfo_t *)info, (const void *)timeout, (size_t)sigsetsize);
}

static int64_t sys_rt_sigqueueinfo_wrap(uint64_t pid, uint64_t sig, uint64_t info, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigqueueinfo((pid_t)pid, (int)sig, (siginfo_t *)info);
}

static int64_t sys_rt_sigsuspend_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_rt_sigsuspend((const sigset_t *)set, (size_t)sigsetsize);
}

static int64_t sys_sigaltstack_wrap(uint64_t ss, uint64_t oss, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_sigaltstack((const stack_t *)ss, (stack_t *)oss);
}

static int64_t sys_pause_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_pause();
}

static int64_t sys_tgkill_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_tgkill((pid_t)tgid, (pid_t)tid, (int)sig);
}

static int64_t sys_rt_tgsigqueueinfo_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t info, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_rt_tgsigqueueinfo((pid_t)tgid, (pid_t)tid, (int)sig, (siginfo_t *)info);
}

static int64_t sys_setpgid_wrap(uint64_t pid, uint64_t pgid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_setpgid((pid_t)pid, (pid_t)pgid);
}

static int64_t sys_getpgrp_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getpgrp();
}

static int64_t sys_setsid_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_setsid();
}

static int64_t sys_getsid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getsid((pid_t)pid);
}

static int64_t sys_getpgid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getpgid((pid_t)pid);
}

/* Socket wrappers */
static int64_t sys_socket_wrap(uint64_t family, uint64_t type, uint64_t protocol, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_socket((uint32_t)family, (uint32_t)type, (uint32_t)protocol);
}

static int64_t sys_bind_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_bind((int)fd, (const sockaddr_t *)addr, (uint32_t)addrlen);
}

static int64_t sys_listen_wrap(uint64_t fd, uint64_t backlog, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_listen((int)fd, (int)backlog);
}

static int64_t sys_accept_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_accept((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen, 0);
}

static int64_t sys_accept4_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_accept((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen, (int)flags);
}

static int64_t sys_connect_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_connect((int)fd, (const sockaddr_t *)addr, (uint32_t)addrlen);
}

static int64_t sys_sendto_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    return sys_sendto((int)fd, (const void *)buf, (size_t)len, (int)flags, (const sockaddr_t *)addr, (uint32_t)addrlen);
}

static int64_t sys_recvfrom_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    return sys_recvfrom((int)fd, (void *)buf, (size_t)len, (int)flags, (sockaddr_t *)addr, (uint32_t *)addrlen);
}

static int64_t sys_sendmsg_wrap(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_sendmsg((int)fd, (const msghdr_t *)msg, (int)flags);
}

static int64_t sys_recvmsg_wrap(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_recvmsg((int)fd, (msghdr_t *)msg, (int)flags);
}

static int64_t sys_shutdown_wrap(uint64_t fd, uint64_t how, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shutdown((int)fd, (int)how);
}

static int64_t sys_socketpair_wrap(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_socketpair((int)domain, (int)type, (int)protocol, (int *)sv);
}

static int64_t sys_getsockname_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getsockname((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen);
}

static int64_t sys_getpeername_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getpeername((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen);
}

static int64_t sys_setsockopt_wrap(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen, uint64_t arg5)
{
    (void)arg5;
    return sys_setsockopt((int)fd, (int)level, (int)optname, (const void *)optval, (uint32_t)optlen);
}

static int64_t sys_getsockopt_wrap(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen, uint64_t arg5)
{
    (void)arg5;
    return sys_getsockopt((int)fd, (int)level, (int)optname, (void *)optval, (uint32_t *)optlen);
}

static int64_t sys_sendmmsg_wrap(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_sendmmsg((int)fd, (void *)msgvec, (uint32_t)vlen, (int)flags);
}

static int64_t sys_recvmmsg_wrap(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags, uint64_t timeout, uint64_t arg5)
{
    (void)arg5;
    return sys_recvmmsg((int)fd, (void *)msgvec, (uint32_t)vlen, (int)flags, (void *)timeout);
}

/* Pipe wrappers */
static int64_t sys_pipe_wrap(uint64_t pipefd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_pipe((int *)pipefd);
}

static int64_t sys_pipe2_wrap(uint64_t pipefd, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_pipe2((int *)pipefd, (int)flags);
}

/* System V IPC wrappers */
static int64_t sys_semget_wrap(uint64_t key, uint64_t nsems, uint64_t semflg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_semget((key_t)key, (int)nsems, (int)semflg);
}

static int64_t sys_semop_wrap(uint64_t semid, uint64_t sops, uint64_t nsops, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_semop((int)semid, (sembuf_t *)sops, (size_t)nsops);
}

static int64_t sys_semtimedop_wrap(uint64_t semid, uint64_t sops, uint64_t nsops, uint64_t timeout, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_semtimedop((int)semid, (sembuf_t *)sops, (size_t)nsops, (const void *)timeout);
}

static int64_t sys_semctl_wrap(uint64_t semid, uint64_t semnum, uint64_t cmd, uint64_t arg, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_semctl((int)semid, (int)semnum, (int)cmd, arg);
}

static int64_t sys_shmget_wrap(uint64_t key, uint64_t size, uint64_t shmflg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmget((key_t)key, (size_t)size, (int)shmflg);
}

static int64_t sys_shmat_wrap(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmat((int)shmid, (const void *)shmaddr, (int)shmflg);
}

static int64_t sys_shmdt_wrap(uint64_t shmaddr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmdt((const void *)shmaddr);
}

static int64_t sys_shmctl_wrap(uint64_t shmid, uint64_t cmd, uint64_t buf, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmctl((int)shmid, (int)cmd, (void *)buf);
}

static int64_t sys_msgget_wrap(uint64_t key, uint64_t msgflg, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_msgget((key_t)key, (int)msgflg);
}

static int64_t sys_msgsnd_wrap(uint64_t msqid, uint64_t msgp, uint64_t msgsz, uint64_t msgflg, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_msgsnd((int)msqid, (const void *)msgp, (size_t)msgsz, (int)msgflg);
}

static int64_t sys_msgrcv_wrap(uint64_t msqid, uint64_t msgp, uint64_t msgsz, uint64_t msgtyp, uint64_t msgflg, uint64_t arg5)
{
    (void)arg5;
    return sys_msgrcv((int)msqid, (void *)msgp, (size_t)msgsz, (int64_t)msgtyp, (int)msgflg);
}

static int64_t sys_msgctl_wrap(uint64_t msqid, uint64_t cmd, uint64_t buf, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_msgctl((int)msqid, (int)cmd, (void *)buf);
}

/* POSIX MQ wrappers */
static int64_t sys_mq_open_wrap(uint64_t name, uint64_t oflag, uint64_t mode, uint64_t attr, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_mq_open((const char *)name, (int)oflag, (uint32_t)mode, (mq_attr_t *)attr);
}

static int64_t sys_mq_unlink_wrap(uint64_t name, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mq_unlink((const char *)name);
}

static int64_t sys_mq_timedsend_wrap(uint64_t mqdes, uint64_t msg_ptr, uint64_t msg_len, uint64_t msg_prio, uint64_t abs_timeout, uint64_t arg5)
{
    (void)arg5;
    return sys_mq_timedsend((int)mqdes, (const char *)msg_ptr, (size_t)msg_len, (uint32_t)msg_prio, (const void *)abs_timeout);
}

static int64_t sys_mq_timedreceive_wrap(uint64_t mqdes, uint64_t msg_ptr, uint64_t msg_len, uint64_t msg_prio, uint64_t abs_timeout, uint64_t arg5)
{
    (void)arg5;
    return sys_mq_timedreceive((int)mqdes, (char *)msg_ptr, (size_t)msg_len, (uint32_t *)msg_prio, (const void *)abs_timeout);
}

static int64_t sys_mq_notify_wrap(uint64_t mqdes, uint64_t notification, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mq_notify((int)mqdes, (const sigevent_t *)notification);
}

static int64_t sys_mq_getsetattr_wrap(uint64_t mqdes, uint64_t newattr, uint64_t oldattr, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mq_getsetattr((int)mqdes, (const mq_attr_t *)newattr, (mq_attr_t *)oldattr);
}

/* Futex wrapper */
static int64_t sys_futex_wrap(uint64_t uaddr, uint64_t futex_op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint64_t val3)
{
    return sys_futex((uint32_t *)uaddr, (int)futex_op, (uint32_t)val, timeout, (uint32_t *)uaddr2, (uint32_t)val3);
}

/* Epoll wrappers */
static int64_t sys_epoll_create_wrap(uint64_t size, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_epoll_create((int)size);
}

static int64_t sys_epoll_create1_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_epoll_create1((int)flags);
}

static int64_t sys_epoll_ctl_wrap(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_epoll_ctl((int)epfd, (int)op, (int)fd, (epoll_event_t *)event);
}

static int64_t sys_epoll_wait_wrap(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_epoll_wait((int)epfd, (epoll_event_t *)events, (int)maxevents, (int)timeout);
}

static int64_t sys_epoll_pwait_wrap(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize)
{
    return sys_epoll_pwait((int)epfd, (epoll_event_t *)events, (int)maxevents, (int)timeout, (const void *)sigmask, (size_t)sigsetsize);
}

/* waitid wrapper */

#define P_PID    1
#define P_PGID   2
#define P_ALL    3
#define WEXITED  0x00000004
#define WSTOPPED 0x00000002
#define WNOWAIT  0x01000000

/* waitid syscall: wait for a child by selector */
static int64_t sys_waitid_impl(uint64_t which, uint64_t upid, uint64_t infop, uint64_t options, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    pid_t selector;
    int   flags = (int)options;

    if (!(flags & (WEXITED | WSTOPPED | WCONTINUED)) || (flags & ~(WNOHANG | WEXITED | WSTOPPED | WCONTINUED | WNOWAIT))) return -EINVAL;
    /* Non-destructive wait snapshots are not supported. */
    if (flags & WNOWAIT) return -EINVAL;

    switch ((int)which) {
        case P_PID :
            selector = (pid_t)upid;
            if (selector <= 0) return -EINVAL;
            break;
        case P_PGID : {
            if (upid > INT64_MAX) return -EINVAL;
            selector = upid ? -(pid_t)upid : 0;
            break;
        }
        case P_ALL :
            selector = -1;
            break;
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

    char   kpath[SYSCALL_PATH_MAX];
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
    char **kargv          = copy_argv_from_user((const char *const *)argv, PROCESS_MAX_ARGV, &argc, &argument_bytes, &copy_error);
    if (argc < 0) {
        if (initial_node) vfs_close(initial_node);
        free_string_array(kargv);
        return copy_error;
    }
    char **kenvp = copy_argv_from_user((const char *const *)envp, PROCESS_MAX_ENVP, &envc, &argument_bytes, &copy_error);
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
            plogk("syscall: Exec of %s node=%p handle=%p rejected with image size %llu\n", kpath, node, node->handle, (unsigned long long)node->size);
            vfs_close(node);
            free_string_array(kargv);
            free_string_array(kenvp);
            return -ENOEXEC;
        }

        size_t header_size = node->size < 256 ? (size_t)node->size : 256;
        elf_data           = malloc(header_size);
        if (!elf_data) {
            plogk("syscall: Exec of %s failed (header allocation, %llu bytes)\n", kpath, (unsigned long long)header_size);
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
                plogk("syscall: Exec of %s failed (shebang argv allocation)\n", kpath);
                vfs_close(node);
                free(elf_data);
                free_string_array(kargv);
                free_string_array(kenvp);
                return -ENOMEM;
            }

            int out         = 0;
            new_argv[out++] = strdup(interpreter);
            if (optional < end) {
                size_t optional_len = end - optional;
                new_argv[out]       = malloc(optional_len + 1);
                if (new_argv[out]) {
                    memcpy(new_argv[out], elf_data + optional, optional_len);
                    new_argv[out][optional_len] = '\0';
                }
                out++;
            }
            new_argv[out++] = strdup(kpath);
            for (int i = 1; i < argc; i++) new_argv[out++] = strdup(kargv[i]);
            bool allocation_failed = false;
            for (int i = 0; i < new_argc; i++)
                if (!new_argv[i]) allocation_failed = true;
            if (allocation_failed) {
                plogk("syscall: Exec of %s failed (shebang argv element allocation)\n", kpath);
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
            plogk("syscall: Exec of %s read only %llu bytes (expected %llu)\n", kpath, (unsigned long long)total, (unsigned long long)expected_size);
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
    proc->start_brk = PROCESS_HEAP_START;
    proc->heap_brk  = PROCESS_HEAP_START;
    proc->stack_brk = PROCESS_STACK_BASE - (long)PROCESS_STACK_SIZE;

    if (setup_process_page_dir(proc)) {
        plogk("syscall: Exec of %s failed (page directory setup)\n", kpath);
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
     * Load ELF into the new page directory BEFORE destroying the old one.
     * This way, if loading fails, we can restore the old address space.
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
        plogk("syscall: Exec of %s rejected by ELF loader (errno %d, %llu bytes, magic=%u)\n", kpath, ret, (unsigned long long)image_size, image_magic);

        /*
         * Loading failed. Destroy the new (incomplete) page directory
         * and its VMAs, then restore the old address space.
         */
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
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        spin_lock(&proc->fd_lock);
        if (proc->fds[i] && (proc->fd_flags[i] & FD_CLOEXEC)) {
            spin_unlock(&proc->fd_lock);
            process_fd_close(proc, i);
            continue;
        }
        spin_unlock(&proc->fd_lock);
    }

    proc->task->clear_child_tid = 0;
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
        frame->rflags = 0x202;
        frame->rsp    = rsp;
        frame->ss     = 0x23;
    }
    return 0;
}

/* Resolve a path and execute the program */
static int64_t do_execve(const char *path, char *const argv[], char *const envp[], syscall_frame_t *frame)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char kpath[SYSCALL_PATH_MAX];
    int  path_ret = copy_resolved_path_at(proc, AT_FDCWD, (uint64_t)path, kpath);
    if (path_ret != EOK) return path_ret;
    return do_execve_resolved(kpath, NULL, argv, envp, frame);
}

/* execve syscall: replace the process image */
static int64_t sys_execve_wrap(uint64_t path, uint64_t argv, uint64_t envp, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return do_execve((const char *)path, (char *const *)argv, (char *const *)envp, NULL);
}

/* getdents64 */

typedef struct linux_dirent64 {
        uint64_t       d_ino;
        int64_t        d_off;
        unsigned short d_reclen;
        unsigned char  d_type;
        char           d_name[];
} linux_dirent64_t;

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

/* Map an internal VFS node type to a getdents64 d_type */
static unsigned char vfs_node_to_dtype(uint16_t type)
{
    if (type & file_dir) return DT_DIR;
    if (type & file_symlink) return DT_LNK;
    if (type & file_block) return DT_BLK;
    if (type & (file_stream | file_fbdev | file_keyboard | file_mouse | file_audio | file_ptmx | file_pts)) return DT_CHR;
    if (type & file_pipe) return DT_FIFO;
    if (type & file_socket) return DT_SOCK;
    return DT_REG;
}

static int64_t sys_getdents64_impl(int fd, uint64_t dirent, uint64_t count);

static int64_t sys_getdents64_wrap(uint64_t fd, uint64_t dirent, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    int64_t ret = sys_getdents64_impl(fd, dirent, count);
    return ret;
}

/* getdents64 syscall: read directory entries */
static int64_t sys_getdents64_impl(int fd, uint64_t dirent, uint64_t count)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    vfs_node_t node = file->node;
    if (!node || !(node->type & file_dir)) {
        process_file_put(file);
        return -ENOTDIR;
    }

    if (count > 65536) {
        process_file_put(file);
        return -EINVAL;
    }

    uint8_t *kbuf = malloc(count);
    if (!kbuf) {
        plogk("syscall: getdents64 buffer allocation failed (%llu bytes)\n", (unsigned long long)count);
        process_file_put(file);
        return -ENOMEM;
    }

    uint64_t written = 0;
    size_t   index   = file->offset;

    for (;;) {
        vfs_dirent_t entry;
        if (vfs_readdir(node, index, &entry) != EOK) break;

        size_t         name_len = strlen(entry.name);
        unsigned short reclen   = (unsigned short)(sizeof(linux_dirent64_t) + name_len + 1);
        reclen                  = (unsigned short)ALIGN_UP(reclen, 8);

        if (written + reclen > count) break;

        linux_dirent64_t *de = (linux_dirent64_t *)(kbuf + written);
        de->d_ino            = entry.inode;
        de->d_off            = (int64_t)(index + 1);
        de->d_reclen         = reclen;
        de->d_type           = vfs_node_to_dtype((uint16_t)entry.type);
        memcpy(de->d_name, entry.name, name_len);
        de->d_name[name_len] = '\0';

        written += reclen;
        index++;
    }

    file->offset = index;

    if (written > 0) {
        if (copy_to_user((void *)dirent, kbuf, written)) {
            free(kbuf);
            process_file_put(file);
            return -EFAULT;
        }
    }

    free(kbuf);
    process_file_put(file);
    return (int64_t)written;
}

/* writev syscall: write a vector of buffers */
static int64_t sys_writev_wrap(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if ((int)fd < 0 || iovcnt > 1024) return -EINVAL;

    iovec_t  kiov[16];
    iovec_t *vec   = kiov;
    int      alloc = 0;

    if (iovcnt > 16) {
        vec = malloc(iovcnt * sizeof(iovec_t));
        if (!vec) {
            plogk("syscall: writev iovec allocation failed (%llu entries)\n", (unsigned long long)iovcnt);
            return -ENOMEM;
        }
        alloc = 1;
    }

    if (copy_from_user(vec, (const void *)iov, iovcnt * sizeof(iovec_t))) {
        if (alloc) free(vec);
        return -EFAULT;
    }

    size_t requested = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len > 0x7ffff000UL - requested) {
            if (alloc) free(vec);
            return -EINVAL;
        }
        requested += vec[i].iov_len;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len == 0) continue;
        int64_t n = process_fd_write_user(proc, (int)fd, vec[i].iov_base, vec[i].iov_len);
        if (n < 0) {
            if (total == 0) total = n;
            goto writev_done;
        }
        total += n;
        if ((size_t)n < vec[i].iov_len) break;
    }
writev_done:
    if (alloc) free(vec);
    return total;
}

/* readv syscall: read into a vector of buffers */
static int64_t sys_readv_wrap(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if ((int)fd < 0 || iovcnt > 1024) return -EINVAL;

    iovec_t  kiov[16];
    iovec_t *vec   = kiov;
    int      alloc = 0;

    if (iovcnt > 16) {
        vec = malloc(iovcnt * sizeof(iovec_t));
        if (!vec) {
            plogk("syscall: readv iovec allocation failed (%llu entries)\n", (unsigned long long)iovcnt);
            return -ENOMEM;
        }
        alloc = 1;
    }

    if (copy_from_user(vec, (const void *)iov, iovcnt * sizeof(iovec_t))) {
        if (alloc) free(vec);
        return -EFAULT;
    }

    size_t requested = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len > 0x7ffff000UL - requested) {
            if (alloc) free(vec);
            return -EINVAL;
        }
        requested += vec[i].iov_len;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len == 0) continue;
        int64_t n = process_fd_read_user(proc, (int)fd, vec[i].iov_base, vec[i].iov_len);
        if (n < 0) {
            if (total == 0) total = n;
            goto readv_done;
        }
        total += n;
        if (!n || (size_t)n < vec[i].iov_len) break;
    }
readv_done:
    if (alloc) free(vec);
    return total;
}

/* chroot syscall: change the root directory */
static int64_t sys_chroot_wrap(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;

    char kpath[SYSCALL_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, kpath);
    if (ret != EOK) return ret;

    vfs_node_t node = vfs_open(kpath);
    if (!node) return -ENOENT;
    if (!(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }
    vfs_close(node);

    strncpy(proc->root, kpath, sizeof(proc->root) - 1);
    proc->root[sizeof(proc->root) - 1] = '\0';
    strncpy(proc->cwd, kpath, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    return 0;
}

/* fcntl wrapper */
static int64_t sys_fcntl_wrap(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_fcntl((int)fd, (int)cmd, arg);
}

/* prctl implementation */

#define PR_SET_PDEATHSIG    1
#define PR_GET_PDEATHSIG    2
#define PR_GET_DUMPABLE     3
#define PR_SET_DUMPABLE     4
#define PR_GET_KEEPCAPS     7
#define PR_SET_KEEPCAPS     8
#define PR_SET_NAME         15
#define PR_GET_NAME         16
#define PR_GET_SECCOMP 21
#define PR_SET_SECCOMP      22
#define PR_SET_TIMERSLACK   29
#define PR_GET_TIMERSLACK   30
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

/* prctl syscall: process control operations */
static int64_t sys_prctl_impl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg6;

    switch ((int)option) {
        case PR_SET_PDEATHSIG : {
            /* arg2 is the signal to send on parent death */
            if ((int)arg2 > 64 && (int)arg2 != 0) return -EINVAL;
            return 0;
        }
        case PR_GET_PDEATHSIG : {
            /* Return 0 (no parent death signal) */
            if (arg2 && copy_to_user((void *)arg2, &(int) {0}, sizeof(int))) return -EFAULT;
            return 0;
        }
        case PR_GET_DUMPABLE : {
            /* Return dumpable=1 */
            if (arg2 && copy_to_user((void *)arg2, &(int) {1}, sizeof(int))) return -EFAULT;
            return 0;
        }
        case PR_SET_DUMPABLE : {
            /* Accept any value */
            return 0;
        }
        case PR_GET_KEEPCAPS :
        case PR_SET_KEEPCAPS :
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
        case PR_SET_SECCOMP : return seccomp_prctl_set(arg2, arg3);
        case PR_GET_SECCOMP : return (arg2 || arg3 || arg4 || arg5) ? -EINVAL : seccomp_prctl_get();
        case PR_SET_TIMERSLACK :
        case PR_GET_TIMERSLACK : {
            /* Return default timer slack = 50000 ns */
            if (option == PR_GET_TIMERSLACK && arg2) {
                uint64_t slack = 50000;
                if (copy_to_user((void *)arg2, &slack, sizeof(slack))) return -EFAULT;
            }
            return 0;
        }
        case PR_SET_NO_NEW_PRIVS : return seccomp_set_no_new_privs(arg2, arg3, arg4, arg5);
        case PR_GET_NO_NEW_PRIVS : return seccomp_get_no_new_privs(arg2, arg3, arg4, arg5);
        default :
            return -EINVAL;
    }
}

#ifndef CONFIG_MODULE_MAX_SIZE
#    define CONFIG_MODULE_MAX_SIZE 64
#endif
#define SYSCALL_MODULE_MAX_SIZE ((size_t)CONFIG_MODULE_MAX_SIZE * 1024U * 1024U)

/* Copy module parameter string from user space */
static int copy_module_params(uint64_t user_params, char params[MODULE_PARAM_MAX])
{
    if (!user_params) {
        params[0] = 0;
        return EOK;
    }
    int ret = strncpy_from_user(params, (const char *)user_params, MODULE_PARAM_MAX);
    return ret < 0 ? ret : EOK;
}

/* swapon syscall: enable a swap area */
static int64_t sys_swapon(uint64_t path, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;
    if (!path) return -EFAULT;
    char name[SYSCALL_PATH_MAX] = {0};
    if (strncpy_from_user(name, (const char *)path, sizeof(name)) < 0) return -EFAULT;
    return swap_activate_path(name, (uint32_t)flags);
}

/* swapoff syscall: disable a swap area */
static int64_t sys_swapoff(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;
    if (!path) return -EFAULT;
    char name[SYSCALL_PATH_MAX] = {0};
    if (strncpy_from_user(name, (const char *)path, sizeof(name)) < 0) return -EFAULT;
    return swap_deactivate_path(name);
}

/* init_module syscall: load a kernel module */
static int64_t sys_init_module_impl(uint64_t image, uint64_t length, uint64_t user_params, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *process = process_current();
    if (!process) return -ESRCH;
    if (process->uid != 0) return -EPERM;
    if (!image || !length) return -EINVAL;
    if (length > SYSCALL_MODULE_MAX_SIZE) return -EFBIG;

    char params[MODULE_PARAM_MAX];
    int  ret = copy_module_params(user_params, params);
    if (ret != EOK) return ret;
    void *copy = malloc((size_t)length);
    if (!copy) {
        plogk("syscall: init_module image allocation failed (%llu bytes)\n", (unsigned long long)length);
        return -ENOMEM;
    }
    if (copy_from_user(copy, (const void *)image, (size_t)length)) {
        free(copy);
        return -EFAULT;
    }
    ret = module_load(copy, (size_t)length, params, 0, NULL);
    free(copy);
    return ret;
}

/* finit_module syscall: load a kernel module from a fd */
static int64_t sys_finit_module_impl(uint64_t fd, uint64_t user_params, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *process = process_current();
    if (!process) return -ESRCH;
    if (process->uid != 0) return -EPERM;

    char params[MODULE_PARAM_MAX];
    int  ret = copy_module_params(user_params, params);
    if (ret != EOK) return ret;
    process_file_t *file = process_fd_get(process, (int)fd);
    if (!file) return -EBADF;
    if ((file->flags & O_ACCMODE) == O_WRONLY) {
        process_file_put(file);
        return -EBADF;
    }
    size_t length = file->node ? file->node->size : 0;
    if (!length || length > SYSCALL_MODULE_MAX_SIZE) {
        process_file_put(file);
        return length ? -EFBIG : -EINVAL;
    }
    void *image = malloc(length);
    if (!image) {
        plogk("syscall: finit_module image allocation failed (%llu bytes)\n", (unsigned long long)length);
        process_file_put(file);
        return -ENOMEM;
    }
    size_t total = 0;
    while (total < length) {
        int64_t count = vfs_file_read(file->node, file->private_data, file->flags, (uint8_t *)image + total, total, length - total);
        if (count < 0) {
            ret = (int)count;
            goto out_finit;
        }
        if (!count) {
            ret = -EIO;
            goto out_finit;
        }
        total += (size_t)count;
    }
    ret = module_load(image, length, params, (unsigned int)flags, file->node->name);
out_finit:
    free(image);
    process_file_put(file);
    return ret;
}

/* delete_module syscall: unload a kernel module */
static int64_t sys_delete_module_impl(uint64_t user_name, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *process = process_current();
    if (!process) return -ESRCH;
    if (process->uid != 0) return -EPERM;
    char name[MODULE_NAME_LEN];
    if (!user_name) return -EFAULT;
    int ret = strncpy_from_user(name, (const char *)user_name, sizeof(name));
    if (ret < 0) return ret;
    return module_unload(name, (unsigned int)flags);
}

static syscall_fn_t syscall_table[SYS_MAX] = {
    [SYS_READ]                   = sys_read,
    [SYS_WRITE]                  = sys_write,
    [SYS_OPEN]                   = sys_open,
    [SYS_CLOSE]                  = sys_close,
    [SYS_STAT]                   = sys_stat,
    [SYS_FSTAT]                  = sys_fstat,
    [SYS_LSTAT]                  = sys_stat,
    [SYS_POLL]                   = sys_poll,
    [SYS_LSEEK]                  = sys_lseek,
    [SYS_MMAP]                   = sys_mmap,
    [SYS_MPROTECT]               = sys_mprotect_wrap,
    [SYS_MUNMAP]                 = sys_munmap,
    [SYS_BRK]                    = sys_brk,
    [SYS_RT_SIGACTION]           = sys_rt_sigaction_wrap,
    [SYS_RT_SIGPROCMASK]         = sys_rt_sigprocmask_wrap,
    [SYS_RT_SIGRETURN]           = sys_rt_sigreturn_wrap,
    [SYS_IOCTL]                  = sys_ioctl,
    [SYS_PREAD64]                = sys_pread64_impl,
    [SYS_PWRITE64]               = sys_pwrite64_impl,
    [SYS_READV]                  = sys_readv_wrap,
    [SYS_WRITEV]                 = sys_writev_wrap,
    [SYS_ACCESS]                 = sys_access_impl,
    [SYS_PIPE]                   = sys_pipe_wrap,
    [SYS_SELECT]                 = sys_select,
    [SYS_SCHED_YIELD]            = sys_sched_yield,
    [SYS_MREMAP]                 = sys_mremap_wrap,
    [SYS_MSYNC]                  = sys_msync_wrap,
    [SYS_MINCORE]                = sys_mincore_wrap,
    [SYS_MADVISE]                = sys_madvise_wrap,
    [SYS_SHMGET]                 = sys_shmget_wrap,
    [SYS_SHMAT]                  = sys_shmat_wrap,
    [SYS_SHMCTL]                 = sys_shmctl_wrap,
    [SYS_DUP]                    = sys_dup,
    [SYS_DUP2]                   = sys_dup2,
    [SYS_PAUSE]                  = sys_pause_wrap,
    [SYS_NANOSLEEP]              = sys_nanosleep,
    [SYS_GETITIMER]              = sys_getitimer_impl,
    [SYS_ALARM]                  = sys_alarm_impl,
    [SYS_SETITIMER]              = sys_setitimer_impl,
    [SYS_GETPID]                 = sys_getpid,
    [SYS_SENDFILE]               = sys_sendfile_impl,
    [SYS_SOCKET]                 = sys_socket_wrap,
    [SYS_CONNECT]                = sys_connect_wrap,
    [SYS_ACCEPT]                 = sys_accept_wrap,
    [SYS_SENDTO]                 = sys_sendto_wrap,
    [SYS_RECVFROM]               = sys_recvfrom_wrap,
    [SYS_SENDMSG]                = sys_sendmsg_wrap,
    [SYS_RECVMSG]                = sys_recvmsg_wrap,
    [SYS_SHUTDOWN]               = sys_shutdown_wrap,
    [SYS_BIND]                   = sys_bind_wrap,
    [SYS_LISTEN]                 = sys_listen_wrap,
    [SYS_GETSOCKNAME]            = sys_getsockname_wrap,
    [SYS_GETPEERNAME]            = sys_getpeername_wrap,
    [SYS_SOCKETPAIR]             = sys_socketpair_wrap,
    [SYS_SETSOCKOPT]             = sys_setsockopt_wrap,
    [SYS_GETSOCKOPT]             = sys_getsockopt_wrap,
    [SYS_CLONE]                  = NULL,
    [SYS_FORK]                   = NULL,
    [SYS_VFORK]                  = NULL,
    [SYS_EXECVE]                 = sys_execve_wrap,
    [SYS_EXIT]                   = sys_exit,
    [SYS_WAIT4]                  = sys_wait4,
    [SYS_KILL]                   = sys_kill,
    [SYS_UNAME]                  = sys_uname,
    [SYS_SEMGET]                 = sys_semget_wrap,
    [SYS_SEMOP]                  = sys_semop_wrap,
    [SYS_SEMCTL]                 = sys_semctl_wrap,
    [SYS_SHMDT]                  = sys_shmdt_wrap,
    [SYS_MSGGET]                 = sys_msgget_wrap,
    [SYS_MSGSND]                 = sys_msgsnd_wrap,
    [SYS_MSGRCV]                 = sys_msgrcv_wrap,
    [SYS_MSGCTL]                 = sys_msgctl_wrap,
    [SYS_FCNTL]                  = sys_fcntl_wrap,
    [SYS_FLOCK]                  = sys_flock_impl,
    [SYS_FSYNC]                  = sys_fsync_impl,
    [SYS_FDATASYNC]              = sys_fdatasync_impl,
    [SYS_TRUNCATE]               = sys_truncate_impl,
    [SYS_FTRUNCATE]              = sys_ftruncate_impl,
    [SYS_GETDENTS]               = sys_getdents64_wrap,
    [SYS_GETCWD]                 = sys_getcwd,
    [SYS_CHDIR]                  = sys_chdir_impl,
    [SYS_FCHDIR]                 = sys_fchdir_impl,
    [SYS_RENAME]                 = sys_rename,
    [SYS_MKDIR]                  = sys_mkdir,
    [SYS_RMDIR]                  = sys_unlink,
    [SYS_CREAT]                  = sys_creat,
    [SYS_LINK]                   = sys_link,
    [SYS_UNLINK]                 = sys_unlink,
    [SYS_SYMLINK]                = sys_symlink,
    [SYS_READLINK]               = sys_readlink,
    [SYS_CHMOD]                  = sys_chmod_impl,
    [SYS_FCHMOD]                 = sys_fchmod_impl,
    [SYS_CHOWN]                  = sys_chown_impl,
    [SYS_FCHOWN]                 = sys_fchown_impl,
    [SYS_LCHOWN]                 = sys_lchown_impl,
    [SYS_UMASK]                  = sys_umask_impl,
    [SYS_GETTIMEOFDAY]           = sys_gettimeofday,
    [SYS_GETRLIMIT]              = sys_getrlimit_impl,
    [SYS_GETRUSAGE]              = sys_getrusage_impl,
    [SYS_SYSINFO]                = sys_sysinfo_impl,
    [SYS_TIMES]                  = sys_times_impl,
    [SYS_PTRACE]                 = sys_ptrace_wrap,
    [SYS_GETUID]                 = sys_getuid,
    [SYS_SYSLOG]                 = sys_syslog_impl,
    [SYS_GETGID]                 = sys_getgid,
    [SYS_SETUID]                 = sys_setuid_impl,
    [SYS_SETGID]                 = sys_setgid_impl,
    [SYS_GETEUID]                = sys_getuid,
    [SYS_GETEGID]                = sys_getgid,
    [SYS_SETPGID]                = sys_setpgid_wrap,
    [SYS_GETPPID]                = sys_getppid,
    [SYS_GETPGRP]                = sys_getpgrp_wrap,
    [SYS_SETSID]                 = sys_setsid_wrap,
    [SYS_SETREUID]               = sys_setreuid_impl,
    [SYS_SETREGID]               = sys_setregid_impl,
    [SYS_GETGROUPS]              = sys_getgroups_impl,
    [SYS_SETGROUPS]              = sys_setgroups_impl,
    [SYS_SETRESUID]              = sys_setresuid_impl,
    [SYS_GETRESUID]              = sys_getresuid_impl,
    [SYS_SETRESGID]              = sys_setresgid_impl,
    [SYS_GETRESGID]              = sys_getresgid_impl,
    [SYS_GETPGID]                = sys_getpgid_wrap,
    [SYS_SETFSUID]               = sys_setfsuid_impl,
    [SYS_SETFSGID]               = sys_setfsgid_impl,
    [SYS_GETSID]                 = sys_getsid_wrap,
    [SYS_CAPGET]                 = sys_capget_impl,
    [SYS_CAPSET]                 = sys_capset_impl,
    [SYS_RT_SIGPENDING]          = sys_rt_sigpending_wrap,
    [SYS_RT_SIGTIMEDWAIT]        = sys_rt_sigtimedwait_wrap,
    [SYS_RT_SIGQUEUEINFO]        = sys_rt_sigqueueinfo_wrap,
    [SYS_RT_SIGSUSPEND]          = sys_rt_sigsuspend_wrap,
    [SYS_SIGALTSTACK]            = sys_sigaltstack_wrap,
    [SYS_UTIME]                  = sys_utime_impl,
    [SYS_MKNOD]                  = sys_mknod_impl,
    [SYS_USELIB]                 = sys_stub,
    [SYS_PERSONALITY]            = sys_personality_impl,
    [SYS_USTAT]                  = sys_stub,
    [SYS_STATFS]                 = sys_statfs_impl,
    [SYS_FSTATFS]                = sys_fstatfs_impl,
    [SYS_SYSFS]                  = sys_stub,
    [SYS_GETPRIORITY]            = sys_getpriority_impl,
    [SYS_SETPRIORITY]            = sys_setpriority_impl,
    [SYS_SCHED_SETPARAM]         = sys_sched_setparam_impl,
    [SYS_SCHED_GETPARAM]         = sys_sched_getparam_impl,
    [SYS_SCHED_SETSCHEDULER]     = sys_sched_setscheduler_impl,
    [SYS_SCHED_GETSCHEDULER]     = sys_sched_getscheduler_impl,
    [SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max_impl,
    [SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min_impl,
    [SYS_SCHED_RR_GET_INTERVAL]  = sys_sched_rr_get_interval_impl,
    [SYS_MLOCK]                  = sys_mlock_wrap,
    [SYS_MUNLOCK]                = sys_munlock_wrap,
    [SYS_MLOCKALL]               = sys_mlockall_wrap,
    [SYS_MUNLOCKALL]             = sys_munlockall_wrap,
    [SYS_VHANGUP]                = sys_stub,
    [SYS_MODIFY_LDT]             = sys_stub,
    [SYS_PIVOT_ROOT]             = sys_stub,
    [SYS__SYSCTL]                = sys_stub,
    [SYS_PRCTL]                  = sys_prctl_impl,
    [SYS_ARCH_PRCTL]             = sys_arch_prctl,
    [SYS_ADJTIMEX]               = sys_adjtimex_impl,
    [SYS_SETRLIMIT]              = sys_setrlimit_impl,
    [SYS_CHROOT]                 = sys_chroot_wrap,
    [SYS_SYNC]                   = sys_sync_impl,
    [SYS_ACCT]                   = sys_acct_impl,
    [SYS_SETTIMEOFDAY]           = sys_settimeofday_impl,
    [SYS_MOUNT]                  = sys_mount,
    [SYS_UMOUNT2]                = sys_umount2,
    [SYS_SWAPON]                 = sys_swapon,
    [SYS_SWAPOFF]                = sys_swapoff,
    [SYS_REBOOT]                 = sys_reboot_impl,
    [SYS_SETHOSTNAME]            = sys_sethostname_impl,
    [SYS_SETDOMAINNAME]          = sys_setdomainname_impl,
    [SYS_IOPL]                   = sys_stub,
    [SYS_IOPERM]                 = sys_stub,
    [SYS_CREATE_MODULE]          = sys_stub,
    [SYS_INIT_MODULE]            = sys_init_module_impl,
    [SYS_DELETE_MODULE]          = sys_delete_module_impl,
    [SYS_GET_KERNEL_SYMS]        = sys_stub,
    [SYS_QUERY_MODULE]           = sys_stub,
    [SYS_QUOTACTL]               = sys_stub,
    [SYS_NFSSERVCTL]             = sys_stub,
    [SYS_GETPMSG]                = sys_stub,
    [SYS_PUTPMSG]                = sys_stub,
    [SYS_AFS_SYSCALL]            = sys_stub,
    [SYS_TUXCALL]                = sys_stub,
    [SYS_SECURITY]               = sys_stub,
    [SYS_GETTID]                 = sys_gettid,
    [SYS_READAHEAD]              = sys_readahead,
    [SYS_SETXATTR]               = sys_setxattr_impl,
    [SYS_LSETXATTR]              = sys_setxattr_impl,
    [SYS_FSETXATTR]              = sys_setxattr_impl,
    [SYS_GETXATTR]               = sys_getxattr_impl,
    [SYS_LGETXATTR]              = sys_getxattr_impl,
    [SYS_FGETXATTR]              = sys_getxattr_impl,
    [SYS_LISTXATTR]              = sys_listxattr_impl,
    [SYS_LLISTXATTR]             = sys_listxattr_impl,
    [SYS_FLISTXATTR]             = sys_listxattr_impl,
    [SYS_REMOVEXATTR]            = sys_removexattr_impl,
    [SYS_LREMOVEXATTR]           = sys_removexattr_impl,
    [SYS_FREMOVEXATTR]           = sys_removexattr_impl,
    [SYS_TKILL]                  = sys_tkill_real,
    [SYS_TIME]                   = sys_time,
    [SYS_FUTEX]                  = sys_futex_wrap,
    [SYS_SCHED_SETAFFINITY]      = sys_sched_setaffinity_impl,
    [SYS_SCHED_GETAFFINITY]      = sys_sched_getaffinity_impl,
    [SYS_SET_THREAD_AREA]        = sys_stub,
    [SYS_IO_SETUP]               = sys_stub,
    [SYS_IO_DESTROY]             = sys_stub,
    [SYS_IO_GETEVENTS]           = sys_stub,
    [SYS_IO_SUBMIT]              = sys_stub,
    [SYS_IO_CANCEL]              = sys_stub,
    [SYS_GET_THREAD_AREA]        = sys_stub,
    [SYS_LOOKUP_DCOOKIE]         = sys_stub,
    [SYS_EPOLL_CREATE]           = sys_epoll_create_wrap,
    [SYS_EPOLL_CTL_OLD]          = sys_epoll_ctl_wrap,
    [SYS_EPOLL_WAIT_OLD]         = sys_epoll_wait_wrap,
    [SYS_REMAP_FILE_PAGES]       = sys_stub,
    [SYS_GETDENTS64]             = sys_getdents64_wrap,
    [SYS_SET_TID_ADDRESS]        = sys_set_tid_address_impl,
    [SYS_RESTART_SYSCALL]        = sys_restart_syscall,
    [SYS_SEMTIMEDOP]             = sys_semtimedop_wrap,
    [SYS_FADVISE64]              = sys_fadvise64,
    [SYS_TIMER_CREATE]           = sys_timer_create_impl,
    [SYS_TIMER_SETTIME]          = sys_timer_settime_impl,
    [SYS_TIMER_GETTIME]          = sys_timer_gettime_impl,
    [SYS_TIMER_GETOVERRUN]       = sys_timer_getoverrun_impl,
    [SYS_TIMER_DELETE]           = sys_timer_delete_impl,
    [SYS_CLOCK_SETTIME]          = sys_clock_settime_impl,
    [SYS_CLOCK_GETTIME]          = sys_clock_gettime_impl,
    [SYS_CLOCK_GETRES]           = sys_clock_getres_impl,
    [SYS_CLOCK_NANOSLEEP]        = sys_clock_nanosleep_impl,
    [SYS_EXIT_GROUP]             = sys_exit_group,
    [SYS_EPOLL_WAIT]             = sys_epoll_wait_wrap,
    [SYS_EPOLL_CTL]              = sys_epoll_ctl_wrap,
    [SYS_TGKILL]                 = sys_tgkill_wrap,
    [SYS_UTIMES]                 = sys_utimes_impl,
    [SYS_VSERVER]                = sys_stub,
    [SYS_MBIND]                  = sys_stub,
    [SYS_SET_MEMPOLICY]          = sys_stub,
    [SYS_GET_MEMPOLICY]          = sys_stub,
    [SYS_MQ_OPEN]                = sys_mq_open_wrap,
    [SYS_MQ_UNLINK]              = sys_mq_unlink_wrap,
    [SYS_MQ_TIMEDSEND]           = sys_mq_timedsend_wrap,
    [SYS_MQ_TIMEDRECEIVE]        = sys_mq_timedreceive_wrap,
    [SYS_MQ_NOTIFY]              = sys_mq_notify_wrap,
    [SYS_MQ_GETSETATTR]          = sys_mq_getsetattr_wrap,
    [SYS_KEXEC_LOAD]             = sys_stub,
    [SYS_WAITID]                 = sys_waitid_impl,
    [SYS_ADD_KEY]                = sys_stub,
    [SYS_REQUEST_KEY]            = sys_stub,
    [SYS_KEYCTL]                 = sys_stub,
    [SYS_IOPRIO_SET]             = sys_ioprio_set_impl,
    [SYS_IOPRIO_GET]             = sys_ioprio_get_impl,
    [SYS_INOTIFY_INIT]           = sys_inotify_init_wrap,
    [SYS_INOTIFY_ADD_WATCH]      = sys_inotify_add_watch_wrap,
    [SYS_INOTIFY_RM_WATCH]       = sys_inotify_rm_watch_wrap,
    [SYS_MIGRATE_PAGES]          = sys_stub,
    [SYS_OPENAT]                 = sys_openat,
    [SYS_MKDIRAT]                = sys_mkdirat,
    [SYS_MKNODAT]                = sys_mknodat_impl,
    [SYS_FCHOWNAT]               = sys_fchownat_impl,
    [SYS_FUTIMESAT]              = sys_futimesat_impl,
    [SYS_NEWFSTATAT]             = sys_newfstatat,
    [SYS_UNLINKAT]               = sys_unlinkat,
    [SYS_RENAMEAT]               = sys_renameat,
    [SYS_LINKAT]                 = sys_linkat,
    [SYS_SYMLINKAT]              = sys_symlinkat,
    [SYS_READLINKAT]             = sys_readlinkat,
    [SYS_FCHMODAT]               = sys_fchmodat_impl,
    [SYS_FACCESSAT]              = sys_faccessat_impl,
    [SYS_PSELECT6]               = sys_pselect6,
    [SYS_PPOLL]                  = sys_ppoll,
    [SYS_UNSHARE]                = sys_unshare_impl,
    [SYS_SET_ROBUST_LIST]        = sys_set_robust_list_impl,
    [SYS_GET_ROBUST_LIST]        = sys_get_robust_list_impl,
    [SYS_SPLICE]                 = sys_splice_impl,
    [SYS_TEE]                    = sys_tee_impl,
    [SYS_SYNC_FILE_RANGE]        = sys_sync_file_range_impl,
    [SYS_VMSPLICE]               = sys_vmsplice_impl,
    [SYS_MOVE_PAGES]             = sys_stub,
    [SYS_UTIMENSAT]              = sys_utimensat_impl,
    [SYS_EPOLL_PWAIT]            = sys_epoll_pwait_wrap,
    [SYS_SIGNALFD]               = sys_signalfd_wrap,
    [SYS_TIMERFD_CREATE]         = sys_timerfd_create_wrap,
    [SYS_EVENTFD]                = sys_eventfd_wrap,
    [SYS_FALLOCATE]              = sys_fallocate_impl,
    [SYS_TIMERFD_SETTIME]        = sys_timerfd_settime_wrap,
    [SYS_TIMERFD_GETTIME]        = sys_timerfd_gettime_wrap,
    [SYS_ACCEPT4]                = sys_accept4_wrap,
    [SYS_SIGNALFD4]              = sys_signalfd4_wrap,
    [SYS_EVENTFD2]               = sys_eventfd2_wrap,
    [SYS_EPOLL_CREATE1]          = sys_epoll_create1_wrap,
    [SYS_DUP3]                   = sys_dup3,
    [SYS_PIPE2]                  = sys_pipe2_wrap,
    [SYS_INOTIFY_INIT1]          = sys_inotify_init1_wrap,
    [SYS_PREADV]                 = sys_preadv_impl,
    [SYS_PWRITEV]                = sys_pwritev_impl,
    [SYS_RT_TGSIGQUEUEINFO]      = sys_rt_tgsigqueueinfo_wrap,
    [SYS_PERF_EVENT_OPEN]        = sys_stub,
    [SYS_RECVMMSG]               = sys_recvmmsg_wrap,
    [SYS_FANOTIFY_INIT]          = sys_stub,
    [SYS_FANOTIFY_MARK]          = sys_stub,
    [SYS_PRLIMIT64]              = sys_prlimit64_impl,
    [SYS_NAME_TO_HANDLE_AT]      = sys_stub,
    [SYS_OPEN_BY_HANDLE_AT]      = sys_stub,
    [SYS_CLOCK_ADJTIME]          = sys_clock_adjtime_impl,
    [SYS_SYNCFS]                 = sys_syncfs_impl,
    [SYS_SENDMMSG]               = sys_sendmmsg_wrap,
    [SYS_SETNS]                  = sys_stub,
    [SYS_GETCPU]                 = sys_getcpu_impl,
    [SYS_PROCESS_VM_READV]       = sys_process_vm_readv_impl,
    [SYS_PROCESS_VM_WRITEV]      = sys_process_vm_writev_impl,
    [SYS_KCMP]                   = sys_stub,
    [SYS_FINIT_MODULE]           = sys_finit_module_impl,
    [SYS_SCHED_SETATTR]          = sys_sched_setattr_impl,
    [SYS_SCHED_GETATTR]          = sys_sched_getattr_impl,
    [SYS_RENAMEAT2]              = sys_renameat2_impl,
    [SYS_SECCOMP]                = sys_seccomp,
    [SYS_GETRANDOM]              = sys_getrandom_impl,
    [SYS_MEMFD_CREATE]           = sys_memfd_create,
    [SYS_KEXEC_FILE_LOAD]        = sys_stub,
    [SYS_BPF]                    = sys_stub,
    [SYS_EXECVEAT]               = sys_execveat_stub,
    [SYS_USERFAULTFD]            = sys_stub,
    [SYS_MEMBARRIER]             = sys_membarrier_stub,
    [SYS_MLOCK2]                 = sys_mlock2_stub,
    [SYS_COPY_FILE_RANGE]        = sys_copy_file_range_stub,
    [SYS_PREADV2]                = sys_preadv2_impl,
    [SYS_PWRITEV2]               = sys_pwritev2_impl,
    [SYS_PKEY_MPROTECT]          = sys_pkey_mprotect_stub,
    [SYS_PKEY_ALLOC]             = sys_pkey_alloc_impl,
    [SYS_PKEY_FREE]              = sys_pkey_free_impl,
    [SYS_STATX]                  = sys_statx,
    [SYS_IO_PGETEVENTS]          = sys_io_pgetevents_impl,
    [SYS_RSEQ]                   = sys_rseq_impl,
    [SYS_PIDFD_SEND_SIGNAL]      = sys_pidfd_send_signal_impl,
    [SYS_IO_URING_SETUP]         = sys_stub,
    [SYS_IO_URING_ENTER]         = sys_stub,
    [SYS_IO_URING_REGISTER]      = sys_stub,
    [SYS_OPEN_TREE]              = sys_stub,
    [SYS_MOVE_MOUNT]             = sys_stub,
    [SYS_FSOPEN]                 = sys_stub,
    [SYS_FSCONFIG]               = sys_stub,
    [SYS_FSMOUNT]                = sys_stub,
    [SYS_FSPICK]                 = sys_stub,
    [SYS_PIDFD_OPEN]             = sys_pidfd_open_impl,
    [SYS_CLONE3]                 = NULL, // frame-aware: dispatched in syscall_dispatch
    [SYS_CLOSE_RANGE]            = sys_close_range,
    [SYS_OPENAT2]                = sys_openat2_impl,
    [SYS_PIDFD_GETFD]            = sys_pidfd_getfd_impl,
    [SYS_FACCESSAT2]             = sys_faccessat2_impl,
    [SYS_PROCESS_MADVISE]        = sys_process_madvise_impl,
    [SYS_EPOLL_PWAIT2]           = sys_epoll_pwait2_impl,
    [SYS_FUTEX_WAITV]            = sys_futex_waitv,
    [SYS_FUTEX_WAKE]             = sys_futex_wake,
    [SYS_FUTEX_WAIT]             = sys_futex_wait,
    [SYS_FUTEX_REQUEUE]          = sys_futex_requeue,
    [SYS_FCHMODAT2]              = sys_fchmodat2_impl,
};

/* Helper: check if a return value is a kernel-internal restart code. */
static inline int is_restart_code(int64_t ret)
{
    return ret == -ERESTARTSYS || ret == -ERESTARTNOINTR || ret == -ERESTARTNOHAND || ret == -ERESTART_RESTARTBLOCK || ret == -ERESTART;
}

/* Dispatch a syscall from a saved register frame */
int syscall_dispatch(syscall_frame_t *frame)
{
    uint64_t num           = frame->rax;
    int64_t  retval        = 0;
    task_t  *dispatch_task = current_task();
    bool     traced        = dispatch_task && ptrace_tracer_pid(dispatch_task);
    bool     force_iret    = traced;

    if (traced) ptrace_syscall_enter(frame, num);
    num = frame->rax;
    if (!seccomp_enforce(frame, &num, &retval)) {
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }
    frame->rax = num;
    if (num == SYS_CLONE && (frame->rdi & CLONE_THREAD)) {
        uint64_t flags = frame->rdi;
        if ((flags & CLONE_PTHREAD_REQUIRED) != CLONE_PTHREAD_REQUIRED || (flags & ~CLONE_PTHREAD_ALLOWED)) {
            frame->rax = (uint64_t)-EINVAL;
            goto check_signals;
        }
        if (((flags & CLONE_PARENT_SETTID) && !frame->rdx) || ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !frame->r10)) {
            frame->rax = (uint64_t)-EFAULT;
            goto check_signals;
        }
        int     error = EOK;
        task_t *child = process_clone_thread(frame, frame->rsi, (flags & CLONE_PARENT_SETTID) ? frame->rdx : 0, (flags & CLONE_CHILD_SETTID) ? frame->r10 : 0,
                                             (flags & CLONE_CHILD_CLEARTID) ? frame->r10 : 0, (flags & CLONE_SETTLS) ? frame->r8 : current_task()->thread.fs_base, &error);
        retval        = child ? (int64_t)child->pid : error;
        frame->rax    = (uint64_t)retval;
        if (child) ptrace_fork_event(frame, PTRACE_EVENT_CLONE, child->pid);
        goto check_signals;
    }

    if (num == SYS_FORK || num == SYS_VFORK || num == SYS_CLONE) {
        uint64_t clone_flags = num == SYS_CLONE ? frame->rdi : SIGCHLD;
        bool     vfork       = num == SYS_VFORK || (clone_flags & CLONE_VFORK);
        uint64_t tid_flags   = CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
        uint64_t supported   = SIGCHLD | tid_flags | CLONE_DETACHED;
        if (vfork) supported |= CLONE_VM | CLONE_VFORK;
        if ((num == SYS_CLONE && (clone_flags & ~supported)) || (num == SYS_CLONE && vfork && !(clone_flags & CLONE_VM)) || ((clone_flags & 0xff) != SIGCHLD)
            || (vfork && num == SYS_CLONE && !frame->rsi) || ((clone_flags & CLONE_PARENT_SETTID) && !frame->rdx) || ((clone_flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !frame->r10)) {
            frame->rax = (uint64_t)-EINVAL;
            goto check_signals;
        }
        if (((clone_flags & CLONE_PARENT_SETTID) && !user_access_ok((void *)frame->rdx, sizeof(uint32_t), 1))
            || ((clone_flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !user_access_ok((void *)frame->r10, sizeof(uint32_t), 1))) {
            frame->rax = (uint64_t)-EFAULT;
            goto check_signals;
        }
        int        error = EOK;
        uint32_t   event = vfork ? PTRACE_EVENT_VFORK : PTRACE_EVENT_FORK;
        process_t *child = process_fork_status_event_mode(&error, event, vfork);
        if (child) {
            uint64_t        kstack_top  = (uint64_t)(child->kernel_stack + PROCESS_KERNEL_STACK);
            uint64_t       *kstack      = (uint64_t *)ALIGN_DOWN(kstack_top, 16ULL);
            syscall_frame_t child_frame = *frame;
            child_frame.rax             = 0;
            if (vfork && num == SYS_CLONE) child_frame.rsp = frame->rsi;
            kstack -= sizeof(syscall_frame_t) / sizeof(uint64_t);
            memcpy(kstack, &child_frame, sizeof(syscall_frame_t));
            *(--kstack)              = (uint64_t)syscall_return;
            child->task->context.rsp = (uint64_t)kstack;
            uint32_t child_tid       = (uint32_t)child->task->pid;
            int      tid_error       = 0;
            if ((clone_flags & CLONE_PARENT_SETTID) && copy_to_user((void *)frame->rdx, &child_tid, sizeof(child_tid))) tid_error = -EFAULT;
            if (!tid_error && (clone_flags & CLONE_CHILD_SETTID)
                && (!user_access_ok_process(child, (void *)frame->r10, sizeof(child_tid), 1) || copy_to_user_process_nofault(child, (void *)frame->r10, &child_tid, sizeof(child_tid))))
                tid_error = -EFAULT;
            if (tid_error) {
                process_fork_discard(child);
                child = NULL;
                error = tid_error;
            } else if (clone_flags & CLONE_CHILD_CLEARTID) {
                child->task->clear_child_tid = frame->r10;
            }
        }
        if (child) { process_fork_publish(child); }
        retval     = child ? (int64_t)child->task->pid : (int64_t)error;
        frame->rax = (uint64_t)retval;
        if (child) ptrace_fork_event(frame, event, child->task->pid);
        if (child && vfork) {
            process_vfork_wait(child);
            ptrace_fork_event(frame, PTRACE_EVENT_VFORK_DONE, child->task->pid);
        }
        goto check_signals;
    }

    /*
     * clone3 seeds the child's resume frame (fork with a caller-provided stack)
     * and clones threads with the parent's register context, so it needs frame
     * access rather than the plain syscall-table ABI.
     */
    if (num == SYS_CLONE3) {
        retval     = sys_clone3_impl(frame, frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }

    /*
     * read/write dominate the small-block streaming path. The dispatcher
     * already has the task selected by current_task(), so avoid a second
     * task lookup, the syscall-table lookup, and the indirect call.
     */
    if (num == SYS_READ) {
        retval     = sys_read_task(dispatch_task, frame->rdi, frame->rsi, frame->rdx);
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }
    if (num == SYS_WRITE) {
        retval     = sys_write_task(dispatch_task, frame->rdi, frame->rsi, frame->rdx);
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }

    if (num >= SYS_MAX || !syscall_table[num]) {
        task_t *task = current_task();
        plogk("syscall: Unimplemented syscall %llu from pid %llu (%s)\n", num, task ? task->pid : 0, task ? task->name : "?");
        retval     = -ENOSYS;
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }

    /*
     * sys_stub intentionally shares one implementation; record the syscall
     * number here, where the dispatcher still has it, so userspace probes
     * (notably Xorg) identify the missing ABI entry in the kernel log.
     */
    if (syscall_table[num] == sys_stub) {
        task_t *task = current_task();
        plogk("syscall: syscall %llu is not implemented (pid %llu, %s)\n", num, task ? task->pid : 0, task ? task->name : "?");
    }

    if (num == SYS_EXECVE) {
        retval     = do_execve((const char *)frame->rdi, (char *const *)frame->rsi, (char *const *)frame->rdx, frame);
        frame->rax = (uint64_t)retval;
        if (!retval) ptrace_exec_event(frame);
        goto check_signals;
    }

    if (num == SYS_EXECVEAT) {
        retval     = do_execveat(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame);
        frame->rax = (uint64_t)retval;
        if (!retval) ptrace_exec_event(frame);
        goto check_signals;
    }

    if (num == SYS_RT_SIGRETURN) {
        /*
         * sigreturn restores the saved register context from the
         * signal frame on the user stack. do_rt_sigreturn fills in
         * ALL fields of the syscall frame, including rax.
         * We must NOT override frame->rax here.
         *
         * After restoration, check for pending signals (the old
         * blocked mask was restored, which may unblock signals).
         */
        int64_t sr_ret = do_rt_sigreturn(frame);
        if (sr_ret != 0) {
            /*
             * Linux treats a malformed signal frame as a fatal badframe.
             * Returning to the restorer after a failed sigreturn executes
             * whatever bytes follow its syscall stub and commonly raises
             * the misleading user #GP seen after Ctrl+C.
             */
            process_exit_group(-SIGSEGV);
            return 0;
        }
        /*
         * Success: frame is fully restored; check pending signals.
         * Bypass the normal retval path to avoid overriding frame->rax.
         * Do NOT go through the restart logic since we're restoring
         * a previous context, not returning from a syscall.
         */
        if (frame->cs & 0x3) signal_deliver_for_process(dispatch_task ? dispatch_task->process : NULL, frame);
        return 0;
    }

    retval     = syscall_table[num](frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
    frame->rax = (uint64_t)retval;
check_signals:
    if (traced) {
        force_iret = true;
        ptrace_syscall_exit(frame, (int64_t)frame->rax);
    }
    retval = (int64_t)frame->rax;
    /*
     * On return to userspace, deliver any pending signals.
     * The signal subsystem sets up handler frames (redirecting RIP/RSP)
     * but does NOT modify frame->rax.
     *
     * After signal delivery, handle syscall restart:
     * - If the syscall body returned a restart code (-ERESTARTSYS, etc.)
     *   and a signal handler was installed, we either restart the syscall
     *   (by adjusting frame->rip back before the syscall instruction) or
     *   return -EINTR.
     * - If the syscall completed successfully (or returned a non-restart
     *   error), the return value is preserved regardless of signals.
     */
    if (frame->cs & 0x3) {
        uint64_t saved_rip = frame->rip;

        int sig_ret = signal_deliver_for_process(dispatch_task ? dispatch_task->process : NULL, frame);
        if (frame->rip != saved_rip) force_iret = true;

        if (sig_ret == 1) {
            /*
             * Process terminated by signal default action
             * (signal_deliver_if_pending already called process_exit)
             */
            task_exit();
            return 0;
        }

        /* Handle syscall restart if the syscall was interrupted */
        if (is_restart_code(retval)) {
            /*
             * The syscall body returned a restart code, meaning it
             * detected a pending signal and was interrupted before
             * completing.
             *
             * signal_deliver_if_pending() has now delivered the signal.
             * We need to decide whether to restart or return -EINTR.
             */
            bool restart = false;

            if (retval == -ERESTARTNOINTR || retval == -ERESTART_RESTARTBLOCK) {
                /* Always restart regardless of SA_RESTART */
                restart = true;
            } else if (retval == -ERESTARTSYS) {
                /* Restart only if the handler has SA_RESTART */
                /*
                 * For now check if a user handler was set up:
                 * if signal_deliver_if_pending set frame->rip to a handler
                 * (frame->rip != saved_rip) and it has SA_RESTART flag,
                 * we could restart. Since we don't track which signal
                 * was the one that interrupted, we need to be conservative.
                 *
                 * For SIG_DFL or SIG_IGN (no user handler), we always
                 * restart with ERESTARTSYS. With a user handler, the
                 * SA_RESTART flag on that signal determines restart.
                 *
                 * We check if frame->rip was changed (handler installed):
                 * - If unchanged: no user handler, so restart
                 * - If changed: user handler installed, need SA_RESTART
                 * which we don't have readily available here.
                 * For full correctness, we should convert to -EINTR.
                 */
                if (frame->rip == saved_rip) {
                    /* No user handler was set up, restart */
                    restart = true;
                } else {
                    /*
                     * User handler was set up, check SA_RESTART.
                     * Since we cannot easily know which signal interrupted
                     * the syscall without deeper plumbing, we convert to
                     * -EINTR for safety. Full SA_RESTART support requires
                     * passing the signal number from signal_deliver_if_pending
                     * back to this function.
                     */
                    restart = false;
                }
            } else if (retval == -ERESTARTNOHAND) {
                /* Restart only if no user handler was set up */
                restart = (frame->rip == saved_rip);
            } else if (retval == -ERESTART) {
                /* Legacy ERESTART: restart if SA_RESTART */
                if (frame->rip == saved_rip) restart = true;
            }

            if (restart) {
                /*
                 * Restart the syscall: restore the original syscall number
                 * in frame->rax and adjust frame->rip to point back before
                 * the syscall instruction. When we return to userspace:
                 *   - RAX = original syscall number
                 *   - RIP = address of int 0x80 / syscall instruction
                 * The application will re-execute the syscall.
                 *
                 * Both int 0x80 (2 bytes) and syscall (2 bytes) have
                 * the return RIP pointing 2 bytes after the instruction.
                 */
                frame->rax = num;
                frame->rip = saved_rip - 2;
            } else {
                /* Not restarting: return -EINTR to userspace */
                frame->rax = (uint64_t)-EINTR;
            }
        }
        /*
         * If retval is NOT a restart code (e.g., syscall completed
         * successfully or returned a non-restart error), frame->rax
         * is left unchanged. The signal is delivered but the syscall
         * return value is preserved.
         */
    }

    /*
     * SYSRET is valid only for the ordinary 64-bit userspace selectors and
     * canonical lower-half addresses.  Full-context restoration paths use
     * IRETQ so RCX/R11 are restored instead of taking their syscall-ABI role.
     */
    if (force_iret || frame->cs != 0x33 || frame->ss != 0x2b || frame->rip >= PROCESS_USER_STACK_TOP || frame->rsp >= PROCESS_USER_STACK_TOP) return 0;
    return 1;
}

/*
 * Restore registers and return via IRETQ.  The whole kernel runs with
 * %gs -> per-CPU, so swapgs restores the task's user GS before returning;
 * cli keeps a timer interrupt from landing between swapgs and iretq (iretq
 * restores the saved IF).  Also the resume path for fork/clone/thread, which
 * context_switch() reaches with IF already enabled.
 */
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
                     "cli\n\t"
                     "swapgs\n\t"
                     "iretq\n\t");
}

/* Restore registers and return via SYSRET */
__attribute__((naked, used)) static void syscall_return_sysret(void)
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
                     "movq 0(%rsp), %rcx\n\t"
                     "movq 16(%rsp), %r11\n\t"
                     "cli\n\t"
                     "movq 24(%rsp), %rsp\n\t"
                     "swapgs\n\t"
                     "sysretq\n\t");
}

/* Interrupt-based syscall entry point (int 0x80, always from user mode) */
__attribute__((naked)) void syscall_entry(void)
{
    __asm__ volatile("swapgs\n\t"
                     "cld\n\t"
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

/* SYSCALL-instruction entry point */
__attribute__((naked)) static void syscall_entry_syscall(void)
{
    __asm__ volatile(
        "swapgs\n\t"
        "movq %rsp, %gs:" SYSCALL_STRINGIFY(SYSCALL_CPU_USER_RSP_OFFSET) "\n\t"
                                                                         "movq %gs:" SYSCALL_STRINGIFY(SYSCALL_CPU_KERNEL_RSP_OFFSET) ", %rsp\n\t"
                                                                                                                                      "cld\n\t"
                                                                                                                                      "pushq $0x2B\n\t"
                                                                                                                                      "pushq %gs:" SYSCALL_STRINGIFY(
                                                                                                                                          SYSCALL_CPU_USER_RSP_OFFSET) "\n\t"
                                                                                                                                                                       "pushq %r11\n\t"
                                                                                                                                                                       "pushq $0x33\n\t"
                                                                                                                                                                       "pushq %rcx\n\t"
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
                                                                                                                                                                       "testl %eax, %eax\n\t"
                                                                                                                                                                       "jz syscall_return\n\t"
                                                                                                                                                                       "jmp syscall_return_sysret\n\t");
}

/*
 * Program this CPU's SYSCALL MSRs.  The kernel-mode GS base is established
 * separately (cpu_gs_install in smp.c) so the BSP has it before sched_init().
 */
void syscall_init_cpu(void)
{
    uint64_t star = rdmsr(0xC0000081);
    star &= 0x00000000FFFFFFFFULL;
    /* SYSRET adds 16 to STAR[63:48] for CS and 8 for SS. */
    star |= ((uint64_t)0x08 << 32) | ((uint64_t)0x23 << 48);
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (uint64_t)syscall_entry_syscall);
    wrmsr(0xC0000084, 0x200);
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);
}

/* Install the syscall entry points */
void syscall_init(void)
{
    register_interrupt_handler(SYSCALL_VECTOR, (void *)syscall_entry, 0, 0xee);

    cpu_processor_t *cpu = get_current_cpu();
    if (!cpu) panic("syscall: BSP per-CPU state is unavailable.");
    syscall_init_cpu();
    plogk("syscall: int 0x%02x interface initialized.\n", SYSCALL_VECTOR);
}
