/*
 *
 *      syscall_io.c
 *      File I/O syscalls: open, read/write, vector I/O, splice and sync
 *
 *      2026/11/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <ipc/pipe.h>
#include <kernel/abi.h>
#include <kernel/debug/printk.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/memfd.h>
#include <process/fcntl.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>

#define SYSCALL_IO_CHUNK 16384

/* close_range flags */
#define CLOSE_RANGE_UNSHARE (1U << 1)
#define CLOSE_RANGE_CLOEXEC (1U << 2)

/* dirent d_type */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

/* openat2 how argument */
typedef struct open_how {
        uint64_t flags;
        uint64_t mode;
        uint64_t resolve;
} open_how_t;

typedef struct getdents64_context {
        uint8_t *buffer;
        size_t   capacity;
        size_t   written;
        bool     entry_too_large;
} getdents64_context_t;

/* open syscall: open or create a file */
int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!path) return -EFAULT;
    if ((flags & O_TMPFILE) == O_TMPFILE) {
        process_t *proc = process_current();
        if (!proc) return -ESRCH;

        char dir[CONFIG_VFS_PATH_MAX];
        int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, dir);
        if (ret != EOK) return ret;

        vfs_node_t dnode = vfs_open_checked(dir, &ret);
        if (!dnode) return ret;
        if (!(dnode->type & file_dir)) {
            vfs_close(dnode);
            return -ENOTDIR;
        }

        vfs_close(dnode);
        static uint64_t tmp_id = 0;
        uint64_t        id     = __atomic_fetch_add(&tmp_id, 1, __ATOMIC_RELAXED);
        char            tmp[CONFIG_VFS_PATH_MAX];
        (void)snprintf(tmp, sizeof(tmp), "%s/.tmp.%llu", dir, id);

        ret = vfs_mkfile_mode(tmp, 0600);
        if (ret != EOK) return ret;

        vfs_node_t node = vfs_open_checked(tmp, &ret);
        if (!node) return ret;

        /*
         * Keep the file linked for now; systemd will link it via linkat(AT_EMPTY_PATH),
         * then mark it as O_TMPFILE so linkat can handle it.
         */
        int fd = process_fd_install(proc, node, flags & ~O_TMPFILE);
        if (fd < 0) vfs_close(node);
        return fd;
    }

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
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
int64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (!path) return -EFAULT;
    if ((flags & O_TMPFILE) == O_TMPFILE) {
        process_t *proc = process_current();
        if (!proc) return -ESRCH;

        char dir[CONFIG_VFS_PATH_MAX];
        int  ret = copy_resolved_path_at(proc, (int)dirfd, path, dir);
        if (ret != EOK) return ret;

        vfs_node_t dnode = vfs_open_checked(dir, &ret);
        if (!dnode) return ret;
        if (!(dnode->type & file_dir)) {
            vfs_close(dnode);
            return -ENOTDIR;
        }

        vfs_close(dnode);
        static uint64_t tmp_id2 = 0;
        uint64_t        id      = __atomic_fetch_add(&tmp_id2, 1, __ATOMIC_RELAXED);
        char            tmp[CONFIG_VFS_PATH_MAX];
        (void)snprintf(tmp, sizeof(tmp), "%s/.tmp.%llu", dir, id);

        ret = vfs_mkfile_mode(tmp, 0600);
        if (ret != EOK) return ret;

        vfs_node_t node = vfs_open_checked(tmp, &ret);
        if (!node) return ret;

        int fd = process_fd_install(proc, node, flags & ~O_TMPFILE);
        if (fd < 0) vfs_close(node);
        return fd;
    }

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
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
int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (first > last) return -EINVAL;
    if (flags & ~(uint64_t)(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) return -EINVAL;
    if (flags & CLOSE_RANGE_UNSHARE) return -EINVAL;
    if (first >= CONFIG_PROCESS_MAX_FD) return EOK;

    uint64_t end = last < (uint64_t)(CONFIG_PROCESS_MAX_FD - 1) ? last : (uint64_t)(CONFIG_PROCESS_MAX_FD - 1);
    if (flags & CLOSE_RANGE_CLOEXEC) {
        spin_lock(&proc->fd_lock);
        for (uint64_t fd = first; fd <= end; fd++)
            if (proc->fds[fd]) proc->fd_flags[fd] |= FD_CLOEXEC;
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

/* System call handler for `creat`. */
int64_t sys_creat(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_open(path, O_CREAT | O_WRONLY, mode, 0, 0, 0);
}

/* System call handler for `close`. */
int64_t sys_close(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* System call handler for `read_task`. */
int64_t sys_read_task(task_t *task, uint64_t fd, uint64_t buf, uint64_t size)
{
    if (!buf && size) return -EFAULT;
    process_t *proc = task ? task->process : NULL;
    if (!proc) return -ESRCH;

    return process_fd_read_user(proc, (int)fd, (void *)buf, (size_t)size);
}

/* System call handler for `read`. */
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_read_task(current_task(), fd, buf, size);
}

/* System call handler for `write_task`. */
int64_t sys_write_task(task_t *task, uint64_t fd, uint64_t buf, uint64_t size)
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

/* System call handler for `write`. */
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_write_task(current_task(), fd, buf, size);
}

/* System call handler for `lseek`. */
int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_seek(proc, (int)fd, (int64_t)offset, (int)whence);
}

/* System call handler for `dup`. */
int64_t sys_dup(uint64_t oldfd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* System call handler for `dup2`. */
int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    return process_fd_dup2(proc, (int)oldfd, (int)newfd);
}

/* System call handler for `dup3`. */
int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

    if (newfd < CONFIG_PROCESS_MAX_FD && proc->fds[newfd]) {
        proc->fd_flags[newfd] = FD_CLOEXEC;
    } else {
        result = -EBADF;
    }

    spin_unlock(&proc->fd_lock);
    return result;
}

/* ioctl syscall: issue a device control command */
int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* Commands may arrive sign-extended when libc exposes them as int; truncate at the ABI boundary. */
    return process_fd_ioctl(proc, (int)fd, (size_t)(uint32_t)req, (void *)arg);
}

/* System call handler for `fsync`. */
int64_t sys_fsync_impl(uint64_t fd, uint64_t data_only, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

    int result = vfs_fsync(file->node, &file->wb_err, 0);
    process_file_put(file);
    return result;
}

/* System call handler for `fdatasync`. */
int64_t sys_fdatasync_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

    int result = vfs_fsync(file->node, &file->wb_err, 1);
    process_file_put(file);
    return result;
}

/* System call handler for `syncfs`. */
int64_t sys_syncfs_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* readahead syscall: prefetch file data */
int64_t sys_readahead(uint64_t fd, uint64_t offset, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
int64_t sys_fadvise64(uint64_t fd, uint64_t offset, uint64_t len, uint64_t advice, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (advice > 5 || offset > UINT64_MAX - len) return -EINVAL;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    process_file_t *file = process_fd_get(proc, (int)fd);

    if (!file) return -EBADF;
    int result = EOK;

    if (advice == 3) {
        result = vfs_readahead(file->node, offset, (size_t)len);
    } else if (advice == 4) {
        uint64_t end = len ? offset + len - 1 : UINT64_MAX;
        result       = vfs_drop_pages(file->node, offset, end, 1);
    }

    process_file_put(file);
    return result == -EOPNOTSUPP ? EOK : result;
}

/* copy_file_range syscall: copy a byte range between two descriptors */
int64_t sys_copy_file_range_wrap(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out, uint64_t len, uint64_t flags)
{
    if (flags) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf_in  = process_fd_get(proc, (int)fd_in);
    process_file_t *pf_out = process_fd_get(proc, (int)fd_out);
    if (!pf_in || !pf_out) {
        if (pf_in) process_file_put(pf_in);
        if (pf_out) process_file_put(pf_out);
        return -EBADF;
    }

    /*
     * Semantics: with a struct offset pointer the copy starts at
     * *off, the fd position is left untouched and *off is bumped by the
     * copied byte count.  With a NULL pointer the fd position itself is
     * consumed, so repeated calls advance toward EOF and eventually
     * return 0 — callers (systemd) rely on that to end their loops.
     */
    bool    have_off_in  = (off_in != 0);
    bool    have_off_out = (off_out != 0);
    int64_t user_off_in = 0, user_off_out = 0;
    int64_t saved_pos_in = -1, saved_pos_out = -1;
    int64_t ret;

    if (have_off_in) {
        if (copy_from_user(&user_off_in, (const void *)off_in, sizeof(user_off_in))) {
            ret = -EFAULT;
            goto out;
        }
        if (user_off_in < 0) {
            ret = -EINVAL;
            goto out;
        }
    }
    if (have_off_out) {
        if (copy_from_user(&user_off_out, (const void *)off_out, sizeof(user_off_out))) {
            ret = -EFAULT;
            goto out;
        }
        if (user_off_out < 0) {
            ret = -EINVAL;
            goto out;
        }
    }

    uint8_t buf[SYSCALL_IO_CHUNK];
    if (pf_in->node == pf_out->node) {
        int64_t start_in  = have_off_in ? user_off_in : process_fd_seek(proc, (int)fd_in, 0, SEEK_CUR);
        int64_t start_out = have_off_out ? user_off_out : process_fd_seek(proc, (int)fd_out, 0, SEEK_CUR);
        if (start_in >= 0 && start_out >= 0 && (start_in < start_out ? (uint64_t)(start_out - start_in) : (uint64_t)(start_in - start_out)) < len) {
            ret = -EINVAL;
            goto out;
        }

        /*
         * One description cannot ride its single shared offset through a copy,
         * so this walks both explicit positions and leaves the offset alone.
         */
        if (fd_in == fd_out) {
            if (!have_off_in || !have_off_out) {
                ret = -EINVAL;
                goto out;
            }

            uint64_t pos_in  = (uint64_t)user_off_in;
            uint64_t pos_out = (uint64_t)user_off_out;
            int64_t  copied  = 0;
            mutex_lock(&pf_in->io_lock);
            while ((uint64_t)copied < len) {
                size_t chunk = len - (uint64_t)copied;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);

                int64_t got = vfs_file_read_granted(pf_in->node, pf_in->private_data, pf_in->flags, buf, pos_in, chunk, proc);
                if (got <= 0) {
                    copied = copied ? copied : got;
                    break;
                }

                int64_t put = vfs_file_write_granted(pf_out->node, pf_out->private_data, pf_out->flags, buf, pos_out, (size_t)got, proc);
                if (put <= 0) {
                    copied = copied ? copied : put;
                    break;
                }

                copied += put;
                pos_in += (uint64_t)put;
                pos_out += (uint64_t)put;
                if ((size_t)got < chunk) break;
            }

            mutex_unlock(&pf_in->io_lock);
            if (copied >= 0) {
                int64_t reached_in  = (int64_t)pos_in;
                int64_t reached_out = (int64_t)pos_out;
                if (copy_to_user((void *)off_in, &reached_in, sizeof(reached_in)) || copy_to_user((void *)off_out, &reached_out, sizeof(reached_out))) copied = -EFAULT;
            }

            ret = process_fd_write_flush(proc, (int)fd_out, copied);
            process_file_put(pf_in);
            process_file_put(pf_out);
            return ret;
        }
    }
    if (have_off_in) {
        saved_pos_in = process_fd_seek(proc, (int)fd_in, 0, SEEK_CUR);
        if (saved_pos_in < 0) {
            ret = saved_pos_in;
            goto out;
        }
        if (process_fd_seek(proc, (int)fd_in, user_off_in, SEEK_SET) < 0) {
            ret = -EINVAL;
            goto out;
        }
    }
    if (have_off_out) {
        saved_pos_out = process_fd_seek(proc, (int)fd_out, 0, SEEK_CUR);
        if (saved_pos_out < 0) {
            ret = saved_pos_out;
            goto out;
        }
        if (process_fd_seek(proc, (int)fd_out, user_off_out, SEEK_SET) < 0) {
            ret = -EINVAL;
            goto out;
        }
    }

    size_t total_copied = 0;
    while (total_copied < len) {
        size_t chunk = len - total_copied;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        int64_t nread = process_fd_read(proc, (int)fd_in, buf, chunk);
        if (nread < 0) {
            ret = total_copied ? (int64_t)total_copied : nread;
            goto out;
        }
        if (nread == 0) break;

        int64_t nwritten = 0;
        size_t  sent     = 0;
        while (sent < (size_t)nread) {
            nwritten = process_fd_write_deferred(proc, (int)fd_out, buf + sent, (size_t)nread - sent);
            if (nwritten <= 0) break;
            sent += (size_t)nwritten;
        }

        /* Give back the part of the chunk the output would not take, so the input offset matches the bytes copied; a pipe or socket input has no offset, so its refused tail is dropped. */
        if (sent < (size_t)nread) process_fd_seek(proc, (int)fd_in, -(int64_t)((size_t)nread - sent), SEEK_CUR);
        if (nwritten <= 0 || sent < (size_t)nread) {
            ret = (total_copied + sent) ? (int64_t)(total_copied + sent) : nwritten;
            goto out;
        }

        total_copied += sent;
        if ((size_t)nread < chunk) break;
    }
    ret = (int64_t)total_copied;
out:
    /* Report the reached positions, then restore fd positions for explicit-offset callers */
    if (have_off_in) {
        int64_t pos = process_fd_seek(proc, (int)fd_in, 0, SEEK_CUR);
        if (ret >= 0 && pos >= 0 && copy_to_user((void *)off_in, &pos, sizeof(pos))) ret = -EFAULT;
        if (saved_pos_in >= 0) process_fd_seek(proc, (int)fd_in, saved_pos_in, SEEK_SET);
    }
    if (have_off_out) {
        int64_t pos = process_fd_seek(proc, (int)fd_out, 0, SEEK_CUR);
        if (ret >= 0 && pos >= 0 && copy_to_user((void *)off_out, &pos, sizeof(pos))) ret = -EFAULT;
        if (saved_pos_out >= 0) process_fd_seek(proc, (int)fd_out, saved_pos_out, SEEK_SET);
    }

    ret = process_fd_write_flush(proc, (int)fd_out, ret);
    process_file_put(pf_in);
    process_file_put(pf_out);
    return ret;
}

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

/* Pack one VFS entry directly into the kernel-side dirent buffer. */
static bool getdents64_emit(const vfs_dirent_t *entry, size_t next_index, void *opaque)
{
    getdents64_context_t *context     = opaque;
    size_t                name_len    = strlen(entry->name);
    size_t                record_size = ALIGN_UP(sizeof(linux_dirent64_t) + name_len + 1, 8);
    if (record_size > UINT16_MAX || record_size > context->capacity - context->written) {
        if (!context->written) context->entry_too_large = true;
        return false;
    }

    linux_dirent64_t *de = (linux_dirent64_t *)(context->buffer + context->written);
    de->d_ino            = entry->inode;
    de->d_off            = (int64_t)next_index;
    de->d_reclen         = (unsigned short)record_size;
    de->d_type           = vfs_node_to_dtype(entry->type);
    memcpy(de->d_name, entry->name, name_len + 1);
    memset((uint8_t *)de + sizeof(linux_dirent64_t) + name_len + 1, 0, record_size - sizeof(linux_dirent64_t) - name_len - 1);
    context->written += record_size;

    return true;
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
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("syscall: getdents64 buffer allocation failed (%llu bytes)\n", count);
        process_file_put(file);
        return -ENOMEM;
    }

    getdents64_context_t context = {
        .buffer   = kbuf,
        .capacity = (size_t)count,
    };

    size_t next_index = file->offset;
    int    result     = vfs_readdir_batch(node, file->offset, getdents64_emit, &context, &next_index);
    if (result != EOK) {
        free(kbuf);
        process_file_put(file);
        return result;
    }
    if (context.entry_too_large) {
        free(kbuf);
        process_file_put(file);
        return -EINVAL;
    }

    file->offset = next_index;
    if (context.written > 0) {
        if (copy_to_user((void *)dirent, kbuf, context.written)) {
            free(kbuf);
            process_file_put(file);
            return -EFAULT;
        }
    }

    free(kbuf);
    process_file_put(file);
    return (int64_t)context.written;
}

/* System call handler for `getdents64`. */
int64_t sys_getdents64_wrap(uint64_t fd, uint64_t dirent, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    int64_t ret = sys_getdents64_impl(fd, dirent, count);
    return ret;
}

/* writev syscall: write a vector of buffers */
int64_t sys_writev_wrap(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("syscall: writev iovec allocation failed (%llu entries)\n", iovcnt);
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
        int64_t n = process_fd_write_user_deferred(proc, (int)fd, vec[i].iov_base, vec[i].iov_len);
        if (n < 0) {
            if (total == 0) total = n;
            goto writev_done;
        }
        total += n;
        if ((size_t)n < vec[i].iov_len) break;
    }
writev_done:
    total = process_fd_write_flush(proc, (int)fd, total);
    if (alloc) free(vec);
    return total;
}

/* readv syscall: read into a vector of buffers */
int64_t sys_readv_wrap(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("syscall: readv iovec allocation failed (%llu entries)\n", iovcnt);
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

/* fcntl wrapper */
int64_t sys_fcntl_wrap(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_fcntl((int)fd, (int)cmd, arg);
}

/* truncate syscall */
int64_t sys_truncate_impl(uint64_t path, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!path) return -EFAULT;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, name);

    if (ret) return ret;
    char resolved[CONFIG_VFS_PATH_MAX];
    ret = process_resolve_path_at(proc, PROCESS_AT_FDCWD, name, resolved, sizeof(resolved));

    if (ret) return ret;
    vfs_node_t node = vfs_open(resolved);

    if (!node) return -ENOENT;
    ret = vfs_truncate(node, length);
    vfs_close(node);

    return ret;
}

/* ftruncate syscall */
int64_t sys_ftruncate_impl(uint64_t fd, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if ((int64_t)length < 0) return -EINVAL;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);

    if (!pf || !pf->node) {
        if (pf) process_file_put(pf);
        return -EBADF;
    }
    if ((pf->flags & O_ACCMODE) == O_RDONLY) {
        process_file_put(pf);
        return -EINVAL;
    }

    int ret;
    if (memfd_is_node(pf->node)) {
        ret = memfd_resize(pf->node, length);
    } else {
        ret = vfs_truncate(pf->node, length);
    }

    process_file_put(pf);
    return ret;
}

/* sync syscall: flush all filesystems */
int64_t sys_sync_impl(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    vfs_sync_all();
    return 0;
}

/* pread64 syscall */
int64_t sys_pread64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (!buf && count) return -EFAULT;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    if (offset > SIZE_MAX) return -EOVERFLOW;
    return process_fd_pread_user(proc, (int)fd, (void *)buf, (size_t)count, offset);
}

/* pwrite64 syscall */
int64_t sys_pwrite64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (!buf && count) return -EFAULT;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    if (offset > SIZE_MAX) return -EOVERFLOW;
    return process_fd_pwrite_user(proc, (int)fd, (const void *)buf, (size_t)count, offset);
}

/* fallocate syscall */
int64_t sys_fallocate_impl(uint64_t fd, uint64_t mode, uint64_t offset, uint64_t len, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if ((int64_t)offset < 0 || (int64_t)len <= 0 || offset > UINT64_MAX - len) return -EINVAL;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);

    if (!pf || !pf->node) {
        if (pf) process_file_put(pf);
        return -EBADF;
    }
    if ((pf->flags & O_ACCMODE) == O_RDONLY) {
        process_file_put(pf);
        return -EBADF;
    }
    if (memfd_is_node(pf->node)) {
        int ret = memfd_fallocate(pf->node, (uint32_t)mode, offset, len);
        process_file_put(pf);
        return ret;
    }
    if (mode & ~3ULL) {
        process_file_put(pf);
        return -EOPNOTSUPP;
    }
    if (mode & 2) {
        process_file_put(pf);
        return -EOPNOTSUPP;
    } // FALLOC_FL_PUNCH_HOLE

    int ret = EOK;
    if (offset + len > pf->node->size) ret = vfs_truncate(pf->node, offset + len);

    process_file_put(pf);
    return ret;
}

/* sync_file_range syscall */
int64_t sys_sync_file_range_impl(uint64_t fd, uint64_t offset, uint64_t nbytes, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (flags & ~7ULL) return -EINVAL;
    if (nbytes > UINT64_MAX - offset) return -EINVAL;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf) return -EBADF;

    /* The wait and write flags all come down to one synchronous writeback of the range; a count of zero reaches the end of the file. */
    int result = EOK;
    if (flags) result = vfs_writeback_range(pf->node, offset, nbytes ? offset + nbytes - 1 : UINT64_MAX, 0);

    process_file_put(pf);
    return result;
}

/* sendfile syscall: copy data between file descriptors */
int64_t sys_sendfile_impl(uint64_t out_fd, uint64_t in_fd, uint64_t offset, uint64_t count, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (!count) return 0;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    process_file_t *pf_in  = process_fd_get(proc, (int)in_fd);
    process_file_t *pf_out = process_fd_get(proc, (int)out_fd);

    if (!pf_in || !pf_out) {
        if (pf_in) process_file_put(pf_in);
        if (pf_out) process_file_put(pf_out);
        return -EBADF;
    }

    int64_t old_off  = 0;
    bool    have_off = (offset != 0);
    int64_t user_off = 0;

    if (have_off) {
        if (copy_from_user(&user_off, (const void *)offset, sizeof(user_off))) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return -EFAULT;
        }
        if (user_off < 0) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return -EINVAL;
        }

        old_off = process_fd_seek(proc, (int)in_fd, 0, SEEK_CUR);
        if (old_off < 0) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return old_off;
        }
        if (process_fd_seek(proc, (int)in_fd, user_off, SEEK_SET) < 0) {
            process_file_put(pf_in);
            process_file_put(pf_out);
            return -EINVAL;
        }
    }

    uint8_t buf[4096];
    size_t  total = 0;

    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        int64_t n = process_fd_read(proc, (int)in_fd, buf, chunk);
        if (n < 0) {
            if (have_off) process_fd_seek(proc, (int)in_fd, old_off, SEEK_SET);
            process_file_put(pf_in);
            process_file_put(pf_out);
            return process_fd_write_flush(proc, (int)out_fd, total ? (int64_t)total : n);
        }
        if (!n) break;

        int64_t w    = 0;
        size_t  sent = 0;
        while (sent < (size_t)n) {
            w = process_fd_write_deferred(proc, (int)out_fd, buf + sent, (size_t)n - sent);
            if (w <= 0) break;
            sent += (size_t)w;
        }

        /* Give back the part of the chunk the output would not take, so the input offset matches the bytes copied; a pipe or socket input has no offset, so its refused tail is dropped. */
        if (sent < (size_t)n) process_fd_seek(proc, (int)in_fd, -(int64_t)((size_t)n - sent), SEEK_CUR);
        if (w <= 0 || sent < (size_t)n) {
            if (have_off) process_fd_seek(proc, (int)in_fd, old_off, SEEK_SET);
            process_file_put(pf_in);
            process_file_put(pf_out);
            return process_fd_write_flush(proc, (int)out_fd, (total + sent) ? (int64_t)(total + sent) : w);
        }
        total += sent;
        if ((size_t)n < chunk) break;
    }

    if (have_off) {
        int64_t new_off = process_fd_seek(proc, (int)in_fd, 0, SEEK_CUR);
        if (copy_to_user((void *)offset, &new_off, sizeof(int64_t))) {
            process_fd_seek(proc, (int)in_fd, old_off, SEEK_SET);
            process_fd_write_flush(proc, (int)out_fd, (int64_t)total);
            process_file_put(pf_in);
            process_file_put(pf_out);
            return -EFAULT;
        }
        process_fd_seek(proc, (int)in_fd, old_off, SEEK_SET);
    }
    process_file_put(pf_in);
    process_file_put(pf_out);
    return process_fd_write_flush(proc, (int)out_fd, (int64_t)total);
}

/* preadv syscall */
int64_t sys_preadv_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (iovcnt > 1024) return -EINVAL;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    iovec_t  inline_iov[16];
    iovec_t *vectors = inline_iov;

    if (iovcnt > 16) {
        vectors = malloc((size_t)iovcnt * sizeof(*vectors));
        if (!vectors) return -ENOMEM;
    }
    if (iovcnt && copy_from_user(vectors, (const void *)iov, (size_t)iovcnt * sizeof(*vectors))) {
        if (vectors != inline_iov) free(vectors);
        return -EFAULT;
    }

    size_t requested = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vectors[i].iov_len > 0x7ffff000UL - requested) {
            if (vectors != inline_iov) free(vectors);
            return -EINVAL;
        }
        requested += vectors[i].iov_len;
    }

    size_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (!vectors[i].iov_len) continue;
        if (offset > UINT64_MAX - total) {
            if (vectors != inline_iov) free(vectors);
            return total ? (int64_t)total : -EINVAL;
        }

        int64_t n = process_fd_pread_user(proc, (int)fd, vectors[i].iov_base, vectors[i].iov_len, offset + total);
        if (n < 0) {
            if (vectors != inline_iov) free(vectors);
            return total ? (int64_t)total : n;
        }

        total += (size_t)n;
        if ((size_t)n < vectors[i].iov_len) break;
    }
    if (vectors != inline_iov) free(vectors);
    return (int64_t)total;
}

/* pwritev syscall */
int64_t sys_pwritev_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (iovcnt > 1024) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    iovec_t  inline_iov[16];
    iovec_t *vectors = inline_iov;
    if (iovcnt > 16) {
        vectors = malloc((size_t)iovcnt * sizeof(*vectors));
        if (!vectors) return -ENOMEM;
    }
    if (iovcnt && copy_from_user(vectors, (const void *)iov, (size_t)iovcnt * sizeof(*vectors))) {
        if (vectors != inline_iov) free(vectors);
        return -EFAULT;
    }

    size_t requested = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (vectors[i].iov_len > 0x7ffff000UL - requested) {
            if (vectors != inline_iov) free(vectors);
            return -EINVAL;
        }
        requested += vectors[i].iov_len;
    }

    size_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (!vectors[i].iov_len) continue;
        if (offset > UINT64_MAX - total) {
            if (vectors != inline_iov) free(vectors);
            return process_fd_write_flush(proc, (int)fd, total ? (int64_t)total : -EINVAL);
        }

        int64_t n = process_fd_pwrite_user_deferred(proc, (int)fd, vectors[i].iov_base, vectors[i].iov_len, offset + total);
        if (n < 0) {
            if (vectors != inline_iov) free(vectors);
            return process_fd_write_flush(proc, (int)fd, total ? (int64_t)total : n);
        }

        total += (size_t)n;
        if ((size_t)n < vectors[i].iov_len) break;
    }
    if (vectors != inline_iov) free(vectors);
    return process_fd_write_flush(proc, (int)fd, (int64_t)total);
}

/* preadv2 syscall */
int64_t sys_preadv2_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    if (flags & ~1ULL) return -EINVAL; // RWF_HIPRI=1
    return sys_preadv_impl(fd, iov, iovcnt, offset, 0, 0);
}

/* pwritev2 syscall */
int64_t sys_pwritev2_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    if (flags & ~1ULL) return -EINVAL;
    return sys_pwritev_impl(fd, iov, iovcnt, offset, 0, 0);
}

/* splice syscall: copy data between file descriptors */
int64_t sys_splice_impl(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out, uint64_t len, uint64_t flags)
{
    if (flags & ~6ULL) return -EINVAL;

    /* Simple fallback: read from fd_in, write to fd_out */
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!len) return 0;

    bool    have_off_in  = (off_in != 0);
    bool    have_off_out = (off_out != 0);
    int64_t saved_in = -1, saved_out = -1;
    int64_t ret = 0;

    if (have_off_in) {
        int64_t user_in = 0;
        if (copy_from_user(&user_in, (const void *)off_in, sizeof(user_in))) {
            ret = -EFAULT;
            goto done;
        }
        if (user_in < 0) {
            ret = -EINVAL;
            goto done;
        }

        saved_in = process_fd_seek(proc, (int)fd_in, 0, SEEK_CUR);
        if (saved_in < 0) {
            ret = saved_in;
            goto done;
        }
        if (process_fd_seek(proc, (int)fd_in, user_in, SEEK_SET) < 0) {
            ret = -EINVAL;
            goto done;
        }
    }
    if (have_off_out) {
        int64_t user_out = 0;
        if (copy_from_user(&user_out, (const void *)off_out, sizeof(user_out))) {
            ret = -EFAULT;
            goto done;
        }
        if (user_out < 0) {
            ret = -EINVAL;
            goto done;
        }

        saved_out = process_fd_seek(proc, (int)fd_out, 0, SEEK_CUR);
        if (saved_out < 0) {
            ret = saved_out;
            goto done;
        }
        if (process_fd_seek(proc, (int)fd_out, user_out, SEEK_SET) < 0) {
            ret = -EINVAL;
            goto done;
        }
    }

    uint8_t buf[4096];
    size_t  total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        int64_t n = process_fd_read(proc, (int)fd_in, buf, chunk);
        if (n < 0) {
            ret = total ? (int64_t)total : n;
            goto done;
        }
        if (!n) break;

        int64_t w    = 0;
        size_t  sent = 0;
        while (sent < (size_t)n) {
            w = process_fd_write_deferred(proc, (int)fd_out, buf + sent, (size_t)n - sent);
            if (w <= 0) break;
            sent += (size_t)w;
        }

        /* Give back the part of the chunk the output would not take, so the input offset matches the bytes copied; a pipe or socket input has no offset, so its refused tail is dropped. */
        if (sent < (size_t)n) process_fd_seek(proc, (int)fd_in, -(int64_t)((size_t)n - sent), SEEK_CUR);
        if (w <= 0 || sent < (size_t)n) {
            ret = (total + sent) ? (int64_t)(total + sent) : w;
            goto done;
        }

        total += sent;
        if ((size_t)n < chunk) break;
    }
    ret = (int64_t)total;
done:
    ret = process_fd_write_flush(proc, (int)fd_out, ret);

    /* Report the reached positions, then restore fd positions for explicit-offset callers */
    if (have_off_in) {
        int64_t pos = process_fd_seek(proc, (int)fd_in, 0, SEEK_CUR);
        if (ret >= 0 && pos >= 0 && copy_to_user((void *)off_in, &pos, sizeof(pos))) ret = -EFAULT;
        if (saved_in >= 0) process_fd_seek(proc, (int)fd_in, saved_in, SEEK_SET);
    }
    if (have_off_out) {
        int64_t pos = process_fd_seek(proc, (int)fd_out, 0, SEEK_CUR);
        if (ret >= 0 && pos >= 0 && copy_to_user((void *)off_out, &pos, sizeof(pos))) ret = -EFAULT;
        if (saved_out >= 0) process_fd_seek(proc, (int)fd_out, saved_out, SEEK_SET);
    }
    return ret;
}

/* tee syscall: duplicate pipe content without consuming it */
int64_t sys_tee_impl(uint64_t fd_in, uint64_t fd_out, uint64_t len, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (flags & ~2ULL) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf_in  = process_fd_get(proc, (int)fd_in);
    process_file_t *pf_out = process_fd_get(proc, (int)fd_out);
    if (!pf_in || !pf_out) {
        if (pf_in) process_file_put(pf_in);
        if (pf_out) process_file_put(pf_out);
        return -EBADF;
    }

    int64_t ret = -EINVAL;
    if ((pf_in->node->type & file_pipe) && (pf_out->node->type & file_pipe)) {
        uint64_t write_flags = (pf_out->flags & O_NONBLOCK) || (flags & 2ULL) ? O_NONBLOCK : 0;
        ret                  = pipe_tee(pf_in->private_data, pf_out->private_data, (size_t)len, write_flags);
    }

    process_file_put(pf_in);
    process_file_put(pf_out);
    return ret;
}

/* vmsplice syscall: unsupported */
int64_t sys_vmsplice_impl(uint64_t fd, uint64_t iov, uint64_t nr_segs, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)fd;
    (void)iov;
    (void)nr_segs;
    (void)arg4;
    (void)arg5;

    if (flags & ~3ULL) return -EINVAL;
    return -ENOSYS;
}

/* openat2 syscall: openat with an extensible how argument */
int64_t sys_openat2_impl(uint64_t dirfd, uint64_t path, uint64_t how, uint64_t usize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (!path || !how) return -EFAULT;
    if (usize < sizeof(open_how_t)) return -EINVAL;
    if (usize > sizeof(open_how_t)) return -E2BIG;

    open_how_t oh;
    if (copy_from_user(&oh, (const void *)how, sizeof(oh))) return -EFAULT;
    if (oh.resolve & ~31ULL) return -EINVAL;
    if ((oh.flags & O_TMPFILE) == O_TMPFILE) return -EOPNOTSUPP;

    /* Delegate to the same logic as openat: open a file by dirfd+path */
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret) return ret;

    char resolved[CONFIG_VFS_PATH_MAX];
    ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
    if (ret) return ret;

    vfs_node_t node = vfs_open(resolved);
    if (!node) return -ENOENT;

    (void)oh.resolve; // resolve flags not enforced yet
    return process_fd_install(proc, node, oh.flags);
}

/* io_pgetevents syscall: unsupported */
int64_t sys_io_pgetevents_impl(uint64_t ctx_id, uint64_t min_nr, uint64_t nr, uint64_t events, uint64_t timeout, uint64_t sigmask)
{
    (void)ctx_id;
    (void)min_nr;
    (void)nr;
    (void)events;
    (void)timeout;
    (void)sigmask;
    return -ENOSYS;
}
