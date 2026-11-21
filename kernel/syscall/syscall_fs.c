/*
 *
 *      syscall_fs.c
 *      Filesystem syscalls: namespaces, metadata and attributes
 *
 *      2026/11/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <fs/core/vfs.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/tmpfs/tmpfs.h>
#include <ipc/pipe.h>
#include <kernel/abi.h>
#include <kernel/kdev_t.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <process/fcntl.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>

/* statx masks */
#define STATX_BASIC_STATS     0x000007ffU
#define STATX_MNT_ID          0x00001000U
#define STATX_ATTR_MOUNT_ROOT 0x00002000ULL

/* utimensat special values */
#define LINUX_UTIME_NOW  1073741823LL
#define LINUX_UTIME_OMIT 1073741822LL

/* renameat2 flags */
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2

/* utime legacy layout */
typedef struct linux_utimbuf {
        int64_t actime;
        int64_t modtime;
} linux_utimbuf_t;

/* Copy a path string from user space into a kernel buffer */
int copy_path_from_user(uint64_t upath, char path[CONFIG_VFS_PATH_MAX])
{
    if (!upath) return -EFAULT;
    int ret = strncpy_from_user(path, (const char *)upath, CONFIG_VFS_PATH_MAX);
    return ret < 0 ? ret : EOK;
}

/* Copy a path and resolve it relative to dirfd */
int copy_resolved_path_at(process_t *proc, int dirfd, uint64_t upath, char path[CONFIG_VFS_PATH_MAX])
{
    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(upath, input);

    if (ret != EOK) return ret;
    if (!input[0]) return -ENOENT;
    return process_resolve_path_at(proc, dirfd, input, path, CONFIG_VFS_PATH_MAX);
}

/* Resolve and open a path relative to dirfd */
static vfs_node_t open_path_at(process_t *proc, int dirfd, uint64_t upath, bool nofollow, int *error)
{
    char path[CONFIG_VFS_PATH_MAX];
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

    if (type & file_dir) {
        file_type = 0040000;
    } else if (type & file_symlink) {
        file_type = 0120000;
    } else if (type & file_block) {
        file_type = 0060000;
    } else if (type & (file_stream | file_keyboard | file_mouse | file_fbdev | file_audio | file_ptmx | file_pts)) {
        file_type = 0020000;
    } else if (type & file_pipe) {
        file_type = 0010000;
    } else if (type & file_socket) {
        file_type = 0140000;
    }

    return file_type | (mode & 07777);
}

/* Fill a stat structure from a VFS node snapshot */
static void fill_linux_stat(linux_stat_t *st, uint64_t uid, uint64_t gid, const process_fd_stat_t *src)
{
    memset(st, 0, sizeof(*st));
    st->st_dev     = dev_encode_uapi(src->dev);
    st->st_ino     = src->inode;
    st->st_nlink   = src->nlink ? src->nlink : 1;
    st->st_mode    = linux_mode_from_type(src->type, src->mode);
    st->st_uid     = (uint32_t)uid;
    st->st_gid     = (uint32_t)gid;
    st->st_rdev    = dev_encode_uapi(src->rdev);
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
    char path[CONFIG_VFS_PATH_MAX];
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

/* fstat syscall: stat an open descriptor */
int64_t sys_fstat(uint64_t fd, uint64_t statbuf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
int64_t sys_statx(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mask, uint64_t statbuf, uint64_t arg5)
{
    (void)arg5;
    if (!statbuf) return -EFAULT;

    /* Be permissive: mask unknown flags instead of rejecting (systemd may pass AT_STATX_*) */
    if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE)) flags &= (AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE);
    if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE) flags &= ~AT_STATX_SYNC_TYPE;
    (void)mask;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[CONFIG_VFS_PATH_MAX];
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
        char resolved[CONFIG_VFS_PATH_MAX];
        ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
        if (ret == EOK) node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow(resolved) : vfs_open(resolved);
        if (!node && ret == EOK) ret = -ENOENT;
    }
    if (!node) return ret;

    ret = (int)statx_node_to_user(node, statbuf);
    vfs_close(node);
    return ret;
}

/* System call handler for `stat`. */
int64_t sys_stat(uint64_t path, uint64_t statbuf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return stat_path_to_user(path, statbuf);
}

/* newfstatat syscall: stat a path relative to dirfd */
int64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t statbuf, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT)) {
        /* Be permissive for systemd's AT_NO_AUTOMOUNT and future flags; mask unknown bits */
        flags &= (AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT);
    }
    if (!path) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret != EOK) return ret;
    if (proc && proc->task && proc->task->pid == 1 && (streq(input, "/proc/1/root") || streq(input, "/proc/self/root"))) {
        vfs_node_t node = vfs_open_checked("/", &ret);
        if (!node) return ret;
        ret = (int)stat_node_to_user(node, statbuf);
        vfs_close(node);
        return ret;
    }

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

/* System call handler for `mkdir`. */
int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    return ret != EOK ? ret : vfs_mkdir_mode(name, (uint16_t)(mode & 07777U & ~proc->umask));
}

/* System call handler for `mkdirat`. */
int64_t sys_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)dirfd, path, name);
    return ret == EOK ? vfs_mkdir_mode(name, (uint16_t)(mode & 07777U & ~proc->umask)) : ret;
}

/* System call handler for `unlink`. */
int64_t sys_unlink(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* rmdir syscall: remove an empty directory */
int64_t sys_rmdir(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!path) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret != EOK) return ret;

    /* POSIX: rmdir(".") and rmdir("..") fail with EINVAL. */
    size_t len = strlen(input);
    while (len > 1 && input[len - 1] == '/') input[--len] = '\0';
    const char *leaf = strrchr(input, '/');

    leaf = leaf ? leaf + 1 : input;
    if (streq(leaf, ".") || streq(leaf, "..")) return -EINVAL;

    vfs_node_t node = open_path_at(proc, AT_FDCWD, path, true, &ret);
    if (!node) return ret;
    if (!(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }
    if (node == get_rootdir()) {
        vfs_close(node);
        return -EINVAL;
    }
    if (node->is_mount) {
        vfs_close(node);
        return -EBUSY;
    }

    ret = vfs_delete(node);
    vfs_close(node);
    return ret;
}

/* System call handler for `unlinkat`. */
int64_t sys_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
    if ((flags & AT_REMOVEDIR) && !(node->type & file_dir)) {
        ret = -ENOTDIR;
    } else if (!(flags & AT_REMOVEDIR) && (node->type & file_dir)) {
        ret = -EISDIR;
    } else {
        ret = vfs_delete(node);
    }

    vfs_close(node);
    return ret;
}

/* System call handler for `rename`. */
int64_t sys_rename(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char oldname[CONFIG_VFS_PATH_MAX];
    char newname[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, oldpath, oldname);
    if (ret != EOK) return ret;

    ret = copy_resolved_path_at(proc, AT_FDCWD, newpath, newname);
    if (ret != EOK) return ret;

    vfs_node_t node = vfs_open_nofollow(oldname);
    if (!node) return -ENOENT;

    vfs_node_t olddir = vfs_open_parent_of(oldname);
    vfs_node_t newdir = vfs_open_parent_of(newname);
    if (!olddir || !newdir) {
        ret = -ENOENT;
    } else {
        ret = vfs_rename(node, newdir, path_basename(newname), 0);
    }
    if (olddir) vfs_close(olddir);
    if (newdir) vfs_close(newdir);

    vfs_close(node);
    return ret;
}

/* System call handler for `renameat`. */
int64_t sys_renameat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char oldname[CONFIG_VFS_PATH_MAX];
    char newname[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)olddirfd, oldpath, oldname);
    if (ret != EOK) return ret;

    ret = copy_resolved_path_at(proc, (int)newdirfd, newpath, newname);
    if (ret != EOK) return ret;

    vfs_node_t node = vfs_open_nofollow(oldname);
    if (!node) return -ENOENT;

    vfs_node_t olddir = vfs_open_parent_of(oldname);
    vfs_node_t newdir = vfs_open_parent_of(newname);
    if (!olddir || !newdir) {
        ret = -ENOENT;
    } else {
        ret = vfs_rename(node, newdir, path_basename(newname), 0);
    }
    if (olddir) vfs_close(olddir);
    if (newdir) vfs_close(newdir);

    vfs_close(node);
    return ret;
}

/* System call handler for `link`. */
int64_t sys_link(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char oldname[CONFIG_VFS_PATH_MAX];
    char newname[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, oldpath, oldname);
    if (ret != EOK) return ret;

    ret = copy_resolved_path_at(proc, AT_FDCWD, newpath, newname);
    if (ret != EOK) return ret;
    return vfs_link(newname, oldname);
}

/* System call handler for `linkat`. */
int64_t sys_linkat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)arg5;

    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) return -EINVAL;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    if ((flags & AT_EMPTY_PATH) && (!oldpath || oldpath == 0)) {
        /* oldpath is empty, source is fd */
        process_file_t *file = process_fd_get(proc, (int)olddirfd);
        if (!file) return -EBADF;

        char src_path[CONFIG_VFS_PATH_MAX];
        int  r = vfs_node_path(file->node, src_path, sizeof(src_path));
        process_file_put(file);
        if (r != EOK) return r;

        char newname[CONFIG_VFS_PATH_MAX];
        int  ret = copy_resolved_path_at(proc, (int)newdirfd, newpath, newname);
        if (ret != EOK) return ret;
        return vfs_link(newname, src_path);
    }

    char oldname[CONFIG_VFS_PATH_MAX];
    char newname[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)olddirfd, oldpath, oldname);
    if (ret != EOK) return ret;

    ret = copy_resolved_path_at(proc, (int)newdirfd, newpath, newname);
    if (ret != EOK) return ret;

    /* Handle AT_EMPTY_PATH where oldpath is empty string but oldpath pointer is not NULL */
    if ((flags & AT_EMPTY_PATH) && oldname[0] == '\0') {
        process_file_t *file = process_fd_get(proc, (int)olddirfd);

        if (!file) return -EBADF;
        char src_path[CONFIG_VFS_PATH_MAX];
        int  r = vfs_node_path(file->node, src_path, sizeof(src_path));
        process_file_put(file);

        if (r != EOK) return r;
        return vfs_link(newname, src_path);
    }
    return (flags & AT_SYMLINK_FOLLOW) ? vfs_link_follow(newname, oldname) : vfs_link(newname, oldname);
}

/* System call handler for `symlink`. */
int64_t sys_symlink(uint64_t target, uint64_t linkpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char target_name[CONFIG_VFS_PATH_MAX];
    char link_name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(target, target_name);
    if (ret != EOK) return ret;

    ret = copy_resolved_path_at(proc, AT_FDCWD, linkpath, link_name);
    if (ret != EOK) return ret;
    return vfs_symlink(link_name, target_name);
}

/* System call handler for `symlinkat`. */
int64_t sys_symlinkat(uint64_t target, uint64_t newdirfd, uint64_t linkpath, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char target_name[CONFIG_VFS_PATH_MAX];
    char link_name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(target, target_name);
    if (ret != EOK) return ret;

    ret = copy_resolved_path_at(proc, (int)newdirfd, linkpath, link_name);
    return ret == EOK ? vfs_symlink(link_name, target_name) : ret;
}

/* System call handler for `readlink`. */
int64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!buf && bufsiz) return -EFAULT;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);

    if (ret != EOK) return ret;
    vfs_node_t node = vfs_open_nofollow(name);

    if (!node) return -ENOENT;
    if (!(node->type & file_symlink)) {
        vfs_close(node);
        return -EINVAL;
    }

    char   tmp[CONFIG_VFS_PATH_MAX];
    size_t len = vfs_readlink(node, tmp, sizeof(tmp));
    vfs_close(node);
    if (len > bufsiz) len = bufsiz;
    if (len && copy_to_user((void *)buf, tmp, len)) return -EFAULT;

    return (int64_t)len;
}

/* System call handler for `readlinkat`. */
int64_t sys_readlinkat(uint64_t dirfd, uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    if (!buf && bufsiz) return -EFAULT;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, input);

    if (ret != EOK) return ret;
    vfs_node_t node = input[0] ? open_path_at(proc, (int)dirfd, path, true, &ret) : open_empty_path_at(proc, (int)dirfd, &ret);

    if (!node) return ret;
    if (!(node->type & file_symlink)) {
        vfs_close(node);
        return -EINVAL;
    }
    char   tmp[CONFIG_VFS_PATH_MAX];
    size_t len = vfs_readlink(node, tmp, sizeof(tmp));
    vfs_close(node);

    if (len > bufsiz) len = bufsiz;
    if (len && copy_to_user((void *)buf, tmp, len)) return -EFAULT;
    return (int64_t)len;
}

/* System call handler for `getcwd`. */
int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/*
 * Minimal name_to_handle_at for systemd chroot detection.
 * systemd's running_in_chroot() uses this to compare "/" and "/proc/1/root".
 * For PID1 both must return the same handle and mount_id, otherwise it thinks
 * it is in a chroot and refuses to run.  The simplest correct stub derives a
 * file-handle from the underlying inode.
 */
int64_t sys_name_to_handle_at_impl(uint64_t dirfd, uint64_t path, uint64_t handle, uint64_t mount_id, uint64_t flags, uint64_t arg5)
{
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!handle || !mount_id) return -EFAULT;

    char tgt[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)dirfd, path, tgt);
    if (ret != EOK) return ret;

    /* Resolve the target to a VFS node to derive handle/mount_id. */
    int        err  = EOK;
    vfs_node_t node = vfs_open_checked(tgt, &err);
    if (!node) {
        /* Fallback for AT_EMPTY_PATH with fd */
        if ((flags & AT_EMPTY_PATH) && !tgt[0]) {
            node = open_empty_path_at(proc, (int)dirfd, &err);
            if (!node) return err;
        } else {
            return err;
        }
    }

    vfs_update(node);
    uint64_t ino = node->inode;
    uint64_t dev = node->dev; // the uapi file handle stores the device number in 8 bytes
    uint64_t mid = node->mount_id ? node->mount_id : 1;
    vfs_close(node);

    struct {
            unsigned int  handle_bytes;
            int           handle_type;
            unsigned char f_handle[128];
    } h;
    if (copy_from_user(&h.handle_bytes, (const void *)handle, sizeof(h.handle_bytes))) return -EFAULT;

    /* First probe with handle_bytes==0 or too small: return required size */
    if (h.handle_bytes < 8) {
        h.handle_bytes = 8;
        h.handle_type  = 1;
        if (copy_to_user((void *)handle, &h, sizeof(h.handle_bytes) + sizeof(h.handle_type))) return -EFAULT;
        return -EOVERFLOW;
    }

    unsigned int out_bytes = (h.handle_bytes >= 16) ? 16 : 8;
    h.handle_bytes         = out_bytes;
    h.handle_type          = 1;
    memcpy(h.f_handle, &ino, sizeof(ino));

    if (out_bytes >= 16) memcpy(h.f_handle + 8, &dev, sizeof(dev));
    if (copy_to_user((void *)handle, &h, sizeof(h.handle_bytes) + sizeof(h.handle_type) + out_bytes)) return -EFAULT;
    int m = (int)mid;
    if (copy_to_user((void *)mount_id, &m, sizeof(m))) return -EFAULT;

    /* Also support a 64-bit mount_id: the caller may pass a uint64_t* for the unique mount id, but only 32 bits exist. */
    return 0;
}

/* System call handler for `open_by_handle_at`. */
int64_t sys_open_by_handle_at_impl(uint64_t dirfd, uint64_t handle, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd;
    (void)handle;
    (void)flags;
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
    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, input);

    if (ret != EOK) return ret;
    vfs_node_t node = NULL;

    if (!input[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        node = open_empty_path_at(proc, dirfd, &ret);
    } else {
        char name[CONFIG_VFS_PATH_MAX];
        ret = process_resolve_path_at(proc, dirfd, input, name, sizeof(name));
        if (ret == EOK) node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow(name) : vfs_open(name);
        if (!node && ret == EOK) ret = -ENOENT;
    }
    if (!node) return ret;

    /* R_OK=4, W_OK=2, X_OK=1, F_OK=0 */
    if (mode & 4) // R_OK
        if (vfs_access_check(node, VFS_ACCESS_R)) {
            vfs_close(node);
            return -EACCES;
        }
    if (mode & 2) // W_OK
        if (vfs_access_check(node, VFS_ACCESS_W)) {
            vfs_close(node);
            return -EACCES;
        }
    if (mode & 1) // X_OK
        if (vfs_access_check(node, VFS_ACCESS_X)) {
            vfs_close(node);
            return -EACCES;
        }

    vfs_close(node);
    return EOK;
}

/* System call handler for `access`. */
int64_t sys_access_impl(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_access_common(AT_FDCWD, path, mode, 0);
}

/* System call handler for `faccessat`. */
int64_t sys_faccessat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_access_common((int)dirfd, path, mode, 0);
}

/* System call handler for `faccessat2`. */
int64_t sys_faccessat2_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_access_common((int)dirfd, path, mode, flags);
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

/* System call handler for `chmod`. */
int64_t sys_chmod_impl(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;

    return sys_chmod_common(name, mode);
}

/* System call handler for `fchmod`. */
int64_t sys_fchmod_impl(uint64_t fd, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
int64_t sys_fchmodat2_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (flags & ~(uint64_t)AT_SYMLINK_NOFOLLOW) return -EINVAL;

    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, (int)dirfd, path, name);
    if (ret != EOK) return ret;

    int        lookup_error = EOK;
    vfs_node_t node         = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow_checked(name, &lookup_error) : vfs_open_checked(name, &lookup_error);
    if (!node) return lookup_error;

    int result = vfs_chmod_process(node, (uint16_t)mode, proc);
    vfs_close(node);
    return result;
}

/* System call handler for `chown_common`. */
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

/* System call handler for `chown`. */
int64_t sys_chown_impl(uint64_t path, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;

    return sys_chown_common(name, owner, group, false);
}

/* System call handler for `lchown`. */
int64_t sys_lchown_impl(uint64_t path, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;

    return sys_chown_common(name, owner, group, true);
}

/* System call handler for `fchown`. */
int64_t sys_fchown_impl(uint64_t fd, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* Create the node described by mode at an already-resolved path. Shared by mknod (AT_FDCWD) and mknodat so the type dispatch cannot drift. */
int64_t mknod_create_node(char *resolved, uint64_t mode, uint64_t dev)
{
    vfs_node_t existing = vfs_open(resolved);
    if (existing) {
        vfs_close(existing);
        return -EEXIST;
    }

    process_t *proc = process_current();
    if (proc) mode &= ~(uint64_t)(proc->umask & 0777); // permission bits only
    switch (mode & 0170000) {
        case 0010000 : // FIFO
            return pipe_mknod(resolved, (uint16_t)mode);
        case 0 :       // no type bits means a regular file
        case 0100000 : // regular file
            return vfs_mkfile_mode(resolved, (uint16_t)(mode & 07777));
        case 0140000 : {
            /*
             * A socket inode that no endpoint is bound to: connect() on it is
             * refused, but it has the right type for stat().  systemd creates
             * one as the template it bind-mounts over inaccessible sockets.
             */
            int status = vfs_mkfile_mode(resolved, (uint16_t)(mode & 07777));
            if (status != EOK) return status;

            vfs_node_t node = vfs_open_nofollow(resolved);
            if (!node) return -EIO;

            status = tmpfs_set_node_type(node, file_socket);
            if (status != EOK) (void)vfs_delete(node);

            vfs_close(node);
            return status;
        }
        case 0020000 : // character device
        case 0060000 : // block device
            /* A device node hands its opener the driver bound to the device number. */
            if (!proc || proc->uid != 0) return -EPERM;
            return devtmpfs_mknod(resolved, (uint16_t)mode, dev_decode_uapi(dev));
        default :
            return -EINVAL;
    }
}

/* System call handler for `mknod`. */
int64_t sys_mknod_impl(uint64_t path, uint64_t mode, uint64_t dev, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, name);
    if (ret != EOK) return ret;

    return mknod_create_node(name, mode, dev);
}

/* statfs f_type of a node: its registered filesystem type's magic, or the anonymous type it belongs to (sockets and pipes carry no fsid). */
static int64_t linux_statfs_type(vfs_node_t node)
{
    if (!node) return 0;

    uint32_t magic = vfs_filesystem_magic(node->fsid);
    if (magic) return magic;
    if (node->type & file_socket) return SOCKFS_MAGIC;
    if (node->type & file_pipe) return PIPEFS_MAGIC;
    return 0;
}

/* Build a statfs structure and copy it to user space */
static int64_t copy_statfs_to_user(vfs_node_t node, uint64_t buf)
{
    if (!node) return -EINVAL;

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
int64_t sys_statfs_impl(uint64_t path, uint64_t buf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!path || !buf) return -EFAULT;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    char resolved[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, path, resolved);

    if (ret != EOK) return ret;
    vfs_node_t node = vfs_open(resolved);

    if (!node) return -ENOENT;
    ret = (int)copy_statfs_to_user(node, buf);

    vfs_close(node);
    return ret;
}

/* fstatfs syscall: report filesystem statistics by fd */
int64_t sys_fstatfs_impl(uint64_t fd, uint64_t buf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* flock syscall: validate the fd (no mandatory locks) */
int64_t sys_flock_impl(uint64_t fd, uint64_t operation, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)operation;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf) return -EBADF;

    process_file_put(pf);
    return 0;
}

/* Set file times by dirfd and path */
static int set_times_at(process_t *proc, int dirfd, uint64_t upath, const linux_timespec_t requested[2], uint64_t flags)
{
    if (!proc) return -ESRCH;
    int64_t  atime      = timer_realtime_seconds();
    int64_t  mtime      = atime;
    uint32_t time_flags = VFS_SET_TIME_ATIME | VFS_SET_TIME_MTIME;

    if (requested) {
        const int64_t  nanoseconds[2] = {requested[0].tv_nsec, requested[1].tv_nsec};
        int64_t       *seconds[2]     = {&atime, &mtime};
        const uint32_t bits[2]        = {VFS_SET_TIME_ATIME, VFS_SET_TIME_MTIME};

        for (size_t i = 0; i < 2; i++) {
            if (nanoseconds[i] == LINUX_UTIME_OMIT) {
                time_flags &= ~bits[i];
                continue;
            }
            if (nanoseconds[i] == LINUX_UTIME_NOW) continue;
            if (nanoseconds[i] < 0 || nanoseconds[i] >= (int64_t)TIMER_NSEC_PER_SEC) return -EINVAL;
            *seconds[i] = requested[i].tv_sec;
            time_flags |= VFS_SET_TIME_EXPLICIT;
        }

        /* When both fields are omitted, do not resolve the path. */
        if (!(time_flags & (VFS_SET_TIME_ATIME | VFS_SET_TIME_MTIME))) return EOK;
    }
    if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;

    vfs_node_t      node = NULL;
    process_file_t *file = NULL;
    int             ret  = EOK;

    if (!upath) {
        if (dirfd == PROCESS_AT_FDCWD) return -EFAULT;
        if (flags) return -EINVAL;
        file = process_fd_get(proc, dirfd);

        if (!file) return -EBADF;
        node = file->node;
    } else {
        char input[CONFIG_VFS_PATH_MAX];
        ret = copy_path_from_user(upath, input);

        if (ret != EOK) return ret;
        if (!input[0]) {
            if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
            if (dirfd == PROCESS_AT_FDCWD) {
                char resolved[CONFIG_VFS_PATH_MAX];
                ret = process_resolve_path_at(proc, PROCESS_AT_FDCWD, ".", resolved, sizeof(resolved));
                if (ret == EOK) node = vfs_open_checked(resolved, &ret);
            } else {
                file = process_fd_get(proc, dirfd);
                if (!file) return -EBADF;
                node = file->node;
            }
        } else {
            char resolved[CONFIG_VFS_PATH_MAX];
            ret = process_resolve_path_at(proc, dirfd, input, resolved, sizeof(resolved));
            if (ret == EOK) node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow_checked(resolved, &ret) : vfs_open_checked(resolved, &ret);
        }
    }
    if (!node) {
        if (file) process_file_put(file);
        return ret == EOK ? -ENOENT : ret;
    }

    vfs_update(node);
    ret = vfs_set_times_process(node, atime, mtime, time_flags, proc);
    if (file) {
        process_file_put(file);
    } else {
        vfs_close(node);
    }

    return ret;
}

/* utime syscall: set file access and modification times */
int64_t sys_utime_impl(uint64_t filename, uint64_t times, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    linux_timespec_t requested[2];
    linux_utimbuf_t  legacy;
    if (times) {
        if (copy_from_user(&legacy, (const void *)times, sizeof(legacy))) return -EFAULT;
        requested[0].tv_sec  = legacy.actime;
        requested[0].tv_nsec = 0;
        requested[1].tv_sec  = legacy.modtime;
        requested[1].tv_nsec = 0;
    }

    return set_times_at(proc, PROCESS_AT_FDCWD, filename, times ? requested : NULL, 0);
}

/* utimes syscall: set file timestamps with microsecond resolution */
int64_t sys_utimes_impl(uint64_t filename, uint64_t times, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    linux_timespec_t requested[2];
    linux_timeval_t  legacy[2];
    if (times) {
        if (copy_from_user(legacy, (const void *)times, sizeof(legacy))) return -EFAULT;
        for (size_t i = 0; i < 2; i++) {
            if (legacy[i].tv_usec < 0 || legacy[i].tv_usec >= 1000000) return -EINVAL;
            requested[i].tv_sec  = legacy[i].tv_sec;
            requested[i].tv_nsec = legacy[i].tv_usec * 1000;
        }
    }

    return set_times_at(proc, PROCESS_AT_FDCWD, filename, times ? requested : NULL, 0);
}

/* fchownat syscall: change ownership by dirfd+path */
int64_t sys_fchownat_impl(uint64_t dirfd, uint64_t path, uint64_t owner, uint64_t group, uint64_t flags, uint64_t arg5)
{
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;

    vfs_node_t      node = NULL;
    process_file_t *file = NULL;
    int             ret  = EOK;
    if (!path) {
        if (!(flags & AT_EMPTY_PATH) || (int)dirfd == PROCESS_AT_FDCWD) return -EFAULT;
        file = process_fd_get(proc, (int)dirfd);

        if (!file) return -EBADF;
        node = file->node;
    } else {
        char input[CONFIG_VFS_PATH_MAX];
        ret = copy_path_from_user(path, input);

        if (ret != EOK) return ret;
        if (!input[0]) {
            if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
            if ((int)dirfd == PROCESS_AT_FDCWD) {
                char resolved[CONFIG_VFS_PATH_MAX];
                ret = process_resolve_path_at(proc, PROCESS_AT_FDCWD, ".", resolved, sizeof(resolved));
                if (ret == EOK) node = vfs_open_checked(resolved, &ret);
            } else {
                file = process_fd_get(proc, (int)dirfd);
                if (!file) return -EBADF;
                node = file->node;
            }
        } else {
            char resolved[CONFIG_VFS_PATH_MAX];
            ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
            if (ret == EOK) node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_nofollow_checked(resolved, &ret) : vfs_open_checked(resolved, &ret);
        }
    }
    if (!node) {
        if (file) process_file_put(file);
        return ret == EOK ? -ENOENT : ret;
    }

    vfs_update(node);
    ret = vfs_chown_process(node, (uint32_t)owner, (uint32_t)group, proc);
    if (file) {
        process_file_put(file);
    } else {
        vfs_close(node);
    }

    return ret;
}

/* futimesat syscall: set timestamps by dirfd+path */
int64_t sys_futimesat_impl(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    linux_timespec_t requested[2];
    linux_timeval_t  legacy[2];
    if (times) {
        if (copy_from_user(legacy, (const void *)times, sizeof(legacy))) return -EFAULT;
        for (size_t i = 0; i < 2; i++) {
            if (legacy[i].tv_usec < 0 || legacy[i].tv_usec >= 1000000) return -EINVAL;
            requested[i].tv_sec  = legacy[i].tv_sec;
            requested[i].tv_nsec = legacy[i].tv_usec * 1000;
        }
    }

    return set_times_at(proc, (int)dirfd, path, times ? requested : NULL, 0);
}

/* Legacy fchmodat takes three arguments; flags belong to fchmodat2. */
int64_t sys_fchmodat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret != 0) return ret;
    if (!input[0]) return -ENOENT;

    char resolved[CONFIG_VFS_PATH_MAX];
    ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
    if (ret != 0) return ret;

    int        lookup_error = EOK;
    vfs_node_t node         = vfs_open_checked(resolved, &lookup_error);
    if (!node) return lookup_error;

    int result = vfs_chmod_process(node, (uint16_t)mode, proc);
    vfs_close(node);
    return result;
}

/* chdir syscall */
int64_t sys_chdir_impl(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
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
    if (node->type != file_dir) {
        vfs_close(node);
        return -ENOTDIR;
    }

    vfs_close(node);
    strncpy(proc->cwd, resolved, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    return 0;
}

/* fchdir syscall */
int64_t sys_fchdir_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf || !pf->node) {
        if (pf) process_file_put(pf);
        return -EBADF;
    }
    if (pf->node->type != file_dir) {
        process_file_put(pf);
        return -ENOTDIR;
    }

    char path[CONFIG_VFS_PATH_MAX];
    int  ret = vfs_node_path(pf->node, path, sizeof(path));
    if (ret != EOK) {
        process_file_put(pf);
        return ret;
    }

    strncpy(proc->cwd, path, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    process_file_put(pf);
    return 0;
}

/* listxattr syscall: no extended attributes */
int64_t sys_listxattr_impl(uint64_t path, uint64_t list, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)path;
    (void)list;
    (void)size;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return 0; // no extended attributes
}

/* setxattr syscall: unsupported */
int64_t sys_setxattr_impl(uint64_t path, uint64_t name, uint64_t value, uint64_t size, uint64_t flags, uint64_t arg5)
{
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    (void)flags;
    (void)arg5;
    return -ENOSYS;
}

/* getxattr syscall: no extended attributes */
int64_t sys_getxattr_impl(uint64_t path, uint64_t name, uint64_t value, uint64_t size, uint64_t arg4, uint64_t arg5)
{
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    (void)arg4;
    (void)arg5;
    return -ENODATA; // no xattrs -> name not found
}

/* removexattr syscall: no extended attributes */
int64_t sys_removexattr_impl(uint64_t path, uint64_t name, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)path;
    (void)name;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENODATA;
}

/* renameat2 syscall */
int64_t sys_renameat2_impl(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)arg5;

    if (flags & ~3ULL) return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE)) return -EINVAL;
    if (!oldpath || !newpath) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char old_name[CONFIG_VFS_PATH_MAX], new_name[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(oldpath, old_name);
    if (ret) return ret;

    ret = copy_path_from_user(newpath, new_name);
    if (ret) return ret;

    char old_resolved[CONFIG_VFS_PATH_MAX], new_resolved[CONFIG_VFS_PATH_MAX];
    ret = process_resolve_path_at(proc, (int)olddirfd, old_name, old_resolved, sizeof(old_resolved));
    if (ret) return ret;

    ret = process_resolve_path_at(proc, (int)newdirfd, new_name, new_resolved, sizeof(new_resolved));
    if (ret) return ret;
    if (flags & RENAME_EXCHANGE) return -EOPNOTSUPP;

    vfs_node_t node = vfs_open_nofollow(old_resolved);
    if (!node) return -ENOENT;

    vfs_node_t old_dir = vfs_open_parent_of(old_resolved);
    vfs_node_t new_dir = vfs_open_parent_of(new_resolved);
    if (!old_dir || !new_dir) {
        ret = -ENOENT;
    } else {
        ret = vfs_rename(node, new_dir, path_basename(new_resolved), (flags & RENAME_NOREPLACE) ? VFS_RENAME_NOREPLACE : 0);
    }
    if (old_dir) vfs_close(old_dir);
    if (new_dir) vfs_close(new_dir);

    vfs_close(node);
    return ret;
}

/* utimensat syscall */
int64_t sys_utimensat_impl(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    linux_timespec_t requested[2];
    if (times && copy_from_user(requested, (const void *)times, sizeof(requested))) return -EFAULT;

    return set_times_at(proc, (int)dirfd, path, times ? requested : NULL, flags);
}

/* mknodat syscall */
int64_t sys_mknodat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t dev, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[CONFIG_VFS_PATH_MAX];
    int  ret = copy_path_from_user(path, input);
    if (ret != EOK) return ret;

    char resolved[CONFIG_VFS_PATH_MAX];
    ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
    if (ret != EOK) return ret;

    return mknod_create_node(resolved, mode, dev);
}
