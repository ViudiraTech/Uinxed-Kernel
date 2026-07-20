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
#include <string.h>
#include <syscall.h>
#include <syscall_table.h>
#include <task.h>
#include <uaccess.h>
#include <uinxed.h>
#include <vfs.h>

#define SYSCALL_PATH_MAX  256
#define SYSCALL_IO_CHUNK  4096
#define AT_FDCWD          -100
#define AT_REMOVEDIR      0x200
#define SIGCHLD           17
#define STATX_BASIC_STATS 0x000007ffU

typedef struct {
        int16_t l_type;
        int16_t l_whence;
        int64_t l_start;
        int64_t l_len;
        int32_t l_pid;
} linux_flock_t;

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
        int64_t tv_sec;
        int64_t tv_nsec;
} linux_timespec_t;

typedef struct {
        int64_t tv_sec;
        int64_t tv_usec;
} linux_timeval_t;

typedef struct {
        int32_t fd;
        int16_t events;
        int16_t revents;
} linux_pollfd_t;

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
        uint64_t                __spare2[14];
} linux_statx_t;

static int copy_path_from_user(uint64_t upath, char path[SYSCALL_PATH_MAX])
{
    if (!upath) return -EFAULT;
    return strncpy_from_user(path, (const char *)upath, SYSCALL_PATH_MAX) < 0 ? -EFAULT : EOK;
}

static uint32_t linux_mode_from_type(uint16_t type, uint32_t mode)
{
    uint32_t file_type = 0100000;

    if (type & file_dir)
        file_type = 0040000;
    else if (type & file_symlink)
        file_type = 0120000;
    else if (type & file_block)
        file_type = 0060000;
    else if (type & (file_stream | file_keyboard | file_mouse | file_fbdev | file_audio))
        file_type = 0020000;
    else if (type & file_pipe)
        file_type = 0010000;
    else if (type & file_socket)
        file_type = 0140000;

    return file_type | (mode & 07777);
}

static void fill_linux_stat(linux_stat_t *st, uint64_t uid, uint64_t gid, const process_fd_stat_t *src)
{
    memset(st, 0, sizeof(*st));
    st->st_dev     = src->dev;
    st->st_ino     = src->inode;
    st->st_nlink   = 1;
    st->st_mode    = linux_mode_from_type(src->type, src->mode);
    st->st_uid     = (uint32_t)uid;
    st->st_gid     = (uint32_t)gid;
    st->st_rdev    = src->rdev;
    st->st_size    = (int64_t)src->size;
    st->st_blksize = src->blksz ? (int64_t)src->blksz : 4096;
    st->st_blocks  = (st->st_size + 511) / 512;
}

static void fill_linux_statx(linux_statx_t *stx, uint64_t uid, uint64_t gid, const process_fd_stat_t *src)
{
    memset(stx, 0, sizeof(*stx));
    stx->stx_mask       = STATX_BASIC_STATS;
    stx->stx_blksize    = src->blksz ? (uint32_t)src->blksz : 4096;
    stx->stx_nlink      = 1;
    stx->stx_uid        = (uint32_t)uid;
    stx->stx_gid        = (uint32_t)gid;
    stx->stx_mode       = (uint16_t)linux_mode_from_type(src->type, src->mode);
    stx->stx_ino        = src->inode;
    stx->stx_size       = src->size;
    stx->stx_blocks     = (src->size + 511) / 512;
    stx->stx_rdev_major = (uint32_t)(src->rdev >> 8);
    stx->stx_rdev_minor = (uint32_t)(src->rdev & 0xff);
    stx->stx_dev_major  = (uint32_t)(src->dev >> 8);
    stx->stx_dev_minor  = (uint32_t)(src->dev & 0xff);
}

static int64_t stat_path_to_user(uint64_t upath, uint64_t ubuf)
{
    char path[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(upath, path);
    if (ret != EOK) return ret;
    if (!ubuf) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = vfs_open(path);
    if (!node) return -ENOENT;
    vfs_update(node);

    process_fd_stat_t src = {
        .dev   = node->dev,
        .inode = node->inode,
        .mode  = node->mode,
        .type  = node->type,
        .rdev  = node->rdev,
        .size  = node->size,
        .blksz = node->blksz,
    };
    linux_stat_t st;
    fill_linux_stat(&st, proc->uid, proc->gid, &src);
    vfs_close(node);
    return copy_to_user((void *)ubuf, &st, sizeof(st)) ? -EFAULT : EOK;
}

static int64_t statx_path_to_user(uint64_t upath, uint64_t ubuf)
{
    char path[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(upath, path);
    if (ret != EOK) return ret;
    if (!ubuf) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = vfs_open(path);
    if (!node) return -ENOENT;
    vfs_update(node);

    process_fd_stat_t src = {
        .dev   = node->dev,
        .inode = node->inode,
        .mode  = node->mode,
        .type  = node->type,
        .rdev  = node->rdev,
        .size  = node->size,
        .blksz = node->blksz,
    };
    linux_statx_t stx;
    fill_linux_statx(&stx, proc->uid, proc->gid, &src);
    vfs_close(node);
    return copy_to_user((void *)ubuf, &stx, sizeof(stx)) ? -EFAULT : EOK;
}

static const char *path_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static vfs_node_t vfs_open_parent_of(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return vfs_open("/");
    *slash = '\0';
    return vfs_open(path);
}

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

static int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!req) return -EFAULT;

    linux_timespec_t ts;
    if (copy_from_user(&ts, (const void *)req, sizeof(ts))) return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) return -EINVAL;

    uint64_t ticks = (uint64_t)ts.tv_sec * 100 + (uint64_t)((ts.tv_nsec + 9999999LL) / 10000000LL);
    if (!ticks && (ts.tv_sec || ts.tv_nsec)) ticks = 1;
    task_sleep_ticks(ticks);

    if (rem) {
        linux_timespec_t zero = {0, 0};
        if (copy_to_user((void *)rem, &zero, sizeof(zero))) return -EFAULT;
    }
    return 0;
}

static int64_t sys_wait4(uint64_t pid, uint64_t exit_code, uint64_t options, uint64_t rusage, uint64_t arg4, uint64_t arg5)
{
    (void)options;
    (void)rusage;
    (void)arg4;
    (void)arg5;

    int status = 0;
    int ret    = process_wait((pid_t)pid, &status);
    if (ret) return -ECHILD;
    if (exit_code && copy_to_user((void *)exit_code, &status, sizeof(status))) return -EFAULT;
    return (int64_t)pid;
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

static int64_t sys_brk(uint64_t addr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return 0;
    if (!addr) return (int64_t)proc->heap_brk;
    if (addr < PROCESS_HEAP_START || addr > PROCESS_HEAP_MAX) return (int64_t)proc->heap_brk;

    if (addr > proc->heap_brk) {
        uintptr_t start = ALIGN_UP(proc->heap_brk, PAGE_4K_SIZE);
        uintptr_t end   = ALIGN_UP(addr, PAGE_4K_SIZE);
        if (end > start && process_mmap(proc, start, end - start, VM_READ | VM_WRITE)) return (int64_t)proc->heap_brk;
    }
    proc->heap_brk = addr;
    return (int64_t)proc->heap_brk;
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

static int64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd;
    (void)arg4;
    (void)arg5;
    return sys_open(path, flags, mode, 0, 0, 0);
}

static int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (first > last) return -EINVAL;

    int status = 0;
    for (uint64_t fd = first; fd <= last && fd < PROCESS_MAX_FD; fd++) {
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

static int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (flags) return -EINVAL;
    return sys_dup2(oldfd, newfd, 0, 0, 0, 0);
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

static int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)timeout;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!fds && nfds) return -EFAULT;

    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        linux_pollfd_t pfd;
        if (copy_from_user(&pfd, (const void *)(fds + i * sizeof(pfd)), sizeof(pfd))) return -EFAULT;
        pfd.revents = 0;
        if (pfd.fd >= 0) {
            int ret = process_fd_poll(proc, pfd.fd, (size_t)pfd.events);
            if (ret < 0) {
                pfd.revents = 0x0020;
            } else {
                pfd.revents = (int16_t)ret;
                if (pfd.revents) ready++;
            }
        }
        if (copy_to_user((void *)(fds + i * sizeof(pfd)), &pfd, sizeof(pfd))) return -EFAULT;
    }
    return ready;
}

static int64_t sys_fstat(uint64_t fd, uint64_t statbuf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!statbuf) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_fd_stat_t fdst;
    int               ret = process_fd_stat(proc, (int)fd, &fdst);
    if (ret != EOK) return ret;

    linux_stat_t st;
    fill_linux_stat(&st, proc->uid, proc->gid, &fdst);
    return copy_to_user((void *)statbuf, &st, sizeof(st)) ? -EFAULT : EOK;
}

static int64_t sys_statx(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mask, uint64_t statbuf, uint64_t arg5)
{
    (void)dirfd;
    (void)flags;
    (void)mask;
    (void)arg5;
    return statx_path_to_user(path, statbuf);
}

static int64_t sys_gettimeofday(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tz;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tv) return 0;

    linux_timeval_t now = {
        .tv_sec  = (int64_t)(sched_ticks() / 100),
        .tv_usec = (int64_t)((sched_ticks() % 100) * 10000),
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
    int64_t now = (int64_t)(sched_ticks() / 100);
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
    return sys_exit(status, 0, 0, 0, 0, 0);
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
    (void)dirfd;
    (void)flags;
    (void)arg4;
    (void)arg5;
    return stat_path_to_user(path, statbuf);
}

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
    return proc && proc->parent && proc->parent->task ? (int64_t)proc->parent->task->pid : 0;
}

static int64_t sys_gettid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    return sys_getpid(arg0, arg1, arg2, arg3, arg4, arg5);
}

static int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)mode;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(path, name);
    return ret != EOK ? ret : vfs_mkdir(name);
}

static int64_t sys_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd;
    return sys_mkdir(path, mode, arg3, arg4, arg5, 0);
}

static int64_t sys_unlink(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(path, name);
    if (ret != EOK) return ret;
    vfs_node_t node = vfs_open(name);
    if (!node) return -ENOENT;
    ret = vfs_delete(node);
    vfs_close(node);
    return ret;
}

static int64_t sys_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (flags & ~AT_REMOVEDIR) return -EINVAL;
    return sys_unlink(path, 0, 0, 0, 0, 0);
}

static int64_t sys_rename(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    char oldname[SYSCALL_PATH_MAX];
    char newname[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(oldpath, oldname);
    if (ret != EOK) return ret;
    ret = copy_path_from_user(newpath, newname);
    if (ret != EOK) return ret;
    vfs_node_t node = vfs_open(oldname);
    if (!node) return -ENOENT;
    ret = vfs_rename(node, path_basename(newname));
    vfs_close(node);
    return ret;
}

static int64_t sys_renameat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t arg4, uint64_t arg5)
{
    (void)olddirfd;
    (void)newdirfd;
    (void)arg4;
    (void)arg5;
    return sys_rename(oldpath, newpath, 0, 0, 0, 0);
}

static int64_t sys_link(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    char oldname[SYSCALL_PATH_MAX];
    char newname[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(oldpath, oldname);
    if (ret != EOK) return ret;
    ret = copy_path_from_user(newpath, newname);
    if (ret != EOK) return ret;
    return vfs_link(newname, oldname);
}

static int64_t sys_linkat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)olddirfd;
    (void)newdirfd;
    (void)flags;
    (void)arg5;
    return sys_link(oldpath, newpath, 0, 0, 0, 0);
}

static int64_t sys_symlink(uint64_t target, uint64_t linkpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    char target_name[SYSCALL_PATH_MAX];
    char link_name[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(target, target_name);
    if (ret != EOK) return ret;
    ret = copy_path_from_user(linkpath, link_name);
    if (ret != EOK) return ret;
    return vfs_symlink(link_name, target_name);
}

static int64_t sys_symlinkat(uint64_t target, uint64_t newdirfd, uint64_t linkpath, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)newdirfd;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_symlink(target, linkpath, 0, 0, 0, 0);
}

static int64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!buf && bufsiz) return -EFAULT;
    char name[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user(path, name);
    if (ret != EOK) return ret;

    char parent_path[SYSCALL_PATH_MAX];
    memcpy(parent_path, name, sizeof(parent_path));
    vfs_node_t parent = vfs_open_parent_of(parent_path);
    if (!parent) return -ENOENT;
    vfs_node_t node = vfs_do_search(parent, path_basename(name));
    vfs_close(parent);
    if (!node) return -ENOENT;
    char   tmp[SYSCALL_PATH_MAX];
    size_t len = vfs_readlink(node, tmp, sizeof(tmp));
    if (len > bufsiz) len = bufsiz;
    if (len && copy_to_user((void *)buf, tmp, len)) return -EFAULT;
    return (int64_t)len;
}

static int64_t sys_readlinkat(uint64_t dirfd, uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd;
    (void)arg4;
    (void)arg5;
    return sys_readlink(path, buf, bufsiz, 0, 0, 0);
}

static int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    static const char cwd[] = "/";
    if (!buf) return -EFAULT;
    if (size < sizeof(cwd)) return -ERANGE;
    return copy_to_user((void *)buf, cwd, sizeof(cwd)) ? -EFAULT : (int64_t)sizeof(cwd);
}

typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static syscall_fn_t syscall_table[SYS_MAX] = {
    [SYS_READ]         = sys_read,
    [SYS_WRITE]        = sys_write,
    [SYS_OPEN]         = sys_open,
    [SYS_CLOSE]        = sys_close,
    [SYS_STAT]         = sys_stat,
    [SYS_FSTAT]        = sys_fstat,
    [SYS_LSTAT]        = sys_stat,
    [SYS_POLL]         = sys_poll,
    [SYS_LSEEK]        = sys_lseek,
    [SYS_MMAP]         = sys_mmap,
    [SYS_MUNMAP]       = sys_munmap,
    [SYS_BRK]          = sys_brk,
    [SYS_IOCTL]        = sys_ioctl,
    [SYS_DUP]          = sys_dup,
    [SYS_DUP2]         = sys_dup2,
    [SYS_NANOSLEEP]    = sys_nanosleep,
    [SYS_GETPID]       = sys_getpid,
    [SYS_CLONE]        = NULL,
    [SYS_FORK]         = NULL,
    [SYS_VFORK]        = NULL,
    [SYS_EXIT]         = sys_exit,
    [SYS_WAIT4]        = sys_wait4,
    [SYS_KILL]         = sys_kill,
    [SYS_UNAME]        = sys_uname,
    [SYS_CREAT]        = sys_creat,
    [SYS_RENAME]       = sys_rename,
    [SYS_MKDIR]        = sys_mkdir,
    [SYS_LINK]         = sys_link,
    [SYS_UNLINK]       = sys_unlink,
    [SYS_SYMLINK]      = sys_symlink,
    [SYS_READLINK]     = sys_readlink,
    [SYS_GETCWD]       = sys_getcwd,
    [SYS_GETTIMEOFDAY] = sys_gettimeofday,
    [SYS_GETUID]       = sys_getuid,
    [SYS_GETGID]       = sys_getgid,
    [SYS_GETEUID]      = sys_getuid,
    [SYS_GETEGID]      = sys_getgid,
    [SYS_GETPPID]      = sys_getppid,
    [SYS_SCHED_YIELD]  = sys_sched_yield,
    [SYS_GETTID]       = sys_gettid,
    [SYS_TIME]         = sys_time,
    [SYS_EXIT_GROUP]   = sys_exit_group,
    [SYS_OPENAT]       = sys_openat,
    [SYS_MKDIRAT]      = sys_mkdirat,
    [SYS_NEWFSTATAT]   = sys_newfstatat,
    [SYS_UNLINKAT]     = sys_unlinkat,
    [SYS_RENAMEAT]     = sys_renameat,
    [SYS_LINKAT]       = sys_linkat,
    [SYS_SYMLINKAT]    = sys_symlinkat,
    [SYS_READLINKAT]   = sys_readlinkat,
    [SYS_DUP3]         = sys_dup3,
    [SYS_STATX]        = sys_statx,
    [SYS_CLOSE_RANGE]  = sys_close_range,
};

void syscall_dispatch(syscall_frame_t *frame)
{
    uint64_t num = frame->rax;

    if (num == SYS_FORK || num == SYS_VFORK || num == SYS_CLONE) {
        if (num == SYS_CLONE && (frame->rdi != SIGCHLD || frame->rsi)) {
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
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
