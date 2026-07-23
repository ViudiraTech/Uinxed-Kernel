/*
 *
 *      syscall.c
 *      System call dispatch
 *
 *      2026/7/20 By Rainy101112 & JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/tss.h>
#include <chipset/common.h>
#include <fs/vfs.h>
#include <ipc/epoll.h>
#include <ipc/futex.h>
#include <ipc/posix_mq.h>
#include <ipc/socket.h>
#include <ipc/sysv_ipc.h>
#include <kernel/elf_loader.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <syscall/eventfd.h>
#include <syscall/fcntl.h>
#include <syscall/mmap.h>
#include <syscall/signalfd.h>
#include <syscall/syscall.h>
#include <syscall/syscall_table.h>
#include <syscall/timerfd.h>

#define SYSCALL_PATH_MAX  256
#define SYSCALL_IO_CHUNK  4096

/* Mount flag bits stored in vfs_node->flags */
#define MOUNT_FLAG_RDONLY  (1UL << 0)
#define MOUNT_FLAG_NOSUID  (1UL << 1)
#define MOUNT_FLAG_NODEV   (1UL << 2)
#define MOUNT_FLAG_NOEXEC  (1UL << 3)
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

/* Check if the filesystem containing this node is mounted read-only.
 * Walk up to the nearest mount point and inspect its flags. */
static int vfs_mount_is_readonly(vfs_node_t node)
{
    /* Walk up to the mount root */
    vfs_node_t cur = node;
    while (cur) {
        if (cur->is_mount) return (cur->flags & MOUNT_FLAG_RDONLY) != 0;
        if (cur == cur->parent) break;
        cur = cur->parent;
    }
    return 0;
}

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
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 100) return -EINVAL;
    uint64_t ticks = (uint64_t)ts.tv_sec * 100 + (uint64_t)((ts.tv_nsec + 9999999LL) / 10000000LL);
    if (!ticks && (ts.tv_sec || ts.tv_nsec)) ticks = 1;
    task_sleep_ticks(ticks);

    if (rem) {
        linux_timespec_t zero = {0, 0};
        if (copy_to_user((void *)rem, &zero, sizeof(zero))) return -EFAULT;
    }
    return 0;
}

#define WNOHANG    0x00000001
#define WUNTRACED  0x00000002
#define WCONTINUED 0x00000008

static int64_t sys_wait4(uint64_t pid, uint64_t exit_code, uint64_t options, uint64_t rusage, uint64_t arg4, uint64_t arg5)
{
    (void)rusage;
    (void)arg4;
    (void)arg5;

    int flags  = (int)options;
    int status = 0;

    if (flags & WNOHANG) {
        /*
         * Non-blocking poll: check if child is zombie.
         * If zombie, call process_wait which will find it immediately.
         * If not zombie, return 0 without blocking.
         */
        process_t *child = process_find((pid_t)pid);
        process_t *proc  = process_current();

        if (!child || !proc || child->parent != proc) return -ECHILD;
        if (child->task->state != TASK_ZOMBIE) return 0;

        /* Child is zombie - process_wait will return immediately */
    }

    /* Blocking wait */
    int ret = process_wait((pid_t)pid, &status);
    if (ret) return -ECHILD;
    if (exit_code && copy_to_user((void *)exit_code, &status, sizeof(status))) return -EFAULT;
    return (int64_t)pid;
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

    {
        uint32_t access_mask = 0;
        if ((flags & O_ACCMODE) == O_RDONLY || (flags & O_ACCMODE) == O_RDWR) access_mask |= VFS_ACCESS_R;
        if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR) access_mask |= VFS_ACCESS_W;
        if (vfs_access_check(node, access_mask)) {
            vfs_close(node);
            return -EACCES;
        }
    }

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

    /* Check for read-only mount */
    {
        process_file_t *pf = process_fd_get(proc, (int)fd);
        if (pf) {
            int ro = vfs_mount_is_readonly(pf->node);
            process_file_put(pf);
            if (ro) return -EROFS;
        }
    }

    uint8_t tmp[SYSCALL_IO_CHUNK];
    size_t  done = 0;
    while (done < size) {
        size_t chunk = (size - done) < sizeof(tmp) ? (size - done) : sizeof(tmp);
        if (copy_from_user(tmp, (const void *)(buf + done), chunk)) { return done ? (int64_t)done : -EFAULT; }
        int64_t ret = process_fd_write(proc, (int)fd, tmp, chunk);
        if (ret < 0) return done ? (int64_t)done : ret;
        if (!ret) break;
        done += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    return (int64_t)done;
}

static int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    task_t *task = current_task();

    switch (code) {
        case 0x1002 : /* ARCH_SET_FS */
            wrmsr(0xC0000100, addr);
            if (task) task->thread.fs_base = addr;
            return 0;
        case 0x1003 : { /* ARCH_GET_FS */
            uint64_t fs = task ? task->thread.fs_base : 0;
            return copy_to_user((void *)addr, &fs, sizeof(fs));
        }
        case 0x1004 : /* ARCH_SET_GS */
            wrmsr(0xC0000101, addr);
            if (task) task->thread.gs_base = addr;
            return 0;
        case 0x1005 : { /* ARCH_GET_GS */
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
    if (flags) return -EINVAL;
    return sys_dup2(oldfd, newfd, 0, 0, 0, 0);
}

/*
 * Terminal ioctl request codes (Linux-compatible)
 */
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCSCTTY   0x540E
#define TIOCNOTTY   0x5422
#define FIONREAD    0x541B

/* Linux termios structure (x86_64, ~60 bytes) */
struct linux_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

/* Linux winsize structure */
struct linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* Handle terminal ioctls at the syscall level for TTY/console fds */
    switch ((unsigned long)req) {
        case TCGETS: {
            /* Return a minimal cooked terminal termios */
            if (!arg) return -EFAULT;
            struct linux_termios t;
            memset(&t, 0, sizeof(t));
            /* c_iflag: ICRNL | IXON */
            t.c_iflag = 0x0400 | 0x0400;  /* ICRNL */
            /* c_oflag: OPOST | ONLCR */
            t.c_oflag = 0x0001 | 0x0004;  /* OPOST | ONLCR */
            /* c_cflag: CREAD | CS8 | B38400 */
            t.c_cflag = 0x0080 | 0x0030 | 0x000F;  /* CREAD | CS8 | B38400 */
            /* c_lflag: ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE */
            t.c_lflag = 0x0001 | 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0200 | 0x0800;
            t.c_line  = 0;
            /* c_cc: set VEOF=4 (^D), VEOL=0, VERASE=0x7f (BS), VINTR=3 (^C), VKILL=0x15 (^U), VMIN=1, VQUIT=0x1c (^\), VSTART=0x11, VSTOP=0x13, VSUSP=0x1a (^Z), VTIME=0 */
            t.c_cc[0] = 4;    /* VEOF */
            t.c_cc[1] = 0;    /* VEOL */
            t.c_cc[2] = 0x7f; /* VERASE */
            t.c_cc[3] = 3;    /* VINTR */
            t.c_cc[4] = 0x15; /* VKILL */
            t.c_cc[5] = 1;    /* VMIN */
            t.c_cc[6] = 0x1c; /* VQUIT */
            t.c_cc[7] = 0;    /* spare */
            t.c_cc[8] = 0x11; /* VSTART */
            t.c_cc[9] = 0x13; /* VSTOP */
            t.c_cc[10] = 0x1a; /* VSUSP */
            /* VTIME=0 */
            if (copy_to_user((void *)arg, &t, sizeof(t))) return -EFAULT;
            return 0;
        }

        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            /* Accept any terminal settings */
            return 0;

        case TIOCGWINSZ: {
            if (!arg) return -EFAULT;
            struct linux_winsize ws = {80, 25, 0, 0};
            /* Try to get actual size from framebuffer */
            if (copy_to_user((void *)arg, &ws, sizeof(ws))) return -EFAULT;
            return 0;
        }

        case TIOCSWINSZ:
            /* Accept window size change */
            return 0;

        case TIOCGPGRP: {
            /* Return foreground process group */
            if (!arg) return -EFAULT;
            pid_t pgid = proc->pgid ? proc->pgid : proc->task->pid;
            if (copy_to_user((void *)arg, &pgid, sizeof(pgid))) return -EFAULT;
            return 0;
        }

        case TIOCSPGRP: {
            /* Set foreground process group */
            if (!arg) return -EFAULT;
            pid_t pgid;
            if (copy_from_user(&pgid, (const void *)arg, sizeof(pgid))) return -EFAULT;
            proc->pgid = pgid;
            return 0;
        }

        case TIOCSCTTY:
            /* Make this terminal the controlling terminal */
            return 0;

        case TIOCNOTTY:
            /* Release controlling terminal */
            return 0;

        case TCSBRK:
        case TCXONC:
        case TCFLSH:
            /* Flow control / line discipline - accept silently */
            return 0;

        case FIONREAD: {
            /* Return number of bytes available to read */
            /* For now return 0 (nothing available) - this is approximate */
            if (!arg) return -EFAULT;
            int nread = 0;
            if (copy_to_user((void *)arg, &nread, sizeof(nread))) return -EFAULT;
            return 0;
        }

        default:
            /* Delegate to VFS for device-specific ioctls */
            return process_fd_ioctl(proc, (int)fd, (size_t)req, (void *)arg);
    }
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
    if (nfds > 65536) return -EINVAL;

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
    if (!buf) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    const char *cwd = proc->cwd[0] ? proc->cwd : "/";
    size_t      len = strlen(cwd) + 1;
    if (len > size) return -ERANGE;
    return copy_to_user((void *)buf, cwd, len) ? -EFAULT : (int64_t)len;
}

/* ---------- eventfd, timerfd, signalfd wrappers ---------- */

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

static int64_t sys_signalfd_wrap(uint64_t fd, uint64_t mask, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_signalfd((int)fd, (const void *)mask, (int)flags);
}

static int64_t sys_signalfd4_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_signalfd4((int)fd, (const void *)mask, (size_t)sizemask, (int)flags);
}

/* ---------- mount / umount2 ---------- */

#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT    32
#define MS_MANDLOCK   64
#define MS_DIRSYNC   128
#define MS_NOATIME  1024
#define MS_NODIRATIME 2048
#define MS_BIND      4096
#define MS_MOVE      8192
#define MS_REC      16384
#define MS_SILENT   32768

#define MNT_FORCE   1
#define MNT_DETACH  2
#define MNT_EXPIRE  4

static int64_t sys_mount(uint64_t source, uint64_t target, uint64_t fstype,
                         uint64_t flags, uint64_t data, uint64_t arg5)
{
    (void)data;
    (void)arg5;

    char src[SYSCALL_PATH_MAX] = {0};
    char tgt[SYSCALL_PATH_MAX] = {0};
    char fst[SYSCALL_PATH_MAX] = {0};

    if (!target) return -EFAULT;

    /* Copy target path (required) */
    if (strncpy_from_user(tgt, (const char *)target, sizeof(tgt)) < 0) return -EFAULT;

    /* Copy source path (optional — can be NULL for virtual filesystems) */
    if (source) {
        if (strncpy_from_user(src, (const char *)source, sizeof(src)) < 0) return -EFAULT;
    }

    /* Copy filesystem type (optional — can be NULL to let VFS probe) */
    if (fstype) {
        if (strncpy_from_user(fst, (const char *)fstype, sizeof(fst)) < 0) return -EFAULT;
    }

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
        node->flags &= ~(MOUNT_FLAG_RDONLY | MOUNT_FLAG_NOSUID |
                         MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC);
        if (flags & MS_RDONLY)  node->flags |= MOUNT_FLAG_RDONLY;
        if (flags & MS_NOSUID)  node->flags |= MOUNT_FLAG_NOSUID;
        if (flags & MS_NODEV)   node->flags |= MOUNT_FLAG_NODEV;
        if (flags & MS_NOEXEC)  node->flags |= MOUNT_FLAG_NOEXEC;
        vfs_close(node);
        return EOK;
    }

    /* Handle MS_MOVE: move an existing mount to a new location */
    if (flags & MS_MOVE) {
        vfs_close(node);
        return -ENOSYS; /* not yet supported */
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
    node->flags &= ~(MOUNT_FLAG_RDONLY | MOUNT_FLAG_NOSUID |
                     MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC);
    if (flags & MS_RDONLY)  node->flags |= MOUNT_FLAG_RDONLY;
    if (flags & MS_NOSUID)  node->flags |= MOUNT_FLAG_NOSUID;
    if (flags & MS_NODEV)   node->flags |= MOUNT_FLAG_NODEV;
    if (flags & MS_NOEXEC)  node->flags |= MOUNT_FLAG_NOEXEC;

    /* Mark the node as a mount point (if not already) */
    node->is_mount = 1;

    vfs_close(node);

    /* MS_REC: recursive — ignored for non-bind mounts */
    (void)(flags & MS_REC);

    return EOK;
}

static int64_t sys_umount2(uint64_t target, uint64_t flags, uint64_t arg2,
                           uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    char tgt[SYSCALL_PATH_MAX] = {0};

    if (!target) return -EFAULT;
    if (strncpy_from_user(tgt, (const char *)target, sizeof(tgt)) < 0) return -EFAULT;

    /* MNT_FORCE: force unmount even if busy (not fully supported) */
    /* MNT_DETACH: lazy unmount — detach now, cleanup later (not fully supported) */
    if (flags & ~(MNT_FORCE | MNT_DETACH | MNT_EXPIRE)) return -EINVAL;

    return vfs_umount(tgt);
}

/* ---------- Extended syscall stubs ---------- */

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

static int64_t sys_stub_ok(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return EOK;
}

static int64_t sys_access_stub(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    char name[SYSCALL_PATH_MAX];
    if (copy_path_from_user(path, name) != EOK) return -EFAULT;
    vfs_node_t node = vfs_open(name);
    if (!node) return -ENOENT;

    /* R_OK=4, W_OK=2, X_OK=1, F_OK=0 */
    if (mode & 4) { /* R_OK */
        if (vfs_access_check(node, VFS_ACCESS_R)) {
            vfs_close(node);
            return -EACCES;
        }
    }
    if (mode & 2) { /* W_OK */
        if (vfs_access_check(node, VFS_ACCESS_W)) {
            vfs_close(node);
            return -EACCES;
        }
    }
    if (mode & 1) { /* X_OK */
        if (vfs_access_check(node, VFS_ACCESS_X)) {
            vfs_close(node);
            return -EACCES;
        }
    }
    vfs_close(node);
    return EOK;
}

static int64_t sys_chdir_stub(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    char name[SYSCALL_PATH_MAX];
    if (copy_path_from_user(path, name) != EOK) return -EFAULT;
    vfs_node_t node = vfs_open(name);
    if (!node) return -ENOENT;
    if (!(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }
    vfs_close(node);

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    strncpy(proc->cwd, name, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    return EOK;
}

static int64_t sys_fchdir_stub(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
    if (!(file->node->type & file_dir)) {
        process_file_put(file);
        return -ENOTDIR;
    }

    /* Use the node's name as cwd */
    strncpy(proc->cwd, file->node->name, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    process_file_put(file);
    return EOK;
}

static int64_t sys_truncate_stub(uint64_t path, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
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
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *file = NULL;
    spin_lock(&proc->fd_lock);
    if ((int)fd >= 0 && (int)fd < PROCESS_MAX_FD) {
        file = proc->fds[(int)fd];
        if (file) {
            spin_lock(&file->lock);
            file->refcount++;
            spin_unlock(&file->lock);
        }
    }
    spin_unlock(&proc->fd_lock);
    if (!file) return -EBADF;
    vfs_update(file->node);
    file->node->size = length;
    process_file_put(file);
    return EOK;
}

#define CLOCK_REALTIME          0
#define CLOCK_MONOTONIC         1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW     4
#define CLOCK_REALTIME_COARSE   5
#define CLOCK_MONOTONIC_COARSE  6
#define CLOCK_BOOTTIME          7
#define CLOCK_REALTIME_ALARM    8
#define CLOCK_BOOTTIME_ALARM    9
#define CLOCK_TAI              11

static int64_t sys_clock_gettime_stub(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tp) return -EFAULT;

    uint64_t ticks = sched_ticks();
    linux_timespec_t ts;

    switch ((int)clockid) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
        case CLOCK_REALTIME_ALARM:
            /* Wall clock time since epoch */
            ts.tv_sec  = (int64_t)(ticks / 100);
            ts.tv_nsec = (int64_t)((ticks % 100) * 10000000LL);
            break;
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME:
        case CLOCK_BOOTTIME_ALARM:
            /* Monotonic time since boot */
            ts.tv_sec  = (int64_t)(ticks / 100);
            ts.tv_nsec = (int64_t)((ticks % 100) * 10000000LL);
            break;
        case CLOCK_PROCESS_CPUTIME_ID:
        case CLOCK_THREAD_CPUTIME_ID:
            /* CPU time - approximate with elapsed time */
            ts.tv_sec  = (int64_t)(ticks / 100);
            ts.tv_nsec = (int64_t)((ticks % 100) * 10000000LL);
            break;
        case CLOCK_TAI:
            ts.tv_sec  = (int64_t)(ticks / 100);
            ts.tv_nsec = (int64_t)((ticks % 100) * 10000000LL);
            break;
        default:
            return -EINVAL;
    }

    return copy_to_user((void *)tp, &ts, sizeof(ts)) ? -EFAULT : EOK;
}

/* ---------- sysinfo ---------- */

struct linux_sysinfo {
    int64_t uptime;
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
    char _f[20-2*sizeof(uint64_t)-sizeof(uint32_t)];
};

/* ---------- statfs ---------- */

struct linux_statfs {
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
};

#define TMPFS_MAGIC 0x01021994

/* ---------- personality ---------- */

#define PER_LINUX 0x0000

/* ---------- getrusage ---------- */

struct linux_rusage {
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
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN -1

static int64_t sys_getrusage_impl(uint64_t who, uint64_t usage, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!usage) return -EFAULT;
    if ((int)who != RUSAGE_SELF && (int)who != RUSAGE_CHILDREN) return -EINVAL;

    struct linux_rusage ru;
    memset(&ru, 0, sizeof(ru));
    /* Return some approximate usage values */
    uint64_t ticks = sched_ticks();
    ru.ru_utime_sec  = ticks / 100;
    ru.ru_utime_usec = (ticks % 100) * 10000;
    ru.ru_minflt     = 0;
    ru.ru_majflt     = 0;

    if (copy_to_user((void *)usage, &ru, sizeof(ru))) return -EFAULT;
    return 0;
}

/* ---------- restart_syscall ---------- */

static int64_t sys_restart_syscall(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    /* restart_syscall is used by the signal handling path to restart
     * interrupted syscalls. For now return -EINTR since we don't
     * support full syscall restart machinery. */
    return -EINTR;
}

static int64_t sys_personality_impl(uint64_t persona, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    /* If persona != 0xffffffff, set it; always return current personality */
    if ((unsigned int)persona != 0xffffffff) {
        /* Setting personality is not supported, just ignore */
    }
    return PER_LINUX;
}

static int64_t sys_statfs_impl(uint64_t path_or_fd, uint64_t buf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)path_or_fd;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!buf) return -EFAULT;

    struct linux_statfs sf;
    memset(&sf, 0, sizeof(sf));
    sf.f_type    = TMPFS_MAGIC;
    sf.f_bsize   = 4096;
    sf.f_blocks  = 65536;
    sf.f_bfree   = 32768;
    sf.f_bavail  = 32768;
    sf.f_files   = 0;
    sf.f_ffree   = 0;
    sf.f_namelen = 255;
    sf.f_frsize  = 4096;

    if (copy_to_user((void *)buf, &sf, sizeof(sf))) return -EFAULT;
    return 0;
}

static int64_t sys_sysinfo_impl(uint64_t info, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!info) return -EFAULT;

    struct linux_sysinfo si;
    memset(&si, 0, sizeof(si));
    si.uptime   = (int64_t)(sched_ticks() / 100);
    si.procs    = 1;
    si.mem_unit = 1;
    /* Report a generous amount of RAM: 256MB */
    si.totalram = 256 * 1024 * 1024;
    si.freeram  = 128 * 1024 * 1024;
    si.totalswap = 0;
    si.freeswap  = 0;

    if (copy_to_user((void *)info, &si, sizeof(si))) return -EFAULT;
    return 0;
}

static int64_t sys_clock_getres_stub(uint64_t clockid, uint64_t res, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)clockid;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!res) return -EFAULT;
    linux_timespec_t ts = {0, 10000000};
    return copy_to_user((void *)res, &ts, sizeof(ts)) ? -EFAULT : EOK;
}

static int64_t sys_clock_nanosleep_stub(uint64_t clockid, uint64_t flags, uint64_t req, uint64_t rem, uint64_t arg4, uint64_t arg5)
{
    (void)clockid;
    (void)flags;
    (void)arg4;
    (void)arg5;
    return sys_nanosleep(req, rem, 0, 0, 0, 0);
}

static int64_t sys_getrandom_stub(uint64_t buf, uint64_t buflen, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!buf && buflen) return -EFAULT;
    for (uint64_t i = 0; i < buflen; i++) {
        uint8_t val = (uint8_t)(sched_ticks() ^ (i * 1103515245 + 12345));
        if (copy_to_user((void *)(buf + i), &val, 1)) return i ? (int64_t)i : -EFAULT;
    }
    return (int64_t)buflen;
}

static int64_t sys_getcpu_stub(uint64_t cpu, uint64_t node, uint64_t tcache, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tcache;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    task_t  *task = current_task();
    uint32_t c    = task ? task->cpu_id : 0;
    if (cpu && copy_to_user((void *)cpu, &c, sizeof(c))) return -EFAULT;
    if (node) {
        uint32_t n = 0;
        if (copy_to_user((void *)node, &n, sizeof(n))) return -EFAULT;
    }
    return EOK;
}

static int64_t sys_setuid_stub(uint64_t uid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    proc->uid = (uint32_t)uid;
    return EOK;
}

static int64_t sys_setgid_stub(uint64_t gid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    proc->gid = (uint32_t)gid;
    return EOK;
}

static int64_t sys_getresuid_stub(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    uint32_t uid = (uint32_t)sys_getuid(0, 0, 0, 0, 0, 0);
    if (ruid && copy_to_user((void *)ruid, &uid, sizeof(uid))) return -EFAULT;
    if (euid && copy_to_user((void *)euid, &uid, sizeof(uid))) return -EFAULT;
    if (suid && copy_to_user((void *)suid, &uid, sizeof(uid))) return -EFAULT;
    return EOK;
}

static int64_t sys_getresgid_stub(uint64_t rgid, uint64_t egid, uint64_t sgid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    uint32_t gid = (uint32_t)sys_getgid(0, 0, 0, 0, 0, 0);
    if (rgid && copy_to_user((void *)rgid, &gid, sizeof(gid))) return -EFAULT;
    if (egid && copy_to_user((void *)egid, &gid, sizeof(gid))) return -EFAULT;
    if (sgid && copy_to_user((void *)sgid, &gid, sizeof(gid))) return -EFAULT;
    return EOK;
}

static int64_t sys_getrlimit_stub(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)resource;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!rlim) return -EFAULT;
    struct {
            uint64_t rlim_cur;
            uint64_t rlim_max;
    } rl = {(uint64_t)-1, (uint64_t)-1};
    return copy_to_user((void *)rlim, &rl, sizeof(rl)) ? -EFAULT : EOK;
}

static int64_t sys_times_stub(uint64_t tms, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tms) return -EFAULT;
    uint64_t ticks = sched_ticks();
    if (copy_to_user((void *)tms, &ticks, sizeof(ticks))) return -EFAULT;
    return (int64_t)ticks;
}

static int64_t sys_tkill_stub(uint64_t tid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_tkill_impl((pid_t)tid, (int)sig);
}

static int64_t sys_set_tid_address_stub(uint64_t tidptr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* Store the clear_child_tid pointer for use on thread exit */
    proc->clear_child_tid = (int32_t)(tidptr & 0xFFFFFFFF);

    task_t *task = current_task();
    return task ? (int64_t)task->pid : -ESRCH;
}

static int64_t sys_sched_get_priority_max_stub(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)policy;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return 99;
}

static int64_t sys_sched_get_priority_min_stub(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)policy;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return 1;
}

static int64_t sys_sched_rr_get_interval_stub(uint64_t pid, uint64_t interval, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (interval) {
        linux_timespec_t ts = {0, 10000000};
        if (copy_to_user((void *)interval, &ts, sizeof(ts))) return -EFAULT;
    }
    return EOK;
}

static int64_t sys_sched_getaffinity_stub(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)cpusetsize;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (mask) {
        uint64_t m = 1;
        if (copy_to_user((void *)mask, &m, sizeof(m))) return -EFAULT;
    }
    return EOK;
}

static int64_t sys_umask_stub(uint64_t mask, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)mask;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return 022;
}

static int64_t sys_select_stub(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t arg5)
{
    (void)exceptfds;
    (void)arg5;
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
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!buf && count) return -EFAULT;
    process_file_t *file = NULL;
    spin_lock(&proc->fd_lock);
    if ((int)fd >= 0 && (int)fd < PROCESS_MAX_FD) {
        file = proc->fds[(int)fd];
        if (file) {
            spin_lock(&file->lock);
            file->refcount++;
            spin_unlock(&file->lock);
        }
    }
    spin_unlock(&proc->fd_lock);
    if (!file) return -EBADF;
    uint8_t tmp[SYSCALL_IO_CHUNK];
    size_t  done = 0;
    while (done < count) {
        size_t chunk = (count - done) < sizeof(tmp) ? (count - done) : sizeof(tmp);
        size_t ret   = vfs_read(file->node, tmp, offset + done, chunk);
        if (ret == (size_t)-1) {
            process_file_put(file);
            return done ? (int64_t)done : -EIO;
        }
        if (!ret) break;
        if (copy_to_user((void *)(buf + done), tmp, ret)) {
            process_file_put(file);
            return -EFAULT;
        }
        done += ret;
        if (ret < chunk) break;
    }
    process_file_put(file);
    return (int64_t)done;
}

static int64_t sys_pwrite64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!buf && count) return -EFAULT;

    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;

    /* Check for read-only mount */
    if (vfs_mount_is_readonly(file->node)) {
        process_file_put(file);
        return -EROFS;
    }

    uint8_t tmp[SYSCALL_IO_CHUNK];
    size_t  done = 0;
    while (done < count) {
        size_t chunk = (count - done) < sizeof(tmp) ? (count - done) : sizeof(tmp);
        if (copy_from_user(tmp, (const void *)(buf + done), chunk)) {
            process_file_put(file);
            return done ? (int64_t)done : -EFAULT;
        }
        size_t ret = vfs_write(file->node, tmp, offset + done, chunk);
        if (ret == (size_t)-1) {
            process_file_put(file);
            return done ? (int64_t)done : -EIO;
        }
        done += ret;
        if (ret < chunk) break;
    }
    process_file_put(file);
    return (int64_t)done;
}

static int64_t sys_utimensat_stub(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd;
    (void)path;
    (void)times;
    (void)flags;
    (void)arg4;
    (void)arg5;
    return EOK;
}

static int64_t sys_sync_stub(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return EOK;
}

static int64_t sys_prlimit64_stub(uint64_t pid, uint64_t resource, uint64_t new_rlim, uint64_t old_rlim, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)resource;
    (void)new_rlim;
    (void)arg4;
    (void)arg5;
    return sys_getrlimit_stub(0, old_rlim, 0, 0, 0, 0);
}

static int64_t sys_readahead_stub(uint64_t fd, uint64_t offset, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)fd;
    (void)offset;
    (void)count;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return EOK;
}

static int64_t sys_listxattr_stub(uint64_t path, uint64_t list, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)path;
    (void)list;
    (void)size;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return 0;
}

static int64_t sys_fadvise64_stub(uint64_t fd, uint64_t offset, uint64_t len, uint64_t advice, uint64_t arg4, uint64_t arg5)
{
    (void)fd;
    (void)offset;
    (void)len;
    (void)advice;
    (void)arg4;
    (void)arg5;
    return EOK;
}

static int64_t sys_fallocate_stub(uint64_t fd, uint64_t mode, uint64_t offset, uint64_t len, uint64_t arg4, uint64_t arg5)
{
    (void)fd;
    (void)mode;
    (void)offset;
    (void)len;
    (void)arg4;
    (void)arg5;
    return EOK;
}

static int64_t sys_sync_file_range_stub(uint64_t fd, uint64_t offset, uint64_t nbytes, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)fd;
    (void)offset;
    (void)nbytes;
    (void)flags;
    (void)arg4;
    (void)arg5;
    return EOK;
}

static int64_t sys_memfd_create_stub(uint64_t name, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)name;
    (void)flags;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS;
}

static int64_t sys_renameat2_stub(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)flags;
    (void)arg5;
    return sys_renameat(olddirfd, oldpath, newdirfd, newpath, 0, 0);
}

static int64_t sys_execveat_stub(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, uint64_t arg5)
{
    (void)dirfd;
    (void)path;
    (void)argv;
    (void)envp;
    (void)flags;
    (void)arg5;
    return -ENOSYS;
}

static int64_t sys_membarrier_stub(uint64_t cmd, uint64_t flags, uint64_t cpu_id, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)cmd;
    (void)flags;
    (void)cpu_id;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS;
}

static int64_t sys_copy_file_range_stub(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out, uint64_t len, uint64_t flags)
{
    (void)fd_in;
    (void)off_in;
    (void)fd_out;
    (void)off_out;
    (void)len;
    (void)flags;
    return -ENOSYS;
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

static int64_t sys_pselect6_stub(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t sigmask)
{
    (void)exceptfds;
    (void)sigmask;
    return sys_select_stub(nfds, readfds, writefds, 0, timeout, 0);
}

static int64_t sys_ppoll_stub(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize, uint64_t arg5)
{
    (void)sigmask;
    (void)sigsetsize;
    (void)arg5;
    return sys_poll(fds, nfds, timeout, 0, 0, 0);
}

/* ---------- mmap family wrappers (6-arg syscall -> actual function) ---------- */

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

/* ---------- Signal syscall wrappers ---------- */

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

/* ---------- IPC syscall wrappers ---------- */

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
    return sys_bind((int)fd, (const sockaddr_un_t *)addr, (uint32_t)addrlen);
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
    return sys_accept((int)fd, (sockaddr_un_t *)addr, (uint32_t *)addrlen, 0);
}

static int64_t sys_accept4_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_accept((int)fd, (sockaddr_un_t *)addr, (uint32_t *)addrlen, (int)flags);
}

static int64_t sys_connect_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_connect((int)fd, (const sockaddr_un_t *)addr, (uint32_t)addrlen);
}

static int64_t sys_sendto_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    return sys_sendto((int)fd, (const void *)buf, (size_t)len, (int)flags, (const sockaddr_un_t *)addr, (uint32_t)addrlen);
}

static int64_t sys_recvfrom_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    return sys_recvfrom((int)fd, (void *)buf, (size_t)len, (int)flags, (sockaddr_un_t *)addr, (uint32_t *)addrlen);
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
    return sys_getsockname((int)fd, (sockaddr_un_t *)addr, (uint32_t *)addrlen);
}

static int64_t sys_getpeername_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getpeername((int)fd, (sockaddr_un_t *)addr, (uint32_t *)addrlen);
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
extern int64_t sys_pipe(int pipefd[2]);
extern int64_t sys_pipe2(int pipefd[2], int flags);

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

static int64_t sys_mq_timedreceive_wrap(uint64_t mqdes, uint64_t msg_ptr, uint64_t msg_len, uint64_t msg_prio, uint64_t abs_timeout,
                                        uint64_t arg5)
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

/* ---------- waitid wrapper ---------- */

#define P_PID  1
#define P_PGID 2
#define P_ALL  3
#define WEXITED    0x00000004
#define WNOHANG    0x00000001

static int64_t sys_waitid_impl(uint64_t which, uint64_t upid, uint64_t infop, uint64_t options, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    pid_t pid;
    int   flags = (int)options;

    switch ((int)which) {
        case P_PID:
            if (copy_from_user(&pid, (const void *)upid, sizeof(pid))) return -EFAULT;
            break;
        case P_PGID:
        case P_ALL:
            pid = -1; /* Wait for any child */
            break;
        default:
            return -EINVAL;
    }

    /* For now, delegate to wait4 and ignore siginfo_t output */
    int   status = 0;
    int64_t ret;

    if (flags & WNOHANG) {
        process_t *child = process_find(pid);
        process_t *proc  = process_current();
        if (!child || !proc || child->parent != proc) return -ECHILD;
        if (child->task->state != TASK_ZOMBIE) return 0;
    }

    ret = (int64_t)process_wait(pid, &status);
    if (ret) return -ECHILD;

    /* Populate siginfo_t if requested */
    if (infop) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = SIGCHLD;
        info.si_code  = CLD_EXITED;
        info.si_pid   = pid;
        info.si_uid   = 0;
        info.si_status = status;
        if (copy_to_user((void *)infop, &info, sizeof(info))) return -EFAULT;
    }

    return 0;
}

static char **copy_argv_from_user(const char *const *uargv, int *out_count)
{
    *out_count = 0;
    if (!uargv) return NULL;

    /* First pass: count the arguments */
    int   count = 0;
    char *ubuf;
    for (int i = 0;; i++) {
        if (copy_from_user(&ubuf, (void *)&uargv[i], sizeof(char *))) break;
        if (!ubuf) break;
        count++;
    }
    if (count == 0) return NULL;

    /* Allocate kernel array of pointers */
    char **kargv = malloc((count + 1) * sizeof(char *));
    if (!kargv) return NULL;

    /* Second pass: copy each string */
    int i;
    for (i = 0; i < count; i++) {
        char *ustr;
        if (copy_from_user(&ustr, (void *)&uargv[i], sizeof(char *))) {
            kargv[i] = NULL;
            goto fail;
        }

        size_t len = 0;
        char   tmp;
        do {
            if (copy_from_user(&tmp, ustr + len, 1)) {
                kargv[i] = NULL;
                goto fail;
            }
            len++;
        } while (tmp != '\0');

        kargv[i] = malloc(len);
        if (!kargv[i]) goto fail;
        if (copy_from_user(kargv[i], ustr, len)) {
            free(kargv[i]);
            kargv[i] = NULL;
            goto fail;
        }
    }
    kargv[count] = NULL;
    *out_count   = count;
    return kargv;

fail:
    for (int j = 0; j < i; j++)
        if (kargv[j]) free(kargv[j]);
    free(kargv);
    return NULL;
}

static void free_string_array(char **arr)
{
    if (!arr) return;
    for (int i = 0; arr[i]; i++) free(arr[i]);
    free(arr);
}

static int64_t do_execve(const char *path, char *const argv[], char *const envp[])
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char kpath[SYSCALL_PATH_MAX];
    if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0) return -EFAULT;
    kpath[sizeof(kpath) - 1] = '\0';

    int    argc = 0, envc = 0;
    char **kargv = copy_argv_from_user((const char *const *)argv, &argc);
    char **kenvp = copy_argv_from_user((const char *const *)envp, &envc);

    vfs_node_t node = vfs_open(kpath);
    if (!node) {
        free_string_array(kargv);
        free_string_array(kenvp);
        return -ENOENT;
    }

    if (node->size == 0 || node->size > 0x4000000) {
        vfs_close(node);
        free_string_array(kargv);
        free_string_array(kenvp);
        return -ENOEXEC;
    }

    uint8_t *elf_data = malloc(node->size);
    if (!elf_data) {
        vfs_close(node);
        free_string_array(kargv);
        free_string_array(kenvp);
        return -ENOMEM;
    }

    size_t total = 0;
    size_t chunk = SYSCALL_IO_CHUNK;
    while (total < node->size) {
        size_t remaining = node->size - total;
        size_t to_read   = remaining < chunk ? remaining : chunk;
        size_t n         = vfs_read(node->handle, elf_data + total, total, to_read);
        if (n == 0) break;
        total += n;
    }
    vfs_close(node);

    if (total < sizeof(uint32_t)) {
        free(elf_data);
        free_string_array(kargv);
        free_string_array(kenvp);
        return -ENOEXEC;
    }

    /* Reset TLS state for the new process image */
    proc->task->thread.fs_base = 0;
    proc->task->thread.gs_base = 0;

    page_directory_t *old_dir = proc->user_page_dir;

    if (setup_process_page_dir(proc)) {
        free(elf_data);
        free_string_array(kargv);
        free_string_array(kenvp);
        return -ENOMEM;
    }

    if (old_dir) {
        free_page_table_recursive(old_dir->table, 4);
        free(old_dir);
    }

    proc->heap_brk  = PROCESS_HEAP_START;
    proc->stack_brk = PROCESS_STACK_BASE - PROCESS_STACK_SIZE;

    int ret = elf_loader_load_user_process(proc, elf_data, total, kargv, kenvp);
    free(elf_data);
    free_string_array(kargv);
    free_string_array(kenvp);

    if (ret) return -ENOEXEC;
    return 0;
}

static int64_t sys_execve_wrap(uint64_t path, uint64_t argv, uint64_t envp, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return do_execve((const char *)path, (char *const *)argv, (char *const *)envp);
}

/* ---------- getdents64 ---------- */

struct linux_dirent64 {
        uint64_t       d_ino;
        int64_t        d_off;
        unsigned short d_reclen;
        unsigned char  d_type;
        char           d_name[];
};

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

static int64_t sys_getdents64_impl(int fd, uint64_t dirent, uint64_t count);

static int64_t sys_getdents64_wrap(uint64_t fd, uint64_t dirent, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *file = process_fd_get(proc, (int)fd);
    if (!file) return -EBADF;

    int64_t ret = sys_getdents64_impl(fd, dirent, count);
    process_file_put(file);
    return ret;
}

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
        process_file_put(file);
        return -ENOMEM;
    }

    uint64_t written = 0;
    size_t   index   = file->offset;

    for (;;) {
        vfs_dirent_t entry;
        if (vfs_readdir(node, index, &entry) != EOK) break;

        size_t         name_len = strlen(entry.name);
        unsigned short reclen   = (unsigned short)(sizeof(struct linux_dirent64) + name_len + 1);
        reclen                  = (unsigned short)ALIGN_UP(reclen, 8);

        if (written + reclen > count) break;

        struct linux_dirent64 *de = (struct linux_dirent64 *)(kbuf + written);
        de->d_ino                 = entry.inode;
        de->d_off                 = (int64_t)(index + 1);
        de->d_reclen              = reclen;
        de->d_type                = (entry.type & file_dir) ? DT_DIR : DT_REG;
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

/* ---------- writev / readv ---------- */

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
        if (!vec) return -ENOMEM;
        alloc = 1;
    }

    if (copy_from_user(vec, (const void *)iov, iovcnt * sizeof(iovec_t))) {
        if (alloc) free(vec);
        return -EFAULT;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len == 0) continue;
        uint8_t tmp[SYSCALL_IO_CHUNK];
        size_t  iov_done = 0;
        while (iov_done < vec[i].iov_len) {
            size_t chunk = (vec[i].iov_len - iov_done) < sizeof(tmp) ? (size_t)(vec[i].iov_len - iov_done) : sizeof(tmp);
            if (copy_from_user(tmp, (const void *)((uintptr_t)vec[i].iov_base + iov_done), chunk)) {
                if (total == 0) total = -EFAULT;
                goto writev_done;
            }
            int64_t n = process_fd_write(proc, (int)fd, tmp, chunk);
            if (n < 0) {
                if (total == 0) total = n;
                goto writev_done;
            }
            total += n;
            iov_done += (size_t)n;
            if ((size_t)n < chunk) break;
        }
    }
writev_done:

    if (alloc) free(vec);
    return total;
}

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
        if (!vec) return -ENOMEM;
        alloc = 1;
    }

    if (copy_from_user(vec, (const void *)iov, iovcnt * sizeof(iovec_t))) {
        if (alloc) free(vec);
        return -EFAULT;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len == 0) continue;
        uint8_t tmp[SYSCALL_IO_CHUNK];
        size_t  iov_done = 0;
        while (iov_done < vec[i].iov_len) {
            size_t  chunk = (vec[i].iov_len - iov_done) < sizeof(tmp) ? (size_t)(vec[i].iov_len - iov_done) : sizeof(tmp);
            int64_t n     = process_fd_read(proc, (int)fd, tmp, chunk);
            if (n < 0) {
                if (total == 0) total = n;
                goto readv_done;
            }
            if (!n) break;
            if (copy_to_user((void *)((uintptr_t)vec[i].iov_base + iov_done), tmp, (size_t)n)) {
                if (total == 0) total = -EFAULT;
                goto readv_done;
            }
            total += n;
            iov_done += (size_t)n;
            if ((size_t)n < chunk) break;
        }
    }
readv_done:

    if (alloc) free(vec);
    return total;
}

/* ---------- chroot ---------- */

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

    char kpath[256];
    if (strncpy_from_user(kpath, (const char *)path, sizeof(kpath)) < 0) return -EFAULT;
    kpath[sizeof(kpath) - 1] = '\0';

    vfs_node_t node = vfs_open(kpath);
    if (!node) return -ENOENT;
    if (!(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }
    vfs_close(node);

    strncpy(proc->root, kpath, sizeof(proc->root) - 1);
    proc->root[sizeof(proc->root) - 1] = '\0';
    return 0;
}

/* ---------- fcntl wrapper ---------- */

static int64_t sys_fcntl_wrap(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_fcntl((int)fd, (int)cmd, arg);
}

/* ---------- prctl implementation ---------- */

#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_GET_DUMPABLE  3
#define PR_SET_DUMPABLE  4
#define PR_GET_KEEPCAPS  7
#define PR_SET_KEEPCAPS  8
#define PR_SET_NAME      15
#define PR_GET_NAME      16
#define PR_SET_SECCOMP   22
#define PR_GET_SECCOMP   23
#define PR_SET_TIMERSLACK 29
#define PR_GET_TIMERSLACK 30
#define PR_SET_NO_NEW_PRIVS 36
#define PR_GET_NO_NEW_PRIVS 37

static int64_t sys_prctl_impl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg6;

    switch ((int)option) {
        case PR_SET_PDEATHSIG: {
            /* arg2 is the signal to send on parent death */
            /* For now we silently accept but don't implement the full mechanism */
            if ((int)arg2 > 64 && (int)arg2 != 0) return -EINVAL;
            return 0;
        }
        case PR_GET_PDEATHSIG: {
            /* Return 0 (no parent death signal) */
            if (arg2 && copy_to_user((void *)arg2, &(int){0}, sizeof(int))) return -EFAULT;
            return 0;
        }
        case PR_GET_DUMPABLE: {
            /* Return dumpable=1 */
            if (arg2 && copy_to_user((void *)arg2, &(int){1}, sizeof(int))) return -EFAULT;
            return 0;
        }
        case PR_SET_DUMPABLE: {
            /* Accept any value */
            return 0;
        }
        case PR_GET_KEEPCAPS:
        case PR_SET_KEEPCAPS:
            return 0;
        case PR_SET_NAME: {
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
        case PR_GET_NAME: {
            /* Get process name */
            if (arg2) {
                process_t *proc = process_current();
                if (!proc) return -ESRCH;
                if (copy_to_user((void *)arg2, proc->name, 16)) return -EFAULT;
            }
            return 0;
        }
        case PR_SET_SECCOMP:
        case PR_GET_SECCOMP:
            /* No seccomp support */
            return -EINVAL;
        case PR_SET_TIMERSLACK:
        case PR_GET_TIMERSLACK: {
            /* Return default timer slack = 50000 ns */
            if (option == PR_GET_TIMERSLACK && arg2) {
                uint64_t slack = 50000;
                if (copy_to_user((void *)arg2, &slack, sizeof(slack))) return -EFAULT;
            }
            return 0;
        }
        case PR_SET_NO_NEW_PRIVS:
        case PR_GET_NO_NEW_PRIVS:
            return 0;
        default:
            return -EINVAL;
    }
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
    [SYS_PREAD64]                = sys_pread64_stub,
    [SYS_PWRITE64]               = sys_pwrite64_impl,
    [SYS_READV]                  = sys_readv_wrap,
    [SYS_WRITEV]                 = sys_writev_wrap,
    [SYS_ACCESS]                 = sys_access_stub,
    [SYS_PIPE]                   = sys_pipe_wrap,
    [SYS_SELECT]                 = sys_select_stub,
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
    [SYS_GETITIMER]              = sys_stub_ok,
    [SYS_ALARM]                  = sys_stub_ok,
    [SYS_SETITIMER]              = sys_stub_ok,
    [SYS_GETPID]                 = sys_getpid,
    [SYS_SENDFILE]               = sys_stub,
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
    [SYS_FLOCK]                  = sys_stub_ok,
    [SYS_FSYNC]                  = sys_stub_ok,
    [SYS_FDATASYNC]              = sys_stub_ok,
    [SYS_TRUNCATE]               = sys_truncate_stub,
    [SYS_FTRUNCATE]              = sys_ftruncate_stub,
    [SYS_GETDENTS]               = sys_getdents64_wrap,
    [SYS_GETCWD]                 = sys_getcwd,
    [SYS_CHDIR]                  = sys_chdir_stub,
    [SYS_FCHDIR]                 = sys_fchdir_stub,
    [SYS_RENAME]                 = sys_rename,
    [SYS_MKDIR]                  = sys_mkdir,
    [SYS_RMDIR]                  = sys_unlink,
    [SYS_CREAT]                  = sys_creat,
    [SYS_LINK]                   = sys_link,
    [SYS_UNLINK]                 = sys_unlink,
    [SYS_SYMLINK]                = sys_symlink,
    [SYS_READLINK]               = sys_readlink,
    [SYS_CHMOD]                  = sys_stub,
    [SYS_FCHMOD]                 = sys_stub,
    [SYS_CHOWN]                  = sys_stub,
    [SYS_FCHOWN]                 = sys_stub,
    [SYS_LCHOWN]                 = sys_stub,
    [SYS_UMASK]                  = sys_umask_stub,
    [SYS_GETTIMEOFDAY]           = sys_gettimeofday,
    [SYS_GETRLIMIT]              = sys_getrlimit_stub,
    [SYS_GETRUSAGE]              = sys_getrusage_impl,
    [SYS_SYSINFO]                = sys_sysinfo_impl,
    [SYS_TIMES]                  = sys_times_stub,
    [SYS_PTRACE]                 = sys_stub,
    [SYS_GETUID]                 = sys_getuid,
    [SYS_SYSLOG]                 = sys_stub,
    [SYS_GETGID]                 = sys_getgid,
    [SYS_SETUID]                 = sys_setuid_stub,
    [SYS_SETGID]                 = sys_setgid_stub,
    [SYS_GETEUID]                = sys_getuid,
    [SYS_GETEGID]                = sys_getgid,
    [SYS_SETPGID]                = sys_setpgid_wrap,
    [SYS_GETPPID]                = sys_getppid,
    [SYS_GETPGRP]                = sys_getpgrp_wrap,
    [SYS_SETSID]                 = sys_setsid_wrap,
    [SYS_SETREUID]               = sys_setuid_stub,
    [SYS_SETREGID]               = sys_setgid_stub,
    [SYS_GETGROUPS]              = sys_stub_ok,
    [SYS_SETGROUPS]              = sys_stub_ok,
    [SYS_SETRESUID]              = sys_setuid_stub,
    [SYS_GETRESUID]              = sys_getresuid_stub,
    [SYS_SETRESGID]              = sys_setgid_stub,
    [SYS_GETRESGID]              = sys_getresgid_stub,
    [SYS_GETPGID]                = sys_getpgid_wrap,
    [SYS_SETFSUID]               = sys_setuid_stub,
    [SYS_SETFSGID]               = sys_setgid_stub,
    [SYS_GETSID]                 = sys_getsid_wrap,
    [SYS_CAPGET]                 = sys_stub_ok,
    [SYS_CAPSET]                 = sys_stub,
    [SYS_RT_SIGPENDING]          = sys_rt_sigpending_wrap,
    [SYS_RT_SIGTIMEDWAIT]        = sys_rt_sigtimedwait_wrap,
    [SYS_RT_SIGQUEUEINFO]        = sys_rt_sigqueueinfo_wrap,
    [SYS_RT_SIGSUSPEND]          = sys_rt_sigsuspend_wrap,
    [SYS_SIGALTSTACK]            = sys_sigaltstack_wrap,
    [SYS_UTIME]                  = sys_stub_ok,
    [SYS_MKNOD]                  = sys_stub,
    [SYS_USELIB]                 = sys_stub,
    [SYS_PERSONALITY]            = sys_personality_impl,
    [SYS_USTAT]                  = sys_stub,
    [SYS_STATFS]                 = sys_statfs_impl,
    [SYS_FSTATFS]                = sys_statfs_impl,
    [SYS_SYSFS]                  = sys_stub,
    [SYS_GETPRIORITY]            = sys_stub_ok,
    [SYS_SETPRIORITY]            = sys_stub_ok,
    [SYS_SCHED_SETPARAM]         = sys_stub_ok,
    [SYS_SCHED_GETPARAM]         = sys_stub_ok,
    [SYS_SCHED_SETSCHEDULER]     = sys_stub_ok,
    [SYS_SCHED_GETSCHEDULER]     = sys_stub_ok,
    [SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max_stub,
    [SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min_stub,
    [SYS_SCHED_RR_GET_INTERVAL]  = sys_sched_rr_get_interval_stub,
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
    [SYS_ADJTIMEX]               = sys_stub,
    [SYS_SETRLIMIT]              = sys_stub_ok,
    [SYS_CHROOT]                 = sys_chroot_wrap,
    [SYS_SYNC]                   = sys_sync_stub,
    [SYS_ACCT]                   = sys_stub,
    [SYS_SETTIMEOFDAY]           = sys_stub,
    [SYS_MOUNT]                  = sys_mount,
    [SYS_UMOUNT2]                = sys_umount2,
    [SYS_SWAPON]                 = sys_stub,
    [SYS_SWAPOFF]                = sys_stub,
    [SYS_REBOOT]                 = sys_stub,
    [SYS_SETHOSTNAME]            = sys_stub_ok,
    [SYS_SETDOMAINNAME]          = sys_stub_ok,
    [SYS_IOPL]                   = sys_stub,
    [SYS_IOPERM]                 = sys_stub,
    [SYS_CREATE_MODULE]          = sys_stub,
    [SYS_INIT_MODULE]            = sys_stub,
    [SYS_DELETE_MODULE]          = sys_stub,
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
    [SYS_READAHEAD]              = sys_readahead_stub,
    [SYS_SETXATTR]               = sys_stub,
    [SYS_LSETXATTR]              = sys_stub,
    [SYS_FSETXATTR]              = sys_stub,
    [SYS_GETXATTR]               = sys_stub,
    [SYS_LGETXATTR]              = sys_stub,
    [SYS_FGETXATTR]              = sys_stub,
    [SYS_LISTXATTR]              = sys_listxattr_stub,
    [SYS_LLISTXATTR]             = sys_listxattr_stub,
    [SYS_FLISTXATTR]             = sys_listxattr_stub,
    [SYS_REMOVEXATTR]            = sys_stub,
    [SYS_LREMOVEXATTR]           = sys_stub,
    [SYS_FREMOVEXATTR]           = sys_stub,
    [SYS_TKILL]                  = sys_tkill_stub,
    [SYS_TIME]                   = sys_time,
    [SYS_FUTEX]                  = sys_futex_wrap,
    [SYS_SCHED_SETAFFINITY]      = sys_stub_ok,
    [SYS_SCHED_GETAFFINITY]      = sys_sched_getaffinity_stub,
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
    [SYS_SET_TID_ADDRESS]        = sys_set_tid_address_stub,
    [SYS_RESTART_SYSCALL]        = sys_restart_syscall,
    [SYS_SEMTIMEDOP]             = sys_semtimedop_wrap,
    [SYS_FADVISE64]              = sys_fadvise64_stub,
    [SYS_TIMER_CREATE]           = sys_stub,
    [SYS_TIMER_SETTIME]          = sys_stub,
    [SYS_TIMER_GETTIME]          = sys_stub,
    [SYS_TIMER_GETOVERRUN]       = sys_stub,
    [SYS_TIMER_DELETE]           = sys_stub,
    [SYS_CLOCK_SETTIME]          = sys_stub,
    [SYS_CLOCK_GETTIME]          = sys_clock_gettime_stub,
    [SYS_CLOCK_GETRES]           = sys_clock_getres_stub,
    [SYS_CLOCK_NANOSLEEP]        = sys_clock_nanosleep_stub,
    [SYS_EXIT_GROUP]             = sys_exit_group,
    [SYS_EPOLL_WAIT]             = sys_epoll_wait_wrap,
    [SYS_EPOLL_CTL]              = sys_epoll_ctl_wrap,
    [SYS_TGKILL]                 = sys_tgkill_wrap,
    [SYS_UTIMES]                 = sys_stub_ok,
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
    [SYS_IOPRIO_SET]             = sys_stub,
    [SYS_IOPRIO_GET]             = sys_stub,
    [SYS_INOTIFY_INIT]           = sys_stub,
    [SYS_INOTIFY_ADD_WATCH]      = sys_stub,
    [SYS_INOTIFY_RM_WATCH]       = sys_stub,
    [SYS_MIGRATE_PAGES]          = sys_stub,
    [SYS_OPENAT]                 = sys_openat,
    [SYS_MKDIRAT]                = sys_mkdirat,
    [SYS_MKNODAT]                = sys_stub,
    [SYS_FCHOWNAT]               = sys_stub_ok,
    [SYS_FUTIMESAT]              = sys_stub_ok,
    [SYS_NEWFSTATAT]             = sys_newfstatat,
    [SYS_UNLINKAT]               = sys_unlinkat,
    [SYS_RENAMEAT]               = sys_renameat,
    [SYS_LINKAT]                 = sys_linkat,
    [SYS_SYMLINKAT]              = sys_symlinkat,
    [SYS_READLINKAT]             = sys_readlinkat,
    [SYS_FCHMODAT]               = sys_stub_ok,
    [SYS_FACCESSAT]              = sys_access_stub,
    [SYS_PSELECT6]               = sys_pselect6_stub,
    [SYS_PPOLL]                  = sys_ppoll_stub,
    [SYS_UNSHARE]                = sys_stub,
    [SYS_SET_ROBUST_LIST]        = sys_stub_ok,
    [SYS_GET_ROBUST_LIST]        = sys_stub,
    [SYS_SPLICE]                 = sys_stub,
    [SYS_TEE]                    = sys_stub,
    [SYS_SYNC_FILE_RANGE]        = sys_sync_file_range_stub,
    [SYS_VMSPLICE]               = sys_stub,
    [SYS_MOVE_PAGES]             = sys_stub,
    [SYS_UTIMENSAT]              = sys_utimensat_stub,
    [SYS_EPOLL_PWAIT]            = sys_epoll_pwait_wrap,
    [SYS_SIGNALFD]               = sys_signalfd_wrap,
    [SYS_TIMERFD_CREATE]         = sys_timerfd_create_wrap,
    [SYS_EVENTFD]                = sys_eventfd_wrap,
    [SYS_FALLOCATE]              = sys_fallocate_stub,
    [SYS_TIMERFD_SETTIME]        = sys_timerfd_settime_wrap,
    [SYS_TIMERFD_GETTIME]        = sys_timerfd_gettime_wrap,
    [SYS_ACCEPT4]                = sys_accept4_wrap,
    [SYS_SIGNALFD4]              = sys_signalfd4_wrap,
    [SYS_EVENTFD2]               = sys_eventfd2_wrap,
    [SYS_EPOLL_CREATE1]          = sys_epoll_create1_wrap,
    [SYS_DUP3]                   = sys_dup3,
    [SYS_PIPE2]                  = sys_pipe2_wrap,
    [SYS_INOTIFY_INIT1]          = sys_stub,
    [SYS_PREADV]                 = sys_stub,
    [SYS_PWRITEV]                = sys_stub,
    [SYS_RT_TGSIGQUEUEINFO]      = sys_rt_tgsigqueueinfo_wrap,
    [SYS_PERF_EVENT_OPEN]        = sys_stub,
    [SYS_RECVMMSG]               = sys_recvmmsg_wrap,
    [SYS_FANOTIFY_INIT]          = sys_stub,
    [SYS_FANOTIFY_MARK]          = sys_stub,
    [SYS_PRLIMIT64]              = sys_prlimit64_stub,
    [SYS_NAME_TO_HANDLE_AT]      = sys_stub,
    [SYS_OPEN_BY_HANDLE_AT]      = sys_stub,
    [SYS_CLOCK_ADJTIME]          = sys_stub,
    [SYS_SYNCFS]                 = sys_stub_ok,
    [SYS_SENDMMSG]               = sys_sendmmsg_wrap,
    [SYS_SETNS]                  = sys_stub,
    [SYS_GETCPU]                 = sys_getcpu_stub,
    [SYS_PROCESS_VM_READV]       = sys_stub,
    [SYS_PROCESS_VM_WRITEV]      = sys_stub,
    [SYS_KCMP]                   = sys_stub,
    [SYS_FINIT_MODULE]           = sys_stub,
    [SYS_SCHED_SETATTR]          = sys_stub_ok,
    [SYS_SCHED_GETATTR]          = sys_stub_ok,
    [SYS_RENAMEAT2]              = sys_renameat2_stub,
    [SYS_SECCOMP]                = sys_stub,
    [SYS_GETRANDOM]              = sys_getrandom_stub,
    [SYS_MEMFD_CREATE]           = sys_memfd_create_stub,
    [SYS_KEXEC_FILE_LOAD]        = sys_stub,
    [SYS_BPF]                    = sys_stub,
    [SYS_EXECVEAT]               = sys_execveat_stub,
    [SYS_USERFAULTFD]            = sys_stub,
    [SYS_MEMBARRIER]             = sys_membarrier_stub,
    [SYS_MLOCK2]                 = sys_mlock2_stub,
    [SYS_COPY_FILE_RANGE]        = sys_copy_file_range_stub,
    [SYS_PREADV2]                = sys_stub,
    [SYS_PWRITEV2]               = sys_stub,
    [SYS_PKEY_MPROTECT]          = sys_pkey_mprotect_stub,
    [SYS_PKEY_ALLOC]             = sys_stub,
    [SYS_PKEY_FREE]              = sys_stub,
    [SYS_STATX]                  = sys_statx,
    [SYS_IO_PGETEVENTS]          = sys_stub,
    [SYS_RSEQ]                   = sys_stub,
    [SYS_PIDFD_SEND_SIGNAL]      = sys_stub,
    [SYS_IO_URING_SETUP]         = sys_stub,
    [SYS_IO_URING_ENTER]         = sys_stub,
    [SYS_IO_URING_REGISTER]      = sys_stub,
    [SYS_OPEN_TREE]              = sys_stub,
    [SYS_MOVE_MOUNT]             = sys_stub,
    [SYS_FSOPEN]                 = sys_stub,
    [SYS_FSCONFIG]               = sys_stub,
    [SYS_FSMOUNT]                = sys_stub,
    [SYS_FSPICK]                 = sys_stub,
    [SYS_PIDFD_OPEN]             = sys_stub,
    [SYS_CLONE3]                 = sys_stub,
    [SYS_CLOSE_RANGE]            = sys_close_range,
    [SYS_FACCESSAT2]             = sys_access_stub,
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

uint64_t syscall_user_rsp_tmp;

__attribute__((naked)) void syscall_entry_syscall(void)
{
    __asm__ volatile("cld\n\t"
                     "movq %rsp, syscall_user_rsp_tmp(%rip)\n\t"
                     "movq tss0+4(%rip), %rsp\n\t"
                     "pushq $0x23\n\t"
                     "pushq syscall_user_rsp_tmp(%rip)\n\t"
                     "pushq %r11\n\t"
                     "pushq $0x1B\n\t"
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
                     "jmp syscall_return\n\t");
}

void syscall_init(void)
{
    register_interrupt_handler(SYSCALL_VECTOR, (void *)syscall_entry, 0, 0xee);

    uint64_t star = rdmsr(0xC0000081);
    star &= 0x00000000FFFFFFFFULL;
    star |= ((uint64_t)0x08 << 32) | ((uint64_t)0x1B << 48);
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (uint64_t)syscall_entry_syscall);
    wrmsr(0xC0000084, 0x200);
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);
    plogk("syscall: int 0x%02x interface initialized.\n", SYSCALL_VECTOR);
}
