/*
 *
 *      syscall.c
 *      System call dispatch
 *
 *      2026/7/20 By Rainy101112
 *      2026/7/21 By JiTianYu391 - Extended with eventfd, timerfd, signalfd, mmap, 100+ syscalls
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <eventfd.h>
#include <interrupt.h>
#include <mmap.h>
#include <printk.h>
#include <process.h>
#include <sched.h>
#include <signal.h>
#include <signalfd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <syscall_table.h>
#include <task.h>
#include <timerfd.h>
#include <uaccess.h>
#include <uinxed.h>
#include <vfs.h>

#define SYSCALL_PATH_MAX  256
#define SYSCALL_IO_CHUNK  4096
#define AT_FDCWD          -100
#define AT_REMOVEDIR      0x200
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

static int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
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

/* ---------- eventfd, timerfd, signalfd wrappers ---------- */

static int64_t sys_eventfd_wrap(uint64_t initval, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_eventfd((unsigned int)initval, 0);
}

static int64_t sys_eventfd2_wrap(uint64_t initval, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_eventfd2((unsigned int)initval, (int)flags);
}

static int64_t sys_timerfd_create_wrap(uint64_t clockid, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_timerfd_create((int)clockid, (int)flags);
}

static int64_t sys_timerfd_settime_wrap(uint64_t fd, uint64_t flags, uint64_t new_value, uint64_t old_value, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;(void)arg5;
    return sys_timerfd_settime((int)fd, (int)flags, (const void *)new_value, (void *)old_value);
}

static int64_t sys_timerfd_gettime_wrap(uint64_t fd, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_timerfd_gettime((int)fd, (void *)curr_value);
}

static int64_t sys_signalfd_wrap(uint64_t fd, uint64_t mask, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    return sys_signalfd((int)fd, (const void *)mask, (int)flags);
}

static int64_t sys_signalfd4_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;(void)arg5;
    return sys_signalfd4((int)fd, (const void *)mask, (size_t)sizemask, (int)flags);
}

/* ---------- Extended syscall stubs ---------- */

static int64_t sys_stub(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return -ENOSYS;
}

static int64_t sys_stub_ok(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return EOK;
}

static int64_t sys_access_stub(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)mode;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    char name[SYSCALL_PATH_MAX];
    if (copy_path_from_user(path, name) != EOK) return -EFAULT;
    vfs_node_t node = vfs_open(name);
    if (!node) return -ENOENT;
    vfs_close(node);
    return EOK;
}

static int64_t sys_chdir_stub(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    char name[SYSCALL_PATH_MAX];
    if (copy_path_from_user(path, name) != EOK) return -EFAULT;
    vfs_node_t node = vfs_open(name);
    if (!node) return -ENOENT;
    if (!(node->type & file_dir)) { vfs_close(node); return -ENOTDIR; }
    vfs_close(node);
    return EOK;
}

static int64_t sys_truncate_stub(uint64_t path, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    char name[SYSCALL_PATH_MAX];
    if (copy_path_from_user(path, name) != EOK) return -EFAULT;
    vfs_node_t node = vfs_open(name);
    if (!node) return -ENOENT;
    vfs_update(node);
    node->size = length;
    vfs_close(node);
    return EOK;
}

static int64_t sys_ftruncate_stub(uint64_t fd, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = NULL;
    spin_lock(&proc->fd_lock);
    if ((int)fd >= 0 && (int)fd < PROCESS_MAX_FD) {
        file = proc->fds[(int)fd];
        if (file) { spin_lock(&file->lock); file->refcount++; spin_unlock(&file->lock); }
    }
    spin_unlock(&proc->fd_lock);
    if (!file) return -EBADF;
    vfs_update(file->node);
    file->node->size = length;
    process_file_put(file);
    return EOK;
}

static int64_t sys_clock_gettime_stub(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)clockid;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    if (!tp) return -EFAULT;
    linux_timespec_t ts = {
        .tv_sec  = (int64_t)(sched_ticks() / 100),
        .tv_nsec = (int64_t)((sched_ticks() % 100) * 10000000LL),
    };
    return copy_to_user((void *)tp, &ts, sizeof(ts)) ? -EFAULT : EOK;
}

static int64_t sys_clock_getres_stub(uint64_t clockid, uint64_t res, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)clockid;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    if (!res) return -EFAULT;
    linux_timespec_t ts = {0, 10000000};
    return copy_to_user((void *)res, &ts, sizeof(ts)) ? -EFAULT : EOK;
}

static int64_t sys_clock_nanosleep_stub(uint64_t clockid, uint64_t flags, uint64_t req, uint64_t rem, uint64_t arg4, uint64_t arg5)
{
    (void)clockid;(void)flags;(void)arg4;(void)arg5;
    return sys_nanosleep(req, rem, 0, 0, 0, 0);
}

static int64_t sys_getrandom_stub(uint64_t buf, uint64_t buflen, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;(void)arg3;(void)arg4;(void)arg5;
    if (!buf && buflen) return -EFAULT;
    for (uint64_t i = 0; i < buflen; i++) {
        uint8_t val = (uint8_t)(sched_ticks() ^ (i * 1103515245 + 12345));
        if (copy_to_user((void *)(buf + i), &val, 1)) return i ? (int64_t)i : -EFAULT;
    }
    return (int64_t)buflen;
}

static int64_t sys_getcpu_stub(uint64_t cpu, uint64_t node, uint64_t tcache, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tcache;(void)arg3;(void)arg4;(void)arg5;
    task_t *task = current_task();
    uint32_t c = task ? task->cpu_id : 0;
    if (cpu && copy_to_user((void *)cpu, &c, sizeof(c))) return -EFAULT;
    if (node) { uint32_t n = 0; if (copy_to_user((void *)node, &n, sizeof(n))) return -EFAULT; }
    return EOK;
}

static int64_t sys_setuid_stub(uint64_t uid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    proc->uid = (uint32_t)uid;
    return EOK;
}

static int64_t sys_setgid_stub(uint64_t gid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    proc->gid = (uint32_t)gid;
    return EOK;
}

static int64_t sys_getresuid_stub(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    uint32_t uid = (uint32_t)sys_getuid(0,0,0,0,0,0);
    if (ruid) copy_to_user((void *)ruid, &uid, sizeof(uid));
    if (euid) copy_to_user((void *)euid, &uid, sizeof(uid));
    if (suid) copy_to_user((void *)suid, &uid, sizeof(uid));
    return EOK;
}

static int64_t sys_getresgid_stub(uint64_t rgid, uint64_t egid, uint64_t sgid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    uint32_t gid = (uint32_t)sys_getgid(0,0,0,0,0,0);
    if (rgid) copy_to_user((void *)rgid, &gid, sizeof(gid));
    if (egid) copy_to_user((void *)egid, &gid, sizeof(gid));
    if (sgid) copy_to_user((void *)sgid, &gid, sizeof(gid));
    return EOK;
}

static int64_t sys_getrlimit_stub(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)resource;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    if (!rlim) return -EFAULT;
    struct { uint64_t rlim_cur; uint64_t rlim_max; } rl = { (uint64_t)-1, (uint64_t)-1 };
    return copy_to_user((void *)rlim, &rl, sizeof(rl)) ? -EFAULT : EOK;
}

static int64_t sys_times_stub(uint64_t tms, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    if (!tms) return -EFAULT;
    uint64_t ticks = sched_ticks();
    if (copy_to_user((void *)tms, &ticks, sizeof(ticks))) return -EFAULT;
    return (int64_t)ticks;
}

static int64_t sys_tkill_stub(uint64_t tid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_tkill_impl((pid_t)tid, (int)sig);
}

static int64_t sys_set_tid_address_stub(uint64_t tidptr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tidptr;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return (int64_t)sys_getpid(0,0,0,0,0,0);
}

static int64_t sys_sched_get_priority_max_stub(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)policy;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return 99;
}

static int64_t sys_sched_get_priority_min_stub(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)policy;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return 1;
}

static int64_t sys_sched_rr_get_interval_stub(uint64_t pid, uint64_t interval, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    if (interval) {
        linux_timespec_t ts = {0, 10000000};
        if (copy_to_user((void *)interval, &ts, sizeof(ts))) return -EFAULT;
    }
    return EOK;
}

static int64_t sys_sched_getaffinity_stub(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;(void)cpusetsize;(void)arg3;(void)arg4;(void)arg5;
    if (mask) {
        uint64_t m = 1;
        if (copy_to_user((void *)mask, &m, sizeof(m))) return -EFAULT;
    }
    return EOK;
}

static int64_t sys_umask_stub(uint64_t mask, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)mask;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return 022;
}

static int64_t sys_select_stub(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t arg5)
{
    (void)exceptfds;(void)arg5;
    if (!readfds && !writefds) {
        if (timeout) {
            linux_timeval_t tv;
            if (copy_from_user(&tv, (const void *)timeout, sizeof(tv))) return -EFAULT;
            uint64_t ticks = (uint64_t)tv.tv_sec * 100 + (uint64_t)((tv.tv_usec + 9999) / 10000);
            if (ticks) task_sleep_ticks(ticks);
        }
        return 0;
    }
    return sys_poll(readfds, nfds, timeout, 0, 0, 0);
}

static int64_t sys_pread64_stub(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;(void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!buf && count) return -EFAULT;
    process_file_t *file = NULL;
    spin_lock(&proc->fd_lock);
    if ((int)fd >= 0 && (int)fd < PROCESS_MAX_FD) {
        file = proc->fds[(int)fd];
        if (file) { spin_lock(&file->lock); file->refcount++; spin_unlock(&file->lock); }
    }
    spin_unlock(&proc->fd_lock);
    if (!file) return -EBADF;
    uint8_t tmp[SYSCALL_IO_CHUNK];
    size_t done = 0;
    while (done < count) {
        size_t chunk = (count - done) < sizeof(tmp) ? (count - done) : sizeof(tmp);
        size_t ret = vfs_read(file->node, tmp, offset + done, chunk);
        if (ret == (size_t)-1) { process_file_put(file); return done ? (int64_t)done : -EIO; }
        if (!ret) break;
        if (copy_to_user((void *)(buf + done), tmp, ret)) { process_file_put(file); return -EFAULT; }
        done += ret;
        if (ret < chunk) break;
    }
    process_file_put(file);
    return (int64_t)done;
}

static int64_t sys_utimensat_stub(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd;(void)path;(void)times;(void)flags;(void)arg4;(void)arg5;
    return EOK;
}

static int64_t sys_sync_stub(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return EOK;
}

static int64_t sys_prlimit64_stub(uint64_t pid, uint64_t resource, uint64_t new_rlim, uint64_t old_rlim, uint64_t arg4, uint64_t arg5)
{
    (void)pid;(void)resource;(void)new_rlim;(void)arg4;(void)arg5;
    return sys_getrlimit_stub(0, old_rlim, 0, 0, 0, 0);
}

static int64_t sys_epoll_pwait_stub(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize)
{
    (void)epfd;(void)events;(void)maxevents;(void)timeout;(void)sigmask;(void)sigsetsize;
    return -ENOSYS;
}

static int64_t sys_readahead_stub(uint64_t fd, uint64_t offset, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)fd;(void)offset;(void)count;(void)arg3;(void)arg4;(void)arg5;
    return EOK;
}

static int64_t sys_listxattr_stub(uint64_t path, uint64_t list, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)path;(void)list;(void)size;(void)arg3;(void)arg4;(void)arg5;
    return 0;
}

static int64_t sys_fadvise64_stub(uint64_t fd, uint64_t offset, uint64_t len, uint64_t advice, uint64_t arg4, uint64_t arg5)
{
    (void)fd;(void)offset;(void)len;(void)advice;(void)arg4;(void)arg5;
    return EOK;
}

static int64_t sys_fallocate_stub(uint64_t fd, uint64_t mode, uint64_t offset, uint64_t len, uint64_t arg4, uint64_t arg5)
{
    (void)fd;(void)mode;(void)offset;(void)len;(void)arg4;(void)arg5;
    return EOK;
}

static int64_t sys_sync_file_range_stub(uint64_t fd, uint64_t offset, uint64_t nbytes, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)fd;(void)offset;(void)nbytes;(void)flags;(void)arg4;(void)arg5;
    return EOK;
}

static int64_t sys_memfd_create_stub(uint64_t name, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)name;(void)flags;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return -ENOSYS;
}

static int64_t sys_renameat2_stub(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)flags;(void)arg5;
    return sys_renameat(olddirfd, oldpath, newdirfd, newpath, 0, 0);
}

static int64_t sys_execveat_stub(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, uint64_t arg5)
{
    (void)dirfd;(void)path;(void)argv;(void)envp;(void)flags;(void)arg5;
    return -ENOSYS;
}

static int64_t sys_membarrier_stub(uint64_t cmd, uint64_t flags, uint64_t cpu_id, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)cmd;(void)flags;(void)cpu_id;(void)arg3;(void)arg4;(void)arg5;
    return -ENOSYS;
}

static int64_t sys_copy_file_range_stub(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out, uint64_t len, uint64_t flags)
{
    (void)fd_in;(void)off_in;(void)fd_out;(void)off_out;(void)len;(void)flags;
    return -ENOSYS;
}

static int64_t sys_mlock2_stub(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;(void)arg3;(void)arg4;(void)arg5;
    return sys_mlock(addr, length);
}

static int64_t sys_pkey_mprotect_stub(uint64_t addr, uint64_t len, uint64_t prot, uint64_t pkey, uint64_t arg4, uint64_t arg5)
{
    (void)pkey;(void)arg4;(void)arg5;
    return sys_mprotect(addr, len, prot);
}

static int64_t sys_pselect6_stub(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t sigmask)
{
    (void)exceptfds;(void)sigmask;
    return sys_select_stub(nfds, readfds, writefds, 0, timeout, 0);
}

static int64_t sys_ppoll_stub(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize, uint64_t arg5)
{
    (void)sigmask;(void)sigsetsize;(void)arg5;
    return sys_poll(fds, nfds, timeout, 0, 0, 0);
}

/* ---------- mmap family wrappers (6-arg syscall -> actual function) ---------- */

static int64_t sys_mprotect_wrap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    return sys_mprotect(addr, length, prot);
}

static int64_t sys_mremap_wrap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr, uint64_t arg5)
{
    (void)arg5;
    return sys_mremap(old_addr, old_len, new_len, flags, new_addr);
}

static int64_t sys_msync_wrap(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    return sys_msync(addr, length, flags);
}

static int64_t sys_mincore_wrap(uint64_t addr, uint64_t length, uint64_t vec, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    return sys_mincore(addr, length, vec);
}

static int64_t sys_madvise_wrap(uint64_t addr, uint64_t length, uint64_t advice, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    return sys_madvise(addr, length, advice);
}

static int64_t sys_mlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_mlock(addr, length);
}

static int64_t sys_munlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_munlock(addr, length);
}

static int64_t sys_mlockall_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_mlockall(flags);
}

static int64_t sys_munlockall_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_munlockall();
}

/* ---------- Signal syscall wrappers ---------- */

static int64_t sys_rt_sigaction_wrap(uint64_t sig, uint64_t act, uint64_t oact, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;(void)arg5;
    return sys_rt_sigaction((int)sig, (const sigaction_t *)act, (sigaction_t *)oact, (size_t)sigsetsize);
}

static int64_t sys_rt_sigprocmask_wrap(uint64_t how, uint64_t set, uint64_t oset, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;(void)arg5;
    return sys_rt_sigprocmask((int)how, (const sigset_t *)set, (sigset_t *)oset, (size_t)sigsetsize);
}

static int64_t sys_rt_sigreturn_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_rt_sigreturn();
}

static int64_t sys_rt_sigpending_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_rt_sigpending((sigset_t *)set, (size_t)sigsetsize);
}

static int64_t sys_rt_sigtimedwait_wrap(uint64_t set, uint64_t info, uint64_t timeout, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;(void)arg5;
    return sys_rt_sigtimedwait((const sigset_t *)set, (siginfo_t *)info, (const void *)timeout, (size_t)sigsetsize);
}

static int64_t sys_rt_sigqueueinfo_wrap(uint64_t pid, uint64_t sig, uint64_t info, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    return sys_rt_sigqueueinfo((pid_t)pid, (int)sig, (siginfo_t *)info);
}

static int64_t sys_rt_sigsuspend_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_rt_sigsuspend((const sigset_t *)set, (size_t)sigsetsize);
}

static int64_t sys_sigaltstack_wrap(uint64_t ss, uint64_t oss, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_sigaltstack((const stack_t *)ss, (stack_t *)oss);
}

static int64_t sys_pause_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_pause();
}

static int64_t sys_tgkill_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;(void)arg4;(void)arg5;
    return sys_tgkill((pid_t)tgid, (pid_t)tid, (int)sig);
}

static int64_t sys_rt_tgsigqueueinfo_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t info, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;(void)arg5;
    return sys_rt_tgsigqueueinfo((pid_t)tgid, (pid_t)tid, (int)sig, (siginfo_t *)info);
}

static int64_t sys_setpgid_wrap(uint64_t pid, uint64_t pgid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_setpgid((pid_t)pid, (pid_t)pgid);
}

static int64_t sys_getpgrp_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_getpgrp();
}

static int64_t sys_setsid_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;(void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_setsid();
}

static int64_t sys_getsid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_getsid((pid_t)pid);
}

static int64_t sys_getpgid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;(void)arg2;(void)arg3;(void)arg4;(void)arg5;
    return sys_getpgid((pid_t)pid);
}

typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static syscall_fn_t syscall_table[SYS_MAX] = {
    [SYS_READ]                  = sys_read,
    [SYS_WRITE]                 = sys_write,
    [SYS_OPEN]                  = sys_open,
    [SYS_CLOSE]                 = sys_close,
    [SYS_STAT]                  = sys_stat,
    [SYS_FSTAT]                 = sys_fstat,
    [SYS_LSTAT]                 = sys_stat,
    [SYS_POLL]                  = sys_poll,
    [SYS_LSEEK]                 = sys_lseek,
    [SYS_MMAP]                  = sys_mmap,
    [SYS_MPROTECT]              = sys_mprotect_wrap,
    [SYS_MUNMAP]                = sys_munmap,
    [SYS_BRK]                   = sys_brk,
    [SYS_RT_SIGACTION]          = sys_rt_sigaction_wrap,
    [SYS_RT_SIGPROCMASK]        = sys_rt_sigprocmask_wrap,
    [SYS_RT_SIGRETURN]          = sys_rt_sigreturn_wrap,
    [SYS_IOCTL]                 = sys_ioctl,
    [SYS_PREAD64]               = sys_pread64_stub,
    [SYS_PWRITE64]              = sys_stub,
    [SYS_READV]                 = sys_stub,
    [SYS_WRITEV]                = sys_stub,
    [SYS_ACCESS]                = sys_access_stub,
    [SYS_PIPE]                  = sys_stub,
    [SYS_SELECT]                = sys_select_stub,
    [SYS_SCHED_YIELD]           = sys_sched_yield,
    [SYS_MREMAP]                = sys_mremap_wrap,
    [SYS_MSYNC]                 = sys_msync_wrap,
    [SYS_MINCORE]               = sys_mincore_wrap,
    [SYS_MADVISE]               = sys_madvise_wrap,
    [SYS_SHMGET]                = sys_stub,
    [SYS_SHMAT]                 = sys_stub,
    [SYS_SHMCTL]                = sys_stub,
    [SYS_DUP]                   = sys_dup,
    [SYS_DUP2]                  = sys_dup2,
    [SYS_PAUSE]                 = sys_pause_wrap,
    [SYS_NANOSLEEP]             = sys_nanosleep,
    [SYS_GETITIMER]             = sys_stub_ok,
    [SYS_ALARM]                 = sys_stub_ok,
    [SYS_SETITIMER]             = sys_stub_ok,
    [SYS_GETPID]                = sys_getpid,
    [SYS_SENDFILE]              = sys_stub,
    [SYS_SOCKET]                = sys_stub,
    [SYS_CONNECT]               = sys_stub,
    [SYS_ACCEPT]                = sys_stub,
    [SYS_SENDTO]                = sys_stub,
    [SYS_RECVFROM]              = sys_stub,
    [SYS_SENDMSG]               = sys_stub,
    [SYS_RECVMSG]               = sys_stub,
    [SYS_SHUTDOWN]              = sys_stub,
    [SYS_BIND]                  = sys_stub,
    [SYS_LISTEN]                = sys_stub,
    [SYS_GETSOCKNAME]           = sys_stub,
    [SYS_GETPEERNAME]           = sys_stub,
    [SYS_SOCKETPAIR]            = sys_stub,
    [SYS_SETSOCKOPT]            = sys_stub,
    [SYS_GETSOCKOPT]            = sys_stub,
    [SYS_CLONE]                 = NULL,
    [SYS_FORK]                  = NULL,
    [SYS_VFORK]                 = NULL,
    [SYS_EXECVE]                = sys_stub,
    [SYS_EXIT]                  = sys_exit,
    [SYS_WAIT4]                 = sys_wait4,
    [SYS_KILL]                  = sys_kill,
    [SYS_UNAME]                 = sys_uname,
    [SYS_SEMGET]                = sys_stub,
    [SYS_SEMOP]                 = sys_stub,
    [SYS_SEMCTL]                = sys_stub,
    [SYS_SHMDT]                 = sys_stub,
    [SYS_MSGGET]                = sys_stub,
    [SYS_MSGSND]                = sys_stub,
    [SYS_MSGRCV]                = sys_stub,
    [SYS_MSGCTL]                = sys_stub,
    [SYS_FCNTL]                 = sys_stub_ok,
    [SYS_FLOCK]                 = sys_stub_ok,
    [SYS_FSYNC]                 = sys_stub_ok,
    [SYS_FDATASYNC]             = sys_stub_ok,
    [SYS_TRUNCATE]              = sys_truncate_stub,
    [SYS_FTRUNCATE]             = sys_ftruncate_stub,
    [SYS_GETDENTS]              = sys_stub,
    [SYS_GETCWD]                = sys_getcwd,
    [SYS_CHDIR]                 = sys_chdir_stub,
    [SYS_FCHDIR]                = sys_chdir_stub,
    [SYS_RENAME]                = sys_rename,
    [SYS_MKDIR]                 = sys_mkdir,
    [SYS_RMDIR]                 = sys_unlink,
    [SYS_CREAT]                 = sys_creat,
    [SYS_LINK]                  = sys_link,
    [SYS_UNLINK]                = sys_unlink,
    [SYS_SYMLINK]               = sys_symlink,
    [SYS_READLINK]              = sys_readlink,
    [SYS_CHMOD]                 = sys_stub_ok,
    [SYS_FCHMOD]                = sys_stub_ok,
    [SYS_CHOWN]                 = sys_stub_ok,
    [SYS_FCHOWN]                = sys_stub_ok,
    [SYS_LCHOWN]                = sys_stub_ok,
    [SYS_UMASK]                 = sys_umask_stub,
    [SYS_GETTIMEOFDAY]          = sys_gettimeofday,
    [SYS_GETRLIMIT]             = sys_getrlimit_stub,
    [SYS_GETRUSAGE]             = sys_stub_ok,
    [SYS_SYSINFO]               = sys_stub_ok,
    [SYS_TIMES]                 = sys_times_stub,
    [SYS_PTRACE]                = sys_stub,
    [SYS_GETUID]                = sys_getuid,
    [SYS_SYSLOG]                = sys_stub,
    [SYS_GETGID]                = sys_getgid,
    [SYS_SETUID]                = sys_setuid_stub,
    [SYS_SETGID]                = sys_setgid_stub,
    [SYS_GETEUID]               = sys_getuid,
    [SYS_GETEGID]               = sys_getgid,
    [SYS_SETPGID]               = sys_setpgid_wrap,
    [SYS_GETPPID]               = sys_getppid,
    [SYS_GETPGRP]               = sys_getpgrp_wrap,
    [SYS_SETSID]                = sys_setsid_wrap,
    [SYS_SETREUID]              = sys_setuid_stub,
    [SYS_SETREGID]              = sys_setgid_stub,
    [SYS_GETGROUPS]             = sys_stub_ok,
    [SYS_SETGROUPS]             = sys_stub_ok,
    [SYS_SETRESUID]             = sys_setuid_stub,
    [SYS_GETRESUID]             = sys_getresuid_stub,
    [SYS_SETRESGID]             = sys_setgid_stub,
    [SYS_GETRESGID]             = sys_getresgid_stub,
    [SYS_GETPGID]               = sys_getpgid_wrap,
    [SYS_SETFSUID]              = sys_setuid_stub,
    [SYS_SETFSGID]              = sys_setgid_stub,
    [SYS_GETSID]                = sys_getsid_wrap,
    [SYS_CAPGET]                = sys_stub_ok,
    [SYS_CAPSET]                = sys_stub,
    [SYS_RT_SIGPENDING]         = sys_rt_sigpending_wrap,
    [SYS_RT_SIGTIMEDWAIT]       = sys_rt_sigtimedwait_wrap,
    [SYS_RT_SIGQUEUEINFO]       = sys_rt_sigqueueinfo_wrap,
    [SYS_RT_SIGSUSPEND]         = sys_rt_sigsuspend_wrap,
    [SYS_SIGALTSTACK]           = sys_sigaltstack_wrap,
    [SYS_UTIME]                 = sys_stub_ok,
    [SYS_MKNOD]                 = sys_stub,
    [SYS_USELIB]                = sys_stub,
    [SYS_PERSONALITY]           = sys_stub,
    [SYS_USTAT]                 = sys_stub,
    [SYS_STATFS]                = sys_stub_ok,
    [SYS_FSTATFS]               = sys_stub_ok,
    [SYS_SYSFS]                 = sys_stub,
    [SYS_GETPRIORITY]           = sys_stub_ok,
    [SYS_SETPRIORITY]           = sys_stub_ok,
    [SYS_SCHED_SETPARAM]        = sys_stub_ok,
    [SYS_SCHED_GETPARAM]        = sys_stub_ok,
    [SYS_SCHED_SETSCHEDULER]    = sys_stub_ok,
    [SYS_SCHED_GETSCHEDULER]    = sys_stub_ok,
    [SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max_stub,
    [SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min_stub,
    [SYS_SCHED_RR_GET_INTERVAL]  = sys_sched_rr_get_interval_stub,
    [SYS_MLOCK]                 = sys_mlock_wrap,
    [SYS_MUNLOCK]               = sys_munlock_wrap,
    [SYS_MLOCKALL]              = sys_mlockall_wrap,
    [SYS_MUNLOCKALL]            = sys_munlockall_wrap,
    [SYS_VHANGUP]               = sys_stub,
    [SYS_MODIFY_LDT]            = sys_stub,
    [SYS_PIVOT_ROOT]            = sys_stub,
    [SYS__SYSCTL]               = sys_stub,
    [SYS_PRCTL]                 = sys_stub_ok,
    [SYS_ARCH_PRCTL]            = sys_stub,
    [SYS_ADJTIMEX]              = sys_stub,
    [SYS_SETRLIMIT]             = sys_stub_ok,
    [SYS_CHROOT]                = sys_stub,
    [SYS_SYNC]                  = sys_sync_stub,
    [SYS_ACCT]                  = sys_stub,
    [SYS_SETTIMEOFDAY]          = sys_stub,
    [SYS_MOUNT]                 = sys_stub,
    [SYS_UMOUNT2]               = sys_stub,
    [SYS_SWAPON]                = sys_stub,
    [SYS_SWAPOFF]               = sys_stub,
    [SYS_REBOOT]                = sys_stub,
    [SYS_SETHOSTNAME]           = sys_stub_ok,
    [SYS_SETDOMAINNAME]         = sys_stub_ok,
    [SYS_IOPL]                  = sys_stub,
    [SYS_IOPERM]                = sys_stub,
    [SYS_CREATE_MODULE]         = sys_stub,
    [SYS_INIT_MODULE]           = sys_stub,
    [SYS_DELETE_MODULE]         = sys_stub,
    [SYS_GET_KERNEL_SYMS]       = sys_stub,
    [SYS_QUERY_MODULE]          = sys_stub,
    [SYS_QUOTACTL]              = sys_stub,
    [SYS_NFSSERVCTL]            = sys_stub,
    [SYS_GETPMSG]               = sys_stub,
    [SYS_PUTPMSG]               = sys_stub,
    [SYS_AFS_SYSCALL]           = sys_stub,
    [SYS_TUXCALL]               = sys_stub,
    [SYS_SECURITY]              = sys_stub,
    [SYS_GETTID]                = sys_gettid,
    [SYS_READAHEAD]             = sys_readahead_stub,
    [SYS_SETXATTR]              = sys_stub,
    [SYS_LSETXATTR]             = sys_stub,
    [SYS_FSETXATTR]             = sys_stub,
    [SYS_GETXATTR]              = sys_stub,
    [SYS_LGETXATTR]             = sys_stub,
    [SYS_FGETXATTR]             = sys_stub,
    [SYS_LISTXATTR]             = sys_listxattr_stub,
    [SYS_LLISTXATTR]            = sys_listxattr_stub,
    [SYS_FLISTXATTR]            = sys_listxattr_stub,
    [SYS_REMOVEXATTR]           = sys_stub,
    [SYS_LREMOVEXATTR]          = sys_stub,
    [SYS_FREMOVEXATTR]          = sys_stub,
    [SYS_TKILL]                 = sys_tkill_stub,
    [SYS_TIME]                  = sys_time,
    [SYS_FUTEX]                 = sys_stub,
    [SYS_SCHED_SETAFFINITY]     = sys_stub_ok,
    [SYS_SCHED_GETAFFINITY]     = sys_sched_getaffinity_stub,
    [SYS_SET_THREAD_AREA]       = sys_stub,
    [SYS_IO_SETUP]              = sys_stub,
    [SYS_IO_DESTROY]            = sys_stub,
    [SYS_IO_GETEVENTS]          = sys_stub,
    [SYS_IO_SUBMIT]             = sys_stub,
    [SYS_IO_CANCEL]             = sys_stub,
    [SYS_GET_THREAD_AREA]       = sys_stub,
    [SYS_LOOKUP_DCOOKIE]        = sys_stub,
    [SYS_EPOLL_CREATE]          = sys_stub,
    [SYS_EPOLL_CTL_OLD]         = sys_stub,
    [SYS_EPOLL_WAIT_OLD]        = sys_stub,
    [SYS_REMAP_FILE_PAGES]      = sys_stub,
    [SYS_GETDENTS64]            = sys_stub,
    [SYS_SET_TID_ADDRESS]       = sys_set_tid_address_stub,
    [SYS_RESTART_SYSCALL]       = sys_stub,
    [SYS_SEMTIMEDOP]            = sys_stub,
    [SYS_FADVISE64]             = sys_fadvise64_stub,
    [SYS_TIMER_CREATE]          = sys_stub,
    [SYS_TIMER_SETTIME]         = sys_stub,
    [SYS_TIMER_GETTIME]         = sys_stub,
    [SYS_TIMER_GETOVERRUN]      = sys_stub,
    [SYS_TIMER_DELETE]          = sys_stub,
    [SYS_CLOCK_SETTIME]         = sys_stub,
    [SYS_CLOCK_GETTIME]         = sys_clock_gettime_stub,
    [SYS_CLOCK_GETRES]          = sys_clock_getres_stub,
    [SYS_CLOCK_NANOSLEEP]       = sys_clock_nanosleep_stub,
    [SYS_EXIT_GROUP]            = sys_exit_group,
    [SYS_EPOLL_WAIT]            = sys_stub,
    [SYS_EPOLL_CTL]             = sys_stub,
    [SYS_TGKILL]                = sys_tgkill_wrap,
    [SYS_UTIMES]                = sys_stub_ok,
    [SYS_VSERVER]               = sys_stub,
    [SYS_MBIND]                 = sys_stub,
    [SYS_SET_MEMPOLICY]         = sys_stub,
    [SYS_GET_MEMPOLICY]         = sys_stub,
    [SYS_MQ_OPEN]               = sys_stub,
    [SYS_MQ_UNLINK]             = sys_stub,
    [SYS_MQ_TIMEDSEND]          = sys_stub,
    [SYS_MQ_TIMEDRECEIVE]       = sys_stub,
    [SYS_MQ_NOTIFY]             = sys_stub,
    [SYS_MQ_GETSETATTR]         = sys_stub,
    [SYS_KEXEC_LOAD]            = sys_stub,
    [SYS_WAITID]                = sys_stub,
    [SYS_ADD_KEY]               = sys_stub,
    [SYS_REQUEST_KEY]           = sys_stub,
    [SYS_KEYCTL]                = sys_stub,
    [SYS_IOPRIO_SET]            = sys_stub,
    [SYS_IOPRIO_GET]            = sys_stub,
    [SYS_INOTIFY_INIT]          = sys_stub,
    [SYS_INOTIFY_ADD_WATCH]     = sys_stub,
    [SYS_INOTIFY_RM_WATCH]      = sys_stub,
    [SYS_MIGRATE_PAGES]         = sys_stub,
    [SYS_OPENAT]                = sys_openat,
    [SYS_MKDIRAT]               = sys_mkdirat,
    [SYS_MKNODAT]               = sys_stub,
    [SYS_FCHOWNAT]              = sys_stub_ok,
    [SYS_FUTIMESAT]             = sys_stub_ok,
    [SYS_NEWFSTATAT]            = sys_newfstatat,
    [SYS_UNLINKAT]              = sys_unlinkat,
    [SYS_RENAMEAT]              = sys_renameat,
    [SYS_LINKAT]                = sys_linkat,
    [SYS_SYMLINKAT]             = sys_symlinkat,
    [SYS_READLINKAT]            = sys_readlinkat,
    [SYS_FCHMODAT]              = sys_stub_ok,
    [SYS_FACCESSAT]             = sys_access_stub,
    [SYS_PSELECT6]              = sys_pselect6_stub,
    [SYS_PPOLL]                 = sys_ppoll_stub,
    [SYS_UNSHARE]               = sys_stub,
    [SYS_SET_ROBUST_LIST]       = sys_stub_ok,
    [SYS_GET_ROBUST_LIST]       = sys_stub,
    [SYS_SPLICE]                = sys_stub,
    [SYS_TEE]                   = sys_stub,
    [SYS_SYNC_FILE_RANGE]       = sys_sync_file_range_stub,
    [SYS_VMSPLICE]              = sys_stub,
    [SYS_MOVE_PAGES]            = sys_stub,
    [SYS_UTIMENSAT]             = sys_utimensat_stub,
    [SYS_EPOLL_PWAIT]           = sys_epoll_pwait_stub,
    [SYS_SIGNALFD]              = sys_signalfd_wrap,
    [SYS_TIMERFD_CREATE]        = sys_timerfd_create_wrap,
    [SYS_EVENTFD]               = sys_eventfd_wrap,
    [SYS_FALLOCATE]             = sys_fallocate_stub,
    [SYS_TIMERFD_SETTIME]       = sys_timerfd_settime_wrap,
    [SYS_TIMERFD_GETTIME]       = sys_timerfd_gettime_wrap,
    [SYS_ACCEPT4]               = sys_stub,
    [SYS_SIGNALFD4]             = sys_signalfd4_wrap,
    [SYS_EVENTFD2]              = sys_eventfd2_wrap,
    [SYS_EPOLL_CREATE1]         = sys_stub,
    [SYS_DUP3]                  = sys_dup3,
    [SYS_PIPE2]                 = sys_stub,
    [SYS_INOTIFY_INIT1]         = sys_stub,
    [SYS_PREADV]                = sys_stub,
    [SYS_PWRITEV]               = sys_stub,
    [SYS_RT_TGSIGQUEUEINFO]     = sys_rt_tgsigqueueinfo_wrap,
    [SYS_PERF_EVENT_OPEN]       = sys_stub,
    [SYS_RECVMMSG]              = sys_stub,
    [SYS_FANOTIFY_INIT]         = sys_stub,
    [SYS_FANOTIFY_MARK]         = sys_stub,
    [SYS_PRLIMIT64]             = sys_prlimit64_stub,
    [SYS_NAME_TO_HANDLE_AT]     = sys_stub,
    [SYS_OPEN_BY_HANDLE_AT]     = sys_stub,
    [SYS_CLOCK_ADJTIME]         = sys_stub,
    [SYS_SYNCFS]                = sys_stub_ok,
    [SYS_SENDMMSG]              = sys_stub,
    [SYS_SETNS]                 = sys_stub,
    [SYS_GETCPU]                = sys_getcpu_stub,
    [SYS_PROCESS_VM_READV]      = sys_stub,
    [SYS_PROCESS_VM_WRITEV]     = sys_stub,
    [SYS_KCMP]                  = sys_stub,
    [SYS_FINIT_MODULE]          = sys_stub,
    [SYS_SCHED_SETATTR]         = sys_stub_ok,
    [SYS_SCHED_GETATTR]         = sys_stub_ok,
    [SYS_RENAMEAT2]             = sys_renameat2_stub,
    [SYS_SECCOMP]               = sys_stub,
    [SYS_GETRANDOM]             = sys_getrandom_stub,
    [SYS_MEMFD_CREATE]          = sys_memfd_create_stub,
    [SYS_KEXEC_FILE_LOAD]       = sys_stub,
    [SYS_BPF]                   = sys_stub,
    [SYS_EXECVEAT]              = sys_execveat_stub,
    [SYS_USERFAULTFD]           = sys_stub,
    [SYS_MEMBARRIER]            = sys_membarrier_stub,
    [SYS_MLOCK2]                = sys_mlock2_stub,
    [SYS_COPY_FILE_RANGE]       = sys_copy_file_range_stub,
    [SYS_PREADV2]               = sys_stub,
    [SYS_PWRITEV2]              = sys_stub,
    [SYS_PKEY_MPROTECT]         = sys_pkey_mprotect_stub,
    [SYS_PKEY_ALLOC]            = sys_stub,
    [SYS_PKEY_FREE]             = sys_stub,
    [SYS_STATX]                 = sys_statx,
    [SYS_IO_PGETEVENTS]         = sys_stub,
    [SYS_RSEQ]                  = sys_stub,
    [SYS_PIDFD_SEND_SIGNAL]    = sys_stub,
    [SYS_IO_URING_SETUP]       = sys_stub,
    [SYS_IO_URING_ENTER]       = sys_stub,
    [SYS_IO_URING_REGISTER]    = sys_stub,
    [SYS_OPEN_TREE]            = sys_stub,
    [SYS_MOVE_MOUNT]           = sys_stub,
    [SYS_FSOPEN]               = sys_stub,
    [SYS_FSCONFIG]             = sys_stub,
    [SYS_FSMOUNT]              = sys_stub,
    [SYS_FSPICK]               = sys_stub,
    [SYS_PIDFD_OPEN]           = sys_stub,
    [SYS_CLONE3]               = sys_stub,
    [SYS_CLOSE_RANGE]          = sys_close_range,
    [SYS_FACCESSAT2]           = sys_access_stub,
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
        goto check_signals;
    }

    if (num >= SYS_MAX || !syscall_table[num]) {
        frame->rax = (uint64_t)-ENOSYS;
        goto check_signals;
    }

    frame->rax = (uint64_t)syscall_table[num](frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);

check_signals:
    /*
     * On return to userspace, check for pending signals.
     * If a signal was delivered that interrupted the syscall,
     * the return value should be -EINTR or -ERESTART.
     */
    if (frame->cs & 0x3) {
        /* Only deliver signals when returning to user mode */
        int ret = signal_deliver_if_pending(frame);
        if (ret == 1) {
            /* Process terminated, schedule away */
            task_exit();
        }
    }
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
