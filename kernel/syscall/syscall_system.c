/*
 *
 *      syscall_system.c
 *      Credentials, scheduling, mounts, modules and system information
 *
 *      2026/11/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/smp.h>
#include <arch/misc/common.h>
#include <drivers/firmware/acpi.h>
#include <fs/core/vfs.h>
#include <kernel/abi.h>
#include <kernel/debug/printk.h>
#include <kernel/module/module.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/swap.h>
#include <process/namespace.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>

#define SYSCALL_MODULE_MAX_SIZE ((size_t)CONFIG_MODULE_MAX_SIZE * 1024U * 1024U)

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

/* personality */
#define PER_LINUX 0x0000

/* process credentials */
#define CREDENTIAL_ID_UNCHANGED UINT32_MAX

/* capget / capset */
typedef struct linux_cap_header {
        uint32_t version;
        int32_t  pid;
} linux_cap_header_t;

typedef struct linux_cap_data {
        uint32_t effective;
        uint32_t permitted;
        uint32_t inheritable;
} linux_cap_data_t;

/* uname syscall: report kernel identity */
int64_t sys_uname(uint64_t name, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!name) return -EFAULT;

    linux_utsname_t  uts;
    uts_namespace_t *ns = uts_namespace_current();
    char             nodename[65];
    char             domainname[65];

    memset(&uts, 0, sizeof(uts));
    spin_lock(&ns->ns.lock);
    memcpy(nodename, ns->nodename, sizeof(nodename));
    memcpy(domainname, ns->domainname, sizeof(domainname));
    spin_unlock(&ns->ns.lock);

    strncpy(uts.sysname, KERNEL_NAME, sizeof(uts.sysname) - 1);
    strncpy(uts.nodename, nodename, sizeof(uts.nodename) - 1);
    strncpy(uts.release, KERNEL_VERSION, sizeof(uts.release) - 1);
    strncpy(uts.version, BUILD_DATE " " BUILD_TIME, sizeof(uts.version) - 1);
    strncpy(uts.machine, "x86_64", sizeof(uts.machine) - 1);
    strncpy(uts.domainname, domainname, sizeof(uts.domainname) - 1);

    return copy_to_user((void *)name, &uts, sizeof(uts)) ? -EFAULT : EOK;
}

/* System call handler for `getuid`. */
int64_t sys_getuid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* System call handler for `getgid`. */
int64_t sys_getgid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* mount syscall: attach a filesystem */
int64_t sys_mount(uint64_t source, uint64_t target, uint64_t fstype, uint64_t flags, uint64_t data, uint64_t arg5)
{
    (void)data;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;

    char src[CONFIG_VFS_PATH_MAX] = {0};
    char tgt[CONFIG_VFS_PATH_MAX] = {0};
    char fst[CONFIG_VFS_PATH_MAX] = {0};
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

    /* Bind mounts may target regular files, as used by systemd's namespace setup. */
    if (flags & MS_BIND) {
        vfs_close(node);
        return EOK;
    }

    if (!(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }

    /* Handle MS_REMOUNT: change flags on an existing mount */
    if (flags & MS_REMOUNT) {
        if (!node->is_mount && node != rootdir) {
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

        /*
         * Virtual filesystems systemd expects but which are not implemented yet:
         * provide a tmpfs-backed stub so the mount point exists and the subsequent
         * open/write succeeds.
         */
        if (ret == -ENOENT) {
            bool is_stub_fs = !strcmp(fst, "securityfs") || !strcmp(fst, "selinuxfs") || !strcmp(fst, "bpf") || !strcmp(fst, "bpffs") || !strcmp(fst, "debugfs") || !strcmp(fst, "tracefs")
                              || !strcmp(fst, "hugetlbfs") || !strcmp(fst, "mqueue") || !strcmp(fst, "fusectl") || !strcmp(fst, "configfs") || !strcmp(fst, "binfmt_misc") || !strcmp(fst, "autofs")
                              || !strcmp(fst, "efivarfs") || !strcmp(fst, "ramfs") || !strcmp(fst, "devpts") || !strcmp(fst, "fuse") || !strcmp(fst, "overlay") || !strcmp(fst, "nsfs")
                              || !strcmp(fst, "cgroup");
            if (is_stub_fs) {
                /* cgroup (v1) -> cgroup2 on this kernel. */
                if (!strcmp(fst, "cgroup")) {
                    ret = vfs_mount_fs("cgroup2", src[0] ? src : NULL, node);
                } else {
                    ret = vfs_mount_fs("tmpfs", src[0] ? src : NULL, node);
                }
                if (ret == EOK) {
                    static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                    if (ratelimit_allow(&ratelimit)) plogk("mount: stubbed %s on %s as tmpfs\n", fst, tgt);
                } else if (ret == -EBUSY) {
                    ret = EOK;
                }
            }
        }
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
int64_t sys_umount2(uint64_t target, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char tgt[CONFIG_VFS_PATH_MAX];
    int  ret = copy_resolved_path_at(proc, AT_FDCWD, target, tgt);
    if (ret != EOK) return ret;
    if (flags & ~(MNT_FORCE | MNT_DETACH | MNT_EXPIRE)) return -EINVAL;

    /*
     * systemd's early umount of /proc with MNT_DETACH would detach the
     * procfs mount that PID1 needs for /proc/cmdline and the /proc/sys tree.
     * The VFS busy check correctly returns -EBUSY for a non-detach umount, but for
     * MNT_DETACH the current vfs_umount detaches even when busy and clears the
     * child list, breaking later lookups. For PID1, refuse to detach API
     * filesystems that are essential for its own operation.
     */
    if (proc && proc->task && proc->task->pid == 1 && (flags & MNT_DETACH) && (!strcmp(tgt, "/proc") || !strcmp(tgt, "/sys") || !strcmp(tgt, "/dev"))) return -EBUSY;

    int r = vfs_umount(tgt);
    return r;
}

/* reboot syscall: reboot, halt, or power off */
int64_t sys_reboot_impl(uint64_t magic, uint64_t magic2, uint64_t cmd, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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
            power_reset(); // never returns (ACPI reset + 8042/0xCF9 fallbacks)
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

    krn_halt();
}

/* personality syscall: return the fixed personality */
int64_t sys_personality_impl(uint64_t persona, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)persona;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return PER_LINUX;
}

/* sysinfo syscall: report system statistics */
int64_t sys_sysinfo_impl(uint64_t info, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
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

/* chroot syscall: change the root directory */
int64_t sys_chroot_wrap(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;

    char kpath[CONFIG_VFS_PATH_MAX];
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

/* Copy module parameter string from user space */
static int copy_module_params(uint64_t user_params, char params[CONFIG_MODULE_PARAM_MAX])
{
    if (!user_params) {
        params[0] = 0;
        return EOK;
    }

    int ret = strncpy_from_user(params, (const char *)user_params, CONFIG_MODULE_PARAM_MAX);
    return ret < 0 ? ret : EOK;
}

/* init_module syscall: load a kernel module */
int64_t sys_init_module_impl(uint64_t image, uint64_t length, uint64_t user_params, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *process = process_current();
    if (!process) return -ESRCH;
    if (process->uid != 0) return -EPERM;
    if (!image || !length) return -EINVAL;
    if (length > SYSCALL_MODULE_MAX_SIZE) return -EFBIG;

    char params[CONFIG_MODULE_PARAM_MAX];
    int  ret = copy_module_params(user_params, params);
    if (ret != EOK) return ret;

    void *copy = malloc((size_t)length);
    if (!copy) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("syscall: init_module image allocation failed (%llu bytes)\n", length);
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
int64_t sys_finit_module_impl(uint64_t fd, uint64_t user_params, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *process = process_current();
    if (!process) return -ESRCH;
    if (process->uid != 0) return -EPERM;

    char params[CONFIG_MODULE_PARAM_MAX];
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
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("syscall: finit_module image allocation failed (%zu bytes)\n", length);
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
int64_t sys_delete_module_impl(uint64_t user_name, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *process = process_current();
    if (!process) return -ESRCH;
    if (process->uid != 0) return -EPERM;

    char name[CONFIG_MODULE_NAME_LEN];
    if (!user_name) return -EFAULT;

    int ret = strncpy_from_user(name, (const char *)user_name, sizeof(name));
    if (ret < 0) return ret;
    return module_unload(name, (unsigned int)flags);
}

/* getgroups syscall: read supplementary groups */
int64_t sys_getgroups_impl(uint64_t size, uint64_t list, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint16_t count = proc->supplementary_group_count;
    if (size == 0) return count;
    if (size < count) return -EINVAL;
    if (count && !list) return -EFAULT;
    if (count && copy_to_user((void *)list, proc->supplementary_groups, (size_t)count * sizeof(uint32_t))) return -EFAULT;

    return count;
}

/* setgroups syscall: set supplementary groups */
int64_t sys_setgroups_impl(uint64_t size, uint64_t list, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;
    if (size > CONFIG_PROCESS_MAX_GROUPS) return -EINVAL;
    if (size && !list) return -EFAULT;

    uint32_t groups[CONFIG_PROCESS_MAX_GROUPS];
    if (size && copy_from_user(groups, (const void *)list, (size_t)size * sizeof(uint32_t))) return -EFAULT;
    if (size) memcpy(proc->supplementary_groups, groups, (size_t)size * sizeof(uint32_t));

    proc->supplementary_group_count = (uint16_t)size;
    return 0;
}

/* capget syscall: read process capabilities */
int64_t sys_capget_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!header) return -EFAULT;
    linux_cap_header_t hdr;

    if (copy_from_user(&hdr, (const void *)header, sizeof(hdr))) return -EFAULT;
    if (hdr.version != 0x20080522) return -EINVAL;
    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    process_t *target = (hdr.pid == 0 || hdr.pid == (int32_t)proc->task->pid) ? proc : process_find_get((pid_t)hdr.pid);

    if (!target) return -ESRCH;
    linux_cap_data_t caps = {0};

    if (target->uid == 0) caps.effective = caps.permitted = caps.inheritable = 0xFFFFFFFFu;
    if (target != proc) process_put(target);
    if (data && copy_to_user((void *)data, &caps, sizeof(caps))) return -EFAULT;

    return 0;
}

/* capset syscall: set process capabilities */
int64_t sys_capset_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)data;

    if (!header) return -EFAULT;
    linux_cap_header_t hdr;

    if (copy_from_user(&hdr, (const void *)header, sizeof(hdr))) return -EFAULT;
    if (hdr.version != 0x20080522) return -EINVAL;
    process_t *proc = process_current();

    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

/* getpriority syscall: return the default nice value */
int64_t sys_getpriority_impl(uint64_t which, uint64_t who, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)who;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (which > 2) return -EINVAL;
    return 20; // default nice value: 0 -> return 20
}

/* setpriority syscall: validate the nice value */
int64_t sys_setpriority_impl(uint64_t which, uint64_t who, uint64_t niceval, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)who;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (which > 2) return -EINVAL;
    if ((int64_t)niceval < -20 || (int64_t)niceval > 19) return -EINVAL;

    process_t *proc = process_current();

    if (!proc) return -ESRCH;
    if ((int64_t)niceval < 0 && proc->uid != 0) return -EACCES;

    return 0;
}

/* sched_getparam syscall */
int64_t sys_sched_getparam_impl(uint64_t pid, uint64_t param, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!param) return -EFAULT;

    struct {
            int32_t sched_priority;
    } p = {.sched_priority = 0};

    return copy_to_user((void *)param, &p, sizeof(p)) ? -EFAULT : 0;
}

/* sched_getscheduler syscall */
int64_t sys_sched_getscheduler_impl(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return 0; // SCHED_NORMAL
}

/* sched_get_priority_max syscall */
int64_t sys_sched_get_priority_max_impl(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (policy == 0) return 0;
    if (policy == 1 || policy == 2) return 99;
    return -EINVAL;
}

/* sched_get_priority_min syscall */
int64_t sys_sched_get_priority_min_impl(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (policy == 0) return 0;
    if (policy == 1 || policy == 2) return 1;
    return -EINVAL;
}

/* sched_rr_get_interval syscall */
int64_t sys_sched_rr_get_interval_impl(uint64_t pid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!tp) return -EFAULT;
    linux_timespec_t ts = {.tv_sec = 0, .tv_nsec = 100000000};
    return copy_to_user((void *)tp, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/* sched_setaffinity syscall */
int64_t sys_sched_setaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)cpusetsize;
    (void)mask;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

/* sched_getaffinity syscall */
int64_t sys_sched_getaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!mask) return -EFAULT;
    uint32_t ncpus    = get_cpu_count();
    uint8_t  buf[128] = {0};
    size_t   bytes    = (ncpus + 7) / 8;

    if (bytes > cpusetsize) bytes = cpusetsize;
    if (bytes > sizeof(buf)) bytes = sizeof(buf);

    memset(buf, 0xFF, bytes);
    if (ncpus % 8) buf[bytes - 1] &= (uint8_t)((1u << (ncpus % 8)) - 1u);

    return copy_to_user((void *)mask, buf, cpusetsize) ? -EFAULT : (int64_t)bytes;
}

/* sched_setattr syscall */
int64_t sys_sched_setattr_impl(uint64_t pid, uint64_t attr, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)attr;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (flags) return -EINVAL;
    process_t *proc = process_current();

    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

/* sched_getattr syscall */
int64_t sys_sched_getattr_impl(uint64_t pid, uint64_t attr, uint64_t size, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)pid;
    (void)attr;
    (void)size;
    (void)arg4;
    (void)arg5;
    if (flags) return -EINVAL;
    return 0;
}

/* sethostname syscall */
int64_t sys_sethostname_impl(uint64_t name, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!name) return -EFAULT;
    if (len > 64) return -EINVAL;

    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;

    char buffer[65];
    if (copy_from_user(buffer, (const void *)name, len)) return -EFAULT;
    buffer[len] = '\0';

    uts_namespace_t *uts = uts_namespace_current();
    spin_lock(&uts->ns.lock);
    memcpy(uts->nodename, buffer, len + 1);
    spin_unlock(&uts->ns.lock);

    return 0;
}

/* setdomainname syscall */
int64_t sys_setdomainname_impl(uint64_t name, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (!name) return -EFAULT;
    if (len > 64) return -EINVAL;

    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;

    char buffer[65];
    if (copy_from_user(buffer, (const void *)name, len)) return -EFAULT;
    buffer[len] = '\0';

    uts_namespace_t *uts = uts_namespace_current();
    spin_lock(&uts->ns.lock);
    memcpy(uts->domainname, buffer, len + 1);
    spin_unlock(&uts->ns.lock);

    return 0;
}

/* Check whether a UID transition is permitted */
static bool credential_uid_allowed(const process_t *proc, uint32_t uid)
{
    return uid == CREDENTIAL_ID_UNCHANGED || proc->uid == 0 || uid == proc->uid || uid == proc->fsuid;
}

/* Check whether a GID transition is permitted */
static bool credential_gid_allowed(const process_t *proc, uint32_t gid)
{
    return gid == CREDENTIAL_ID_UNCHANGED || proc->uid == 0 || gid == proc->gid || gid == proc->fsgid;
}

/* setuid syscall */
int64_t sys_setuid_impl(uint64_t uid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t requested = (uint32_t)uid;
    if (requested == CREDENTIAL_ID_UNCHANGED) return -EINVAL;
    if (!credential_uid_allowed(proc, requested)) return -EPERM;

    proc->uid   = requested;
    proc->fsuid = requested;
    return 0;
}

/* setgid syscall */
int64_t sys_setgid_impl(uint64_t gid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t requested = (uint32_t)gid;
    if (requested == CREDENTIAL_ID_UNCHANGED) return -EINVAL;
    if (!credential_gid_allowed(proc, requested)) return -EPERM;

    proc->gid   = requested;
    proc->fsgid = requested;
    return 0;
}

/* setreuid syscall */
int64_t sys_setreuid_impl(uint64_t ruid, uint64_t euid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t real = (uint32_t)ruid, effective = (uint32_t)euid;
    if (!credential_uid_allowed(proc, real) || !credential_uid_allowed(proc, effective)) return -EPERM;
    if (effective != CREDENTIAL_ID_UNCHANGED) {
        proc->uid   = effective;
        proc->fsuid = effective;
    }

    return 0;
}

/* setregid syscall */
int64_t sys_setregid_impl(uint64_t rgid, uint64_t egid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t real = (uint32_t)rgid, effective = (uint32_t)egid;
    if (!credential_gid_allowed(proc, real) || !credential_gid_allowed(proc, effective)) return -EPERM;
    if (effective != CREDENTIAL_ID_UNCHANGED) {
        proc->gid   = effective;
        proc->fsgid = effective;
    }

    return 0;
}

/* setresuid syscall */
int64_t sys_setresuid_impl(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t real = (uint32_t)ruid, effective = (uint32_t)euid, saved = (uint32_t)suid;
    if (!credential_uid_allowed(proc, real) || !credential_uid_allowed(proc, effective) || !credential_uid_allowed(proc, saved)) return -EPERM;
    if (effective != CREDENTIAL_ID_UNCHANGED) {
        proc->uid   = effective;
        proc->fsuid = effective;
    }

    return 0;
}

/* setresgid syscall */
int64_t sys_setresgid_impl(uint64_t rgid, uint64_t egid, uint64_t sgid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t real = (uint32_t)rgid, effective = (uint32_t)egid, saved = (uint32_t)sgid;
    if (!credential_gid_allowed(proc, real) || !credential_gid_allowed(proc, effective) || !credential_gid_allowed(proc, saved)) return -EPERM;
    if (effective != CREDENTIAL_ID_UNCHANGED) {
        proc->gid   = effective;
        proc->fsgid = effective;
    }

    return 0;
}

/* setfsuid syscall */
int64_t sys_setfsuid_impl(uint64_t uid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t old = proc->fsuid, requested = (uint32_t)uid;
    if (requested != CREDENTIAL_ID_UNCHANGED && credential_uid_allowed(proc, requested)) proc->fsuid = requested;

    return old;
}

/* setfsgid syscall */
int64_t sys_setfsgid_impl(uint64_t gid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t old = proc->fsgid, requested = (uint32_t)gid;
    if (requested != CREDENTIAL_ID_UNCHANGED && credential_gid_allowed(proc, requested)) proc->fsgid = requested;

    return old;
}

/* getresuid syscall */
int64_t sys_getresuid_impl(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t uid = proc->uid;
    if (ruid && copy_to_user((void *)ruid, &uid, sizeof(uid))) return -EFAULT;
    if (euid && copy_to_user((void *)euid, &uid, sizeof(uid))) return -EFAULT;
    if (suid && copy_to_user((void *)suid, &uid, sizeof(uid))) return -EFAULT;

    return 0;
}

/* getresgid syscall */
int64_t sys_getresgid_impl(uint64_t rgid, uint64_t egid, uint64_t sgid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint32_t gid = proc->gid;
    if (rgid && copy_to_user((void *)rgid, &gid, sizeof(gid))) return -EFAULT;
    if (egid && copy_to_user((void *)egid, &gid, sizeof(gid))) return -EFAULT;
    if (sgid && copy_to_user((void *)sgid, &gid, sizeof(gid))) return -EFAULT;

    return 0;
}

/* umask syscall */
int64_t sys_umask_impl(uint64_t mask, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint16_t old = proc->umask;
    proc->umask  = (uint16_t)(mask & 0777);
    return old;
}

/* getcpu syscall */
int64_t sys_getcpu_impl(uint64_t cpu, uint64_t node, uint64_t tcache, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tcache;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    task_t  *task = current_task();
    uint32_t c    = task ? task->cpu_id : 0;
    uint32_t n    = 0;

    if (cpu && copy_to_user((void *)cpu, &c, sizeof(c))) return -EFAULT;
    if (node && copy_to_user((void *)node, &n, sizeof(n))) return -EFAULT;

    return 0;
}

/* getrandom syscall */
int64_t sys_getrandom_impl(uint64_t buf, uint64_t buflen, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (flags & ~3ULL) return -EINVAL; // GRND_NONBLOCK=1, GRND_RANDOM=2
    if (!buf) return -EFAULT;
    if (!buflen) return 0;
    if (buflen > 33554431) return -EINVAL; // max: 32 MiB - 1

    static spinlock_t random_lock;
    static uint64_t   seed;
    uint8_t           output[256];
    uint64_t          done = 0;

    while (done < buflen) {
        size_t count = buflen - done;
        if (count > sizeof(output)) count = sizeof(output);

        spin_lock(&random_lock);
        uint64_t state = seed;
        if (!state) state = (uint64_t)timer_realtime_ns() ^ sched_ticks() ^ 0x9e3779b97f4a7c15ULL;
        for (size_t i = 0; i < count; i++) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            output[i] = (uint8_t)state;
        }

        seed = state;
        spin_unlock(&random_lock);

        if (copy_to_user((void *)(buf + done), output, count)) return done ? (int64_t)done : -EFAULT;
        done += count;
    }
    return (int64_t)done;
}

/* ioprio_get syscall */
int64_t sys_ioprio_get_impl(uint64_t which, uint64_t who, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)who;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (which > 2) return -EINVAL;
    return 4; // IOPRIO_DEFAULT
}
