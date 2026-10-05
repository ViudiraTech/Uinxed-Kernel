/*
 *
 *      mount_api.c
 *      File-descriptor based mount attribute changes (mount_setattr)
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <process/process.h>
#include <process/namespace.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <security/capability.h>
#include <syscall/syscall.h>


/* Linux x86-64 struct mount_attr, version 0. */
typedef struct {
        uint64_t attr_set;
        uint64_t attr_clr;
        uint64_t propagation;
        uint64_t userns_fd;
} mount_attr_t;

#define MOUNT_ATTR_SIZE_VER0 32

#define MOUNT_ATTR_RDONLY 0x00000001ULL
#define MOUNT_ATTR_NOSUID 0x00000002ULL
#define MOUNT_ATTR_NODEV  0x00000004ULL
#define MOUNT_ATTR_NOEXEC 0x00000008ULL

/* The attributes this kernel models on a mount point. */
#define MOUNT_ATTR_SUPPORTED (MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC)

#define ATTR_SYMLINK_NOFOLLOW 0x100U
#define ATTR_EMPTY_PATH       0x1000U
#define ATTR_RECURSIVE        0x8000U

/* struct mount_attr.propagation carries the MS_* value unchanged. */
#define MS_UNBINDABLE (1ULL << 17)
#define MS_PRIVATE    (1ULL << 18)
#define MS_SLAVE      (1ULL << 19)
#define MS_SHARED     (1ULL << 20)

static uint64_t attr_to_mount_flags(uint64_t attr)
{
    uint64_t flags = 0;
    if (attr & MOUNT_ATTR_RDONLY) flags |= MOUNT_FLAG_RDONLY;
    if (attr & MOUNT_ATTR_NOSUID) flags |= MOUNT_FLAG_NOSUID;
    if (attr & MOUNT_ATTR_NODEV) flags |= MOUNT_FLAG_NODEV;
    if (attr & MOUNT_ATTR_NOEXEC) flags |= MOUNT_FLAG_NOEXEC;
    return flags;
}

/*
 * Resolve a (dirfd, path) pair to a node, honouring AT_EMPTY_PATH.  When the
 * caller needs it, also report the mount the walk arrived through: a bind must
 * attach into that mount, not into whichever mount owns the dentry.
 */
static vfs_node_t resolve_mount_path(process_t *proc, int dfd, uint64_t path, uint64_t flags, uint64_t *mount_out)
{
    char input[CONFIG_VFS_PATH_MAX];
    if (mount_out) *mount_out = 0;
    input[0] = '\0';
    if (path && copy_path_from_user(path, input) != EOK) return NULL;
    if (!input[0]) {
        if (!(flags & ATTR_EMPTY_PATH)) return NULL;
        process_file_t *file = process_fd_get(proc, dfd);
        if (!file) return NULL;
        vfs_node_t node = vfs_node_retain(file->node);
        if (mount_out) *mount_out = file->mount_id;
        process_file_put(file);
        return node;
    }
    char resolved[CONFIG_VFS_PATH_MAX];
    if (process_resolve_path_at(proc, dfd, input, resolved, sizeof(resolved)) != EOK) return NULL;
    if (flags & ATTR_SYMLINK_NOFOLLOW) return vfs_open_nofollow(resolved);
    return vfs_open_mount(resolved, mount_out);
}

/* Marks an open_tree(OPEN_TREE_CLONE) fd: the move_mount that consumes it must
 * attach a new mount instead of relocating the caller's.  Above every O_* bit. */
#define FILE_FLAG_OPEN_TREE_CLONE      (1ULL << 40)
#define FILE_FLAG_OPEN_TREE_RECURSIVE  (1ULL << 41)

/* open_tree(2).  OPEN_TREE_CLONE cannot be refused: util-linux and systemd try
 * the new API first and treat anything but ENOSYS as fatal. */
int64_t sys_open_tree(uint64_t dfd, uint64_t path, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
#define OPEN_TREE_CLONE   1U
#define OPEN_TREE_CLOEXEC 0x80000U
    if (flags & ~(uint64_t)(ATTR_EMPTY_PATH | ATTR_RECURSIVE | ATTR_SYMLINK_NOFOLLOW | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)) return -EINVAL;
    if ((flags & OPEN_TREE_CLONE) && !capability_ns(current_task(), mnt_namespace_current()->ns.owner, CAP_SYS_ADMIN)) return -EPERM;

    uint64_t mount_id = 0;
    vfs_node_t node = resolve_mount_path(proc, (int)dfd, path, flags, &mount_id);
    if (!node) return path ? -ENOENT : -EBADF;
    /* Without OPEN_TREE_CLONE the caller wants the mount itself, not a copy. */
    if (!(flags & OPEN_TREE_CLONE) && !vfs_is_mountpoint(node)) {
        vfs_close(node);
        return -EINVAL;
    }
    int fd = process_fd_install_at(proc, node, (flags & OPEN_TREE_CLOEXEC) ? O_CLOEXEC : 0, mount_id);
    if (fd < 0) {
        vfs_close(node);
        return fd;
    }
    if (flags & OPEN_TREE_CLONE) {
        process_file_t *file = process_fd_get(proc, fd);
        if (file) {
            file->flags |= FILE_FLAG_OPEN_TREE_CLONE;
            if (flags & ATTR_RECURSIVE) file->flags |= FILE_FLAG_OPEN_TREE_RECURSIVE;
            process_file_put(file);
        }
    }
    return fd;
#undef OPEN_TREE_CLONE
#undef OPEN_TREE_CLOEXEC
}

/* move_mount(2): relocate a mount onto an empty mount point in the same namespace. */
int64_t sys_move_mount(uint64_t from_dfd, uint64_t from_path, uint64_t to_dfd, uint64_t to_path, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!capability_ns(current_task(), mnt_namespace_current()->ns.owner, CAP_SYS_ADMIN)) return -EPERM;
#define MOVE_MOUNT_F_EMPTY_PATH 4U
#define MOVE_MOUNT_T_EMPTY_PATH 8U
#define MOVE_MOUNT_F_AUTOMOUNTS 0x10U
#define MOVE_MOUNT_T_AUTOMOUNTS 0x20U
#define MOVE_MOUNT_SET_GROUP    0x100U
#define MOVE_MOUNT_BENEATH      0x200U
    if (flags & ~(uint64_t)(MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH | MOVE_MOUNT_F_AUTOMOUNTS | MOVE_MOUNT_T_AUTOMOUNTS)) return -EINVAL;
    /* Neither mount groups nor moving a mount beneath another are modelled. */
    if (flags & (MOVE_MOUNT_SET_GROUP | MOVE_MOUNT_BENEATH)) return -EOPNOTSUPP;

    /* A clone descriptor attaches as a new mount rather than relocating one. */
    bool cloned = false;
    bool recursive = false;
    if (flags & MOVE_MOUNT_F_EMPTY_PATH) {
        process_file_t *file = process_fd_get(proc, (int)from_dfd);
        if (file) {
            cloned    = (file->flags & FILE_FLAG_OPEN_TREE_CLONE) != 0;
            recursive = (file->flags & FILE_FLAG_OPEN_TREE_RECURSIVE) != 0;
            process_file_put(file);
        }
    }

    uint64_t   source_flags = (flags & MOVE_MOUNT_F_EMPTY_PATH) ? ATTR_EMPTY_PATH : 0;
    uint64_t   target_flags = (flags & MOVE_MOUNT_T_EMPTY_PATH) ? ATTR_EMPTY_PATH : 0;
    uint64_t target_mount = 0;
    uint64_t source_mount = 0;
    vfs_node_t source       = resolve_mount_path(proc, (int)from_dfd, from_path, source_flags, &source_mount);
    if (!source) return from_path ? -ENOENT : -EBADF;
    vfs_node_t target = resolve_mount_path(proc, (int)to_dfd, to_path, target_flags, &target_mount);
    if (!target) {
        vfs_close(source);
        return to_path ? -ENOENT : -EBADF;
    }

    int result = cloned ? vfs_bind_mount(source, target, recursive ? 16384ULL : 0, target_mount, source_mount) : vfs_move_mount_at(source, target, source_mount, target_mount);
    vfs_close(target);
    vfs_close(source);
    return result;
#undef MOVE_MOUNT_F_EMPTY_PATH
#undef MOVE_MOUNT_T_EMPTY_PATH
#undef MOVE_MOUNT_F_AUTOMOUNTS
#undef MOVE_MOUNT_T_AUTOMOUNTS
#undef MOVE_MOUNT_SET_GROUP
#undef MOVE_MOUNT_BENEATH
}

int64_t sys_mount_setattr(uint64_t dfd, uint64_t path, uint64_t flags, uint64_t uattr, uint64_t usize, uint64_t arg5)
{
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (flags & ~(uint64_t)(ATTR_EMPTY_PATH | ATTR_RECURSIVE | ATTR_SYMLINK_NOFOLLOW)) return -EINVAL;
    /* Linux checks the size before the pointer, so the support probe
     * mount_setattr(-1, NULL, 0, NULL, 0) sees EINVAL rather than EFAULT. */
    if (usize < MOUNT_ATTR_SIZE_VER0) return -EINVAL;
    if (!capability_ns(current_task(), mnt_namespace_current()->ns.owner, CAP_SYS_ADMIN)) return -EPERM;
    if (!uattr) return -EFAULT;

    /*
     * Reject a truncated copy the caller does not own, and ignore any extension
     * bytes: a zero value there means "not present", so trailing bytes must be 0.
     */
    size_t copy_size = usize > sizeof(mount_attr_t) ? usize : sizeof(mount_attr_t);
    if (copy_size > sizeof(mount_attr_t)) return -EINVAL;

    mount_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    if (copy_from_user(&attr, (const void *)uattr, copy_size)) return -EFAULT;

    if (attr.attr_set & attr.attr_clr) return -EINVAL;
    if ((attr.attr_set | attr.attr_clr) & ~MOUNT_ATTR_SUPPORTED) return -EOPNOTSUPP;
    /* Idmapped mounts do not exist in this kernel. */
    if (attr.userns_fd) return -EOPNOTSUPP;
    /* Zero leaves the propagation type alone; anything else must be one of the four. */
    if (attr.propagation & ~(uint64_t)(MS_UNBINDABLE | MS_PRIVATE | MS_SLAVE | MS_SHARED)) return -EINVAL;
    if (attr.propagation && (attr.propagation & (attr.propagation - 1))) return -EINVAL;

    char input[CONFIG_VFS_PATH_MAX];
    input[0] = '\0';
    if (path) {
        int ret = copy_path_from_user(path, input);
        if (ret != EOK) return ret;
    } else if (!(flags & ATTR_EMPTY_PATH))
        return -EFAULT;

    vfs_node_t node = NULL;
    uint64_t mount_id = 0;
    if ((flags & ATTR_EMPTY_PATH) && !input[0]) {
        process_file_t *file = process_fd_get(proc, (int)dfd);
        if (!file) return -EBADF;
        node = vfs_node_retain(file->node);
        mount_id = file->mount_id;
        process_file_put(file);
        if (!node) return -EBADF;
    } else {
        char resolved[CONFIG_VFS_PATH_MAX];
        int  ret = process_resolve_path_at(proc, (int)dfd, input, resolved, sizeof(resolved));
        if (ret != EOK) return ret;
        node = vfs_open_checked_at(resolved, (flags & ATTR_SYMLINK_NOFOLLOW) != 0, &ret, &mount_id);
        if (!node) return -ENOENT;
    }

    if (!vfs_is_mountpoint(node)) {
        vfs_close(node);
        return -EINVAL;
    }

    uint64_t set_flags = attr_to_mount_flags(attr.attr_set);
    uint64_t clr_flags = attr_to_mount_flags(attr.attr_clr);
    bool     recursive = (flags & ATTR_RECURSIVE) != 0;

    uint32_t type = UINT32_MAX;
    if (attr.propagation == MS_SHARED) type = VFS_MOUNT_SHARED;
    else if (attr.propagation == MS_PRIVATE) type = VFS_MOUNT_PRIVATE;
    else if (attr.propagation == MS_SLAVE) type = VFS_MOUNT_SLAVE;
    else if (attr.propagation) type = VFS_MOUNT_UNBINDABLE;
    int result = vfs_mount_update(node, mount_id, set_flags, clr_flags, type, recursive);
    vfs_close(node);
    return result;
}
