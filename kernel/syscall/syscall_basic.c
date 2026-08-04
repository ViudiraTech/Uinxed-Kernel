/*
 *
 *      syscall_basic.c
 *      Basic syscall implementations — upgrading stubs to real code
 *
 *      2025 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smp.h>
#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <syscall/syscall.h>
#include <syscall/syscall_table.h>

#define SYSCALL_PATH_MAX VFS_PATH_MAX

/* ======================================================================
 *  Helper: extract basename from a path
 * ====================================================================== */
static const char *path_basename_local(const char *path)
{
    if (!path || !*path) return "";
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') last = p + 1;
    return last;
}

/* ======================================================================
 *  Helper: copy a path string from user space
 * ====================================================================== */
static int copy_path_from_user_fwd(uint64_t upath, char *path, size_t sz)
{
    if (!upath) return -EFAULT;
    int ret = strncpy_from_user(path, (const char *)upath, sz);
    return ret < 0 ? ret : 0;
}

/* ======================================================================
 *  struct timespec64 (used by multiple syscalls)
 * ====================================================================== */
struct linux_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* ======================================================================
 *  getitimer / setitimer / alarm
 * ====================================================================== */

struct linux_itimerval {
    uint64_t it_interval_sec;
    uint64_t it_interval_usec;
    uint64_t it_value_sec;
    uint64_t it_value_usec;
};

int64_t sys_getitimer_impl(uint64_t which, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (which > 2) return -EINVAL;
    if (!curr_value) return -EFAULT;
    struct linux_itimerval tv = {0};
    return copy_to_user((void *)curr_value, &tv, sizeof(tv)) ? -EFAULT : 0;
}

int64_t sys_setitimer_impl(uint64_t which, uint64_t new_value, uint64_t old_value, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    if (which > 2) return -EINVAL;
    (void)new_value;
    if (old_value) {
        struct linux_itimerval tv = {0};
        if (copy_to_user((void *)old_value, &tv, sizeof(tv))) return -EFAULT;
    }
    return 0;
}

int64_t sys_alarm_impl(uint64_t seconds, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    (void)seconds;
    return 0;
}

/* ======================================================================
 *  getgroups / setgroups
 * ====================================================================== */

int64_t sys_getgroups_impl(uint64_t size, uint64_t list, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (size == 0) return 1;
    if (size < 1) return -EINVAL;
    uint32_t gid = proc->gid;
    if (list && copy_to_user((void *)list, &gid, sizeof(gid))) return -EFAULT;
    return 1;
}

int64_t sys_setgroups_impl(uint64_t size, uint64_t list, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;
    if (size > 65536) return -EINVAL;
    (void)list;
    return 0;
}

/* ======================================================================
 *  capget / capset
 * ====================================================================== */

struct linux_cap_header {
    uint32_t version;
    int32_t  pid;
};

struct linux_cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

int64_t sys_capget_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!header) return -EFAULT;
    struct linux_cap_header hdr;
    if (copy_from_user(&hdr, (const void *)header, sizeof(hdr))) return -EFAULT;
    if (hdr.version != 0x20080522) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_t *target = (hdr.pid == 0 || hdr.pid == (int32_t)proc->task->pid) ? proc : process_find_get((pid_t)hdr.pid);
    if (!target) return -ESRCH;
    struct linux_cap_data caps = {0};
    if (target->uid == 0) { caps.effective = caps.permitted = caps.inheritable = 0xFFFFFFFFu; }
    if (target != proc) process_put(target);
    if (data && copy_to_user((void *)data, &caps, sizeof(caps))) return -EFAULT;
    return 0;
}

int64_t sys_capset_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    (void)data;
    if (!header) return -EFAULT;
    struct linux_cap_header hdr;
    if (copy_from_user(&hdr, (const void *)header, sizeof(hdr))) return -EFAULT;
    if (hdr.version != 0x20080522) return -EINVAL;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

/* ======================================================================
 *  flock
 * ====================================================================== */

int64_t sys_flock_impl(uint64_t fd, uint64_t operation, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    (void)operation;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf) return -EBADF;
    process_file_put(pf);
    return 0;
}

/* ======================================================================
 *  utime / utimes
 * ====================================================================== */

int64_t sys_utime_impl(uint64_t filename, uint64_t times, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)filename; (void)times; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0;
}

int64_t sys_utimes_impl(uint64_t filename, uint64_t times, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)filename; (void)times; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0;
}

/* ======================================================================
 *  getpriority / setpriority
 * ====================================================================== */

int64_t sys_getpriority_impl(uint64_t which, uint64_t who, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)who; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (which > 2) return -EINVAL;
    return 20; /* default nice value: 0 → return 20 */
}

int64_t sys_setpriority_impl(uint64_t which, uint64_t who, uint64_t niceval, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)who; (void)arg3; (void)arg4; (void)arg5;
    if (which > 2) return -EINVAL;
    if ((int64_t)niceval < -20 || (int64_t)niceval > 19) return -EACCES;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if ((int64_t)niceval < 0 && proc->uid != 0) return -EACCES;
    return 0;
}

/* ======================================================================
 *  sched_* family
 * ====================================================================== */

int64_t sys_sched_setparam_impl(uint64_t pid, uint64_t param, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)param; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

int64_t sys_sched_getparam_impl(uint64_t pid, uint64_t param, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!param) return -EFAULT;
    struct { int32_t sched_priority; } p = { .sched_priority = 0 };
    return copy_to_user((void *)param, &p, sizeof(p)) ? -EFAULT : 0;
}

int64_t sys_sched_setscheduler_impl(uint64_t pid, uint64_t policy, uint64_t param, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)param; (void)arg3; (void)arg4; (void)arg5;
    if (policy > 2) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (policy != 0 && proc->uid != 0) return -EPERM;
    return 0;
}

int64_t sys_sched_getscheduler_impl(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0; /* SCHED_NORMAL */
}

int64_t sys_sched_get_priority_max_impl(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (policy == 0) return 0;
    if (policy == 1 || policy == 2) return 99;
    return -EINVAL;
}

int64_t sys_sched_get_priority_min_impl(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (policy == 0) return 0;
    if (policy == 1 || policy == 2) return 1;
    return -EINVAL;
}

int64_t sys_sched_rr_get_interval_impl(uint64_t pid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!tp) return -EFAULT;
    struct linux_timespec64 ts = { .tv_sec = 0, .tv_nsec = 100000000 };
    return copy_to_user((void *)tp, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/* ======================================================================
 *  sched_setaffinity / sched_getaffinity
 * ====================================================================== */

int64_t sys_sched_setaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)cpusetsize; (void)mask; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

int64_t sys_sched_getaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)arg3; (void)arg4; (void)arg5;
    if (!mask) return -EFAULT;
    uint32_t ncpus = get_cpu_count();
    uint8_t  buf[128] = {0};
    size_t   bytes = (ncpus + 7) / 8;
    if (bytes > cpusetsize) bytes = cpusetsize;
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    memset(buf, 0xFF, bytes);
    if (ncpus % 8) buf[bytes - 1] &= (uint8_t)((1u << (ncpus % 8)) - 1u);
    return copy_to_user((void *)mask, buf, cpusetsize) ? -EFAULT : (int64_t)bytes;
}

/* ======================================================================
 *  sched_setattr / sched_getattr
 * ====================================================================== */

int64_t sys_sched_setattr_impl(uint64_t pid, uint64_t attr, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)attr; (void)arg3; (void)arg4; (void)arg5;
    if (flags) return -EINVAL;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

int64_t sys_sched_getattr_impl(uint64_t pid, uint64_t attr, uint64_t size, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)pid; (void)attr; (void)size; (void)arg4; (void)arg5;
    if (flags) return -EINVAL;
    return 0;
}

/* ======================================================================
 *  sethostname / setdomainname
 * ====================================================================== */

int64_t sys_sethostname_impl(uint64_t name, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)name; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (len > 64) return -EINVAL;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

int64_t sys_setdomainname_impl(uint64_t name, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)name; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (len > 64) return -EINVAL;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

/* ======================================================================
 *  set_robust_list / get_robust_list
 * ====================================================================== */

int64_t sys_set_robust_list_impl(uint64_t head, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (len != 24) return -EINVAL;  /* sizeof(struct robust_list_head) */
    task_t *task = current_task();
    if (task) task->clear_child_tid = head;
    return 0;
}

int64_t sys_get_robust_list_impl(uint64_t pid, uint64_t head_ptr, uint64_t len_ptr, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_t *target = proc;
    if (pid != 0 && pid != (uint64_t)proc->task->pid) {
        target = process_find_get((pid_t)pid);
        if (!target) return -ESRCH;
    }
    uint64_t head = target->task->clear_child_tid;
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

/* ======================================================================
 *  fchownat / futimesat / fchmodat
 * ====================================================================== */

int64_t sys_fchownat_impl(uint64_t dirfd, uint64_t path, uint64_t owner, uint64_t group, uint64_t flags, uint64_t arg5)
{
    (void)dirfd; (void)path; (void)owner; (void)group; (void)flags; (void)arg5;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

int64_t sys_futimesat_impl(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd; (void)path; (void)times; (void)arg3; (void)arg4; (void)arg5;
    return 0;
}

/*
 * fchmodat(int dirfd, const char *pathname, mode_t mode, int flags)
 *
 * Linux: flags must be 0; AT_SYMLINK_NOFOLLOW is not supported by the
 * classic call (use fchmodat2), and anything else is -EINVAL.
 */
int64_t sys_fchmodat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;

    if (flags == 0x100) return -EOPNOTSUPP; /* AT_SYMLINK_NOFOLLOW: use fchmodat2 */
    if (flags != 0) return -EINVAL;

    char input[SYSCALL_PATH_MAX];
    int  ret = copy_path_from_user_fwd(path, input, sizeof(input));
    if (ret != 0) return ret;
    if (!input[0]) return -ENOENT;

    char resolved[SYSCALL_PATH_MAX];
    ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
    if (ret != 0) return ret;

    vfs_node_t node = vfs_open(resolved);
    if (!node) return -ENOENT;
    node->mode = (uint16_t)(mode & 07777);
    inotify_notify(node, IN_ATTRIB);
    vfs_close(node);
    return EOK;
}

/* ======================================================================
 *  times
 * ====================================================================== */

struct linux_tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

int64_t sys_times_impl(uint64_t tms, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!tms) return -EFAULT;
    int64_t now = timer_realtime_ns() / 10000000;  /* ns → 100Hz ticks */
    struct linux_tms buf = { .tms_utime = now, .tms_stime = 0, .tms_cutime = 0, .tms_cstime = 0 };
    if (copy_to_user((void *)tms, &buf, sizeof(buf))) return -EFAULT;
    return (int64_t)now;
}

/* ======================================================================
 *  setuid / setgid / getresuid / getresgid
 * ====================================================================== */

int64_t sys_setuid_impl(uint64_t uid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0 && (uint32_t)uid != proc->uid) return -EPERM;
    proc->uid = (uint32_t)uid;
    return 0;
}

int64_t sys_setgid_impl(uint64_t gid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0 && (uint32_t)gid != proc->gid) return -EPERM;
    proc->gid = (uint32_t)gid;
    return 0;
}

int64_t sys_getresuid_impl(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    uint32_t uid = proc->uid;
    if (ruid && copy_to_user((void *)ruid, &uid, sizeof(uid))) return -EFAULT;
    if (euid && copy_to_user((void *)euid, &uid, sizeof(uid))) return -EFAULT;
    if (suid && copy_to_user((void *)suid, &uid, sizeof(uid))) return -EFAULT;
    return 0;
}

int64_t sys_getresgid_impl(uint64_t rgid, uint64_t egid, uint64_t sgid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    uint32_t gid = proc->gid;
    if (rgid && copy_to_user((void *)rgid, &gid, sizeof(gid))) return -EFAULT;
    if (egid && copy_to_user((void *)egid, &gid, sizeof(gid))) return -EFAULT;
    if (sgid && copy_to_user((void *)sgid, &gid, sizeof(gid))) return -EFAULT;
    return 0;
}

/* ======================================================================
 *  umask
 * ====================================================================== */

int64_t sys_umask_impl(uint64_t mask, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    uint16_t old = proc->umask;
    proc->umask = (uint16_t)(mask & 0777);
    return old;
}

/* ======================================================================
 *  chdir / fchdir
 * ====================================================================== */

int64_t sys_chdir_impl(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!path) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int ret = copy_path_from_user_fwd(path, name, sizeof(name));
    if (ret) return ret;
    char resolved[SYSCALL_PATH_MAX];
    ret = process_resolve_path_at(proc, PROCESS_AT_FDCWD, name, resolved, sizeof(resolved));
    if (ret) return ret;
    vfs_node_t node = vfs_open(resolved);
    if (!node) return -ENOENT;
    if (node->type != file_dir) { vfs_close(node); return -ENOTDIR; }
    vfs_close(node);
    strncpy(proc->cwd, resolved, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    return 0;
}

int64_t sys_fchdir_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf || !pf->node) {
        if (pf) process_file_put(pf);
        return -EBADF;
    }
    if (pf->node->type != file_dir) { process_file_put(pf); return -ENOTDIR; }
    /* Best-effort: mark cwd */
    strncpy(proc->cwd, "[fd]", sizeof(proc->cwd));
    process_file_put(pf);
    return 0;
}

/* ======================================================================
 *  truncate / ftruncate
 * ====================================================================== */

int64_t sys_truncate_impl(uint64_t path, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!path) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    char name[SYSCALL_PATH_MAX];
    int ret = copy_path_from_user_fwd(path, name, sizeof(name));
    if (ret) return ret;
    char resolved[SYSCALL_PATH_MAX];
    ret = process_resolve_path_at(proc, PROCESS_AT_FDCWD, name, resolved, sizeof(resolved));
    if (ret) return ret;
    vfs_node_t node = vfs_open(resolved);
    if (!node) return -ENOENT;
    ret = vfs_truncate(node, length);
    vfs_close(node);
    return ret;
}

int64_t sys_ftruncate_impl(uint64_t fd, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf || !pf->node) {
        if (pf) process_file_put(pf);
        return -EBADF;
    }
    int ret = vfs_truncate(pf->node, length);
    process_file_put(pf);
    return ret;
}

/* ======================================================================
 *  sync
 * ====================================================================== */

int64_t sys_sync_impl(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    vfs_sync_all();
    return 0;
}

/* ======================================================================
 *  listxattr / llistxattr / flistxattr
 * ====================================================================== */

int64_t sys_listxattr_impl(uint64_t path, uint64_t list, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)path; (void)list; (void)size; (void)arg3; (void)arg4; (void)arg5;
    return 0;   /* no extended attributes */
}

/* ======================================================================
 *  setxattr / getxattr / removexattr (and l-variants and f-variants)
 * ====================================================================== */

int64_t sys_setxattr_impl(uint64_t path, uint64_t name, uint64_t value, uint64_t size, uint64_t flags, uint64_t arg5)
{
    (void)path; (void)name; (void)value; (void)size; (void)flags; (void)arg5;
    return -EOPNOTSUPP;
}

int64_t sys_getxattr_impl(uint64_t path, uint64_t name, uint64_t value, uint64_t size, uint64_t arg4, uint64_t arg5)
{
    (void)path; (void)name; (void)value; (void)size; (void)arg4; (void)arg5;
    return -ENODATA;   /* no xattrs → name not found */
}

int64_t sys_removexattr_impl(uint64_t path, uint64_t name, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)path; (void)name; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return -ENODATA;
}

/* ======================================================================
 *  tkill
 * ====================================================================== */

int64_t sys_tkill_real(uint64_t tid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    /* Send signal directly to the thread */
    process_t *proc = process_find_get((pid_t)tid);
    if (!proc) return -ESRCH;
    int ret = signal_send(proc, (int)sig, NULL);
    process_put(proc);
    return ret;
}

/* ======================================================================
 *  pread64 / pwrite64
 * ====================================================================== */

int64_t sys_pread64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (!buf && count) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int64_t old = process_fd_seek(proc, (int)fd, 0, SEEK_CUR);
    if (old < 0) return old;
    if (process_fd_seek(proc, (int)fd, (int64_t)offset, SEEK_SET) < 0) return -EINVAL;
    uint8_t tmp[4096];
    size_t  done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        int64_t n = process_fd_read(proc, (int)fd, tmp, chunk);
        if (n < 0) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return done ? (int64_t)done : n; }
        if (!n) break;
        if (copy_to_user((void *)(buf + done), tmp, (size_t)n)) {
            process_fd_seek(proc, (int)fd, old, SEEK_SET);
            return done ? (int64_t)done : -EFAULT;
        }
        done += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    process_fd_seek(proc, (int)fd, old, SEEK_SET);
    return (int64_t)done;
}

int64_t sys_pwrite64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (!buf && count) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int64_t old = process_fd_seek(proc, (int)fd, 0, SEEK_CUR);
    if (old < 0) return old;
    if (process_fd_seek(proc, (int)fd, (int64_t)offset, SEEK_SET) < 0) return -EINVAL;
    uint8_t tmp[4096];
    size_t  done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        if (copy_from_user(tmp, (const void *)(buf + done), chunk)) {
            process_fd_seek(proc, (int)fd, old, SEEK_SET);
            return done ? (int64_t)done : -EFAULT;
        }
        int64_t n = process_fd_write(proc, (int)fd, tmp, chunk);
        if (n < 0) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return done ? (int64_t)done : n; }
        if (!n) break;
        done += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    process_fd_seek(proc, (int)fd, old, SEEK_SET);
    return (int64_t)done;
}

/* ======================================================================
 *  getcpu
 * ====================================================================== */

int64_t sys_getcpu_impl(uint64_t cpu, uint64_t node, uint64_t tcache, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tcache; (void)arg3; (void)arg4; (void)arg5;
    task_t  *task = current_task();
    uint32_t c    = task ? task->cpu_id : 0;
    uint32_t n    = 0;
    if (cpu  && copy_to_user((void *)cpu,  &c, sizeof(c))) return -EFAULT;
    if (node && copy_to_user((void *)node, &n, sizeof(n))) return -EFAULT;
    return 0;
}

/* ======================================================================
 *  getrandom
 * ====================================================================== */

int64_t sys_getrandom_impl(uint64_t buf, uint64_t buflen, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    if (flags & ~3ULL) return -EINVAL;          /* GRND_NONBLOCK=1, GRND_RANDOM=2 */
    if (!buf)     return -EFAULT;
    if (!buflen)  return 0;
    if (buflen > 33554431) return -EINVAL;      /* max: 32 MiB - 1 */
    /* Simple xor-shift PRNG seeded with TSC */
    static uint64_t seed;
    if (!seed) seed = timer_realtime_ns();
    for (uint64_t i = 0; i < buflen; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        uint8_t byte = (uint8_t)(seed & 0xFF);
        if (copy_to_user((void *)(buf + i), &byte, 1))
            return (int64_t)i ? (int64_t)i : -EFAULT;
    }
    return (int64_t)buflen;
}

/* ======================================================================
 *  renameat2
 * ====================================================================== */

int64_t sys_renameat2_impl(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2
    if (flags & ~3ULL) return -EINVAL;
    if (!oldpath || !newpath) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char old_name[SYSCALL_PATH_MAX], new_name[SYSCALL_PATH_MAX];
    int ret = copy_path_from_user_fwd(oldpath, old_name, sizeof(old_name));
    if (ret) return ret;
    ret = copy_path_from_user_fwd(newpath, new_name, sizeof(new_name));
    if (ret) return ret;

    char old_resolved[SYSCALL_PATH_MAX], new_resolved[SYSCALL_PATH_MAX];
    ret = process_resolve_path_at(proc, (int)olddirfd, old_name, old_resolved, sizeof(old_resolved));
    if (ret) return ret;
    ret = process_resolve_path_at(proc, (int)newdirfd, new_name, new_resolved, sizeof(new_resolved));
    if (ret) return ret;

    if (flags & RENAME_NOREPLACE) {
        vfs_node_t exist = vfs_open(new_resolved);
        if (exist) { vfs_close(exist); return -EEXIST; }
    }
    if (flags & RENAME_EXCHANGE) return -ENOSYS; /* not supported */

    vfs_node_t node = vfs_open(old_resolved);
    if (!node) return -ENOENT;
    ret = vfs_rename(node, path_basename_local(new_resolved));
    vfs_close(node);
    return ret;
}

/* ======================================================================
 *  clock_gettime / clock_getres
 * ====================================================================== */

int64_t sys_clock_gettime_impl(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!tp) return -EFAULT;
    struct linux_timespec64 ts;
    int64_t ns;
    switch (clockid) {
    case 0: case 5:  /* CLOCK_REALTIME / CLOCK_REALTIME_COARSE */
        ns = timer_realtime_ns(); break;
    case 1: case 4: case 6:  /* CLOCK_MONOTONIC / _RAW / _COARSE */
        ns = timer_realtime_ns(); break;  /* monotonic ≈ realtime in hobby kernel */
    case 2: case 3:  /* PROCESS/THREAD_CPUTIME_ID */
        ns = 0; break;
    default: return -EINVAL;
    }
    ts.tv_sec  = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    return copy_to_user((void *)tp, &ts, sizeof(ts)) ? -EFAULT : 0;
}

int64_t sys_clock_getres_impl(uint64_t clockid, uint64_t res, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!res) return 0;
    struct linux_timespec64 ts;
    switch (clockid) {
    case 0: case 1: case 4: case 5: case 6:
        ts.tv_sec = 0; ts.tv_nsec = 1000000; break;  /* 1 ms resolution */
    case 2: case 3:
        ts.tv_sec = 0; ts.tv_nsec = 1000000; break;
    default: return -EINVAL;
    }
    return copy_to_user((void *)res, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/* ======================================================================
 *  utimensat
 * ====================================================================== */

int64_t sys_utimensat_impl(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd; (void)path; (void)times; (void)flags; (void)arg4; (void)arg5;
    return 0;   /* accepted but timestamps not stored in most FS backends */
}

/* ======================================================================
 *  fallocate
 * ====================================================================== */

int64_t sys_fallocate_impl(uint64_t fd, uint64_t mode, uint64_t offset, uint64_t len, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (mode & ~3ULL) return -EOPNOTSUPP;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf || !pf->node) { if (pf) process_file_put(pf); return -EBADF; }
    if (mode & 2) { process_file_put(pf); return -EOPNOTSUPP; }  /* FALLOC_FL_PUNCH_HOLE */
    if (offset + len > pf->node->size) vfs_truncate(pf->node, offset + len);
    process_file_put(pf);
    return 0;
}

/* ======================================================================
 *  sync_file_range
 * ====================================================================== */

int64_t sys_sync_file_range_impl(uint64_t fd, uint64_t offset, uint64_t nbytes, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)offset; (void)nbytes; (void)arg4; (void)arg5;
    if (flags & ~15ULL) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, (int)fd);
    if (!pf) return -EBADF;
    process_file_put(pf);
    return 0;
}

/* ======================================================================
 *  set_tid_address
 * ====================================================================== */

int64_t sys_set_tid_address_impl(uint64_t tidptr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    task_t *task = current_task();
    if (task) task->clear_child_tid = tidptr;
    return (int64_t)(task ? task->pid : 0);
}

/* ======================================================================
 *  mknodat
 * ====================================================================== */

int64_t sys_mknodat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t dev, uint64_t arg4, uint64_t arg5)
{
    (void)dirfd; (void)path; (void)mode; (void)dev; (void)arg4; (void)arg5;
    process_t *proc = process_current();
    if (!proc || proc->uid != 0) return -EPERM;
    return 0;
}

/* ======================================================================
 *  sendfile
 * ====================================================================== */

int64_t sys_sendfile_impl(uint64_t out_fd, uint64_t in_fd, uint64_t offset, uint64_t count, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (!count) return 0;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    process_file_t *pf_in  = process_fd_get(proc, (int)in_fd);
    process_file_t *pf_out = process_fd_get(proc, (int)out_fd);
    if (!pf_in || !pf_out) {
        if (pf_in)  process_file_put(pf_in);
        if (pf_out) process_file_put(pf_out);
        return -EBADF;
    }

    int64_t old_off   = 0;
    bool    have_off  = (offset != 0);
    int64_t user_off  = 0;

    if (have_off) {
        if (copy_from_user(&user_off, (const void *)offset, sizeof(user_off))) {
            process_file_put(pf_in); process_file_put(pf_out); return -EFAULT;
        }
        old_off = process_fd_seek(proc, (int)in_fd, 0, SEEK_CUR);
        process_fd_seek(proc, (int)in_fd, user_off, SEEK_SET);
    }

    uint8_t buf[4096];
    size_t  total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        int64_t n = process_fd_read(proc, (int)in_fd, buf, chunk);
        if (n < 0) { if (have_off) process_fd_seek(proc, (int)in_fd, old_off, SEEK_SET); process_file_put(pf_in); process_file_put(pf_out); return total ? (int64_t)total : n; }
        if (!n) break;
        int64_t w = process_fd_write(proc, (int)out_fd, buf, (size_t)n);
        if (w < 0) { if (have_off) process_fd_seek(proc, (int)in_fd, old_off, SEEK_SET); process_file_put(pf_in); process_file_put(pf_out); return total ? (int64_t)total : w; }
        total += (size_t)w;
        if ((size_t)n < chunk) break;
    }

    if (have_off) {
        int64_t new_off = process_fd_seek(proc, (int)in_fd, 0, SEEK_CUR);
        copy_to_user((void *)offset, &new_off, sizeof(int64_t));
        process_fd_seek(proc, (int)in_fd, old_off, SEEK_SET);
    }
    process_file_put(pf_in);
    process_file_put(pf_out);
    return (int64_t)total;
}

/* ======================================================================
 *  preadv / pwritev
 * ====================================================================== */

struct sys_iovec { void *iov_base; size_t iov_len; };

int64_t sys_preadv_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (iovcnt > 1024) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int64_t old = process_fd_seek(proc, (int)fd, 0, SEEK_CUR);
    process_fd_seek(proc, (int)fd, (int64_t)offset, SEEK_SET);
    size_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        struct sys_iovec v;
        if (copy_from_user(&v, (const void *)(iov + i * sizeof(v)), sizeof(v))) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return total ? (int64_t)total : -EFAULT; }
        uint8_t tmp[4096];
        size_t  done = 0;
        while (done < v.iov_len) {
            size_t chunk = v.iov_len - done;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            int64_t n = process_fd_read(proc, (int)fd, tmp, chunk);
            if (n < 0) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return total ? (int64_t)total : n; }
            if (!n)    { process_fd_seek(proc, (int)fd, old, SEEK_SET); return (int64_t)total; }
            if (copy_to_user((void *)((uintptr_t)v.iov_base + done), tmp, (size_t)n)) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return (int64_t)total; }
            done += (size_t)n; total += (size_t)n;
            if ((size_t)n < chunk) break;
        }
    }
    process_fd_seek(proc, (int)fd, old, SEEK_SET);
    return (int64_t)total;
}

int64_t sys_pwritev_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (iovcnt > 1024) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    int64_t old = process_fd_seek(proc, (int)fd, 0, SEEK_CUR);
    process_fd_seek(proc, (int)fd, (int64_t)offset, SEEK_SET);
    size_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        struct sys_iovec v;
        if (copy_from_user(&v, (const void *)(iov + i * sizeof(v)), sizeof(v))) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return total ? (int64_t)total : -EFAULT; }
        uint8_t tmp[4096];
        size_t  done = 0;
        while (done < v.iov_len) {
            size_t chunk = v.iov_len - done;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            if (copy_from_user(tmp, (const void *)((uintptr_t)v.iov_base + done), chunk)) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return (int64_t)total; }
            int64_t n = process_fd_write(proc, (int)fd, tmp, chunk);
            if (n < 0) { process_fd_seek(proc, (int)fd, old, SEEK_SET); return total ? (int64_t)total : n; }
            if (!n)    { process_fd_seek(proc, (int)fd, old, SEEK_SET); return (int64_t)total; }
            done += (size_t)n; total += (size_t)n;
            if ((size_t)n < chunk) break;
        }
    }
    process_fd_seek(proc, (int)fd, old, SEEK_SET);
    return (int64_t)total;
}

/* ======================================================================
 *  preadv2 / pwritev2
 * ====================================================================== */

int64_t sys_preadv2_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    if (flags & ~1ULL) return -EINVAL;  /* RWF_HIPRI=1 */
    return sys_preadv_impl(fd, iov, iovcnt, offset, 0, 0);
}

int64_t sys_pwritev2_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
    if (flags & ~1ULL) return -EINVAL;
    return sys_pwritev_impl(fd, iov, iovcnt, offset, 0, 0);
}

/* ======================================================================
 *  pkey_alloc / pkey_free
 * ====================================================================== */

int64_t sys_pkey_alloc_impl(uint64_t flags, uint64_t access_rights, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags; (void)access_rights; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return -ENOSYS;  /* no PKU on this x86 config */
}

int64_t sys_pkey_free_impl(uint64_t pkey, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pkey; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return -ENOSYS;
}

/* ======================================================================
 *  io_pgetevents
 * ====================================================================== */

int64_t sys_io_pgetevents_impl(uint64_t ctx_id, uint64_t min_nr, uint64_t nr, uint64_t events, uint64_t timeout, uint64_t sigmask)
{
    (void)ctx_id; (void)min_nr; (void)nr; (void)events; (void)timeout; (void)sigmask;
    return -ENOSYS;  /* AIO not supported */
}

/* ======================================================================
 *  pidfd_send_signal
 * ====================================================================== */

int64_t sys_pidfd_send_signal_impl(uint64_t pidfd, uint64_t sig, uint64_t info, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)info; (void)arg4; (void)arg5;
    if (flags) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* pidfd is a file descriptor pointing to a process */
    process_file_t *pf = process_fd_get(proc, (int)pidfd);
    process_t *target   = NULL;

    if (pf && pf->node && pf->node->handle) {
        target = (process_t *)pf->node->handle;
    }
    if (!target) {
        /* Fallback: treat pidfd as a raw PID */
        if (pf) process_file_put(pf);
        target = process_find_get((pid_t)pidfd);
        if (!target) return -ESRCH;
        int ret = signal_send(target, (int)sig, NULL);
        process_put(target);
        return ret;
    }

    int ret = signal_send(target, (int)sig, NULL);
    process_file_put(pf);
    return ret;
}

/* ======================================================================
 *  process_vm_readv / process_vm_writev
 * ====================================================================== */

int64_t sys_process_vm_readv_impl(uint64_t pid, uint64_t local_iov, uint64_t local_iovcnt,
                                   uint64_t remote_iov, uint64_t remote_iovcnt, uint64_t flags)
{
    (void)pid; (void)local_iov; (void)local_iovcnt; (void)remote_iov; (void)remote_iovcnt;
    if (flags) return -EINVAL;
    return -ENOSYS;  /* cross-process VM operations not yet implemented */
}

int64_t sys_process_vm_writev_impl(uint64_t pid, uint64_t local_iov, uint64_t local_iovcnt,
                                    uint64_t remote_iov, uint64_t remote_iovcnt, uint64_t flags)
{
    (void)pid; (void)local_iov; (void)local_iovcnt; (void)remote_iov; (void)remote_iovcnt;
    if (flags) return -EINVAL;
    return -ENOSYS;
}

/* ======================================================================
 *  unshare
 * ====================================================================== */

int64_t sys_unshare_impl(uint64_t unshare_flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (unshare_flags) return -EINVAL;   /* no namespace support */
    return 0;
}

/* ======================================================================
 *  splice / tee / vmsplice (we already have these stubs, but add real ones)
 * ====================================================================== */

int64_t sys_splice_impl(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out,
                         uint64_t len, uint64_t flags)
{
    if (flags & ~6ULL) return -EINVAL;
    /* Simple fallback: read from fd_in, write to fd_out */
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!len) return 0;

    uint8_t buf[4096];
    size_t  total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        int64_t n = process_fd_read(proc, (int)fd_in, buf, chunk);
        if (n < 0) return total ? (int64_t)total : n;
        if (!n) break;
        int64_t w = process_fd_write(proc, (int)fd_out, buf, (size_t)n);
        if (w < 0) return total ? (int64_t)total : w;
        total += (size_t)w;
        if ((size_t)n < chunk) break;
    }
    (void)off_in; (void)off_out;
    return (int64_t)total;
}

int64_t sys_tee_impl(uint64_t fd_in, uint64_t fd_out, uint64_t len, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (flags) return -EINVAL;
    return sys_splice_impl(fd_in, 0, fd_out, 0, len, 0);
}

int64_t sys_vmsplice_impl(uint64_t fd, uint64_t iov, uint64_t nr_segs, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)fd; (void)iov; (void)nr_segs; (void)arg4; (void)arg5;
    if (flags & ~3ULL) return -EINVAL;
    return -ENOSYS;
}

/* ======================================================================
 *  ioprio_set / ioprio_get
 * ====================================================================== */

int64_t sys_ioprio_set_impl(uint64_t which, uint64_t who, uint64_t ioprio, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)who; (void)ioprio; (void)arg3; (void)arg4; (void)arg5;
    if (which > 2) return -EINVAL;
    return 0;
}

int64_t sys_ioprio_get_impl(uint64_t which, uint64_t who, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)who; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (which > 2) return -EINVAL;
    return 4;  /* IOPRIO_DEFAULT */
}

/* ======================================================================
 *  timer_create / settime / gettime / getoverrun / delete
 * ====================================================================== */

int64_t sys_timer_create_impl(uint64_t clockid, uint64_t evp, uint64_t timerid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)clockid; (void)evp; (void)arg3; (void)arg4; (void)arg5;
    if (!timerid) return -EFAULT;
    /* POSIX timers: return dummy timer id = 1 */
    int32_t tid = 1;
    if (copy_to_user((void *)timerid, &tid, sizeof(tid))) return -EFAULT;
    return 0;
}

int64_t sys_timer_settime_impl(uint64_t timerid, uint64_t flags, uint64_t new_value, uint64_t old_value, uint64_t arg4, uint64_t arg5)
{
    (void)timerid; (void)flags; (void)new_value; (void)arg4; (void)arg5;
    if (old_value) {
        struct linux_itimerval tv = {0};
        if (copy_to_user((void *)old_value, &tv, sizeof(tv))) return -EFAULT;
    }
    return 0;
}

int64_t sys_timer_gettime_impl(uint64_t timerid, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)timerid; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!curr_value) return -EFAULT;
    struct linux_itimerval tv = {0};
    return copy_to_user((void *)curr_value, &tv, sizeof(tv)) ? -EFAULT : 0;
}

int64_t sys_timer_getoverrun_impl(uint64_t timerid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)timerid; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0;
}

int64_t sys_timer_delete_impl(uint64_t timerid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)timerid; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0;
}

/* ======================================================================
 *  syslog
 * ====================================================================== */

int64_t sys_syslog_impl(uint64_t type, uint64_t buf, uint64_t len, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)buf; (void)len; (void)arg3; (void)arg4; (void)arg5;
    /* 0=close, 1=open, 2=read, 3=read_all, 4=read_clear, 5=clear,
     * 6=disable, 7=enable, 8=set_level, 9=unread, 10=size */
    if (type == 10) return 0;  /* kernel log buffer size: 0 */
    if (type <= 9) return 0;   /* all operations accepted */
    return -EINVAL;
}

/* ======================================================================
 *  adjtimex
 * ====================================================================== */

int64_t sys_adjtimex_impl(uint64_t txc, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)txc; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0;  /* clock synchronized, no adjustment needed */
}

/* ======================================================================
 *  settimeofday
 * ====================================================================== */

int64_t sys_settimeofday_impl(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tz; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    if (!tv) return -EFAULT;
    struct { int64_t tv_sec; int64_t tv_usec; } timeval;
    if (copy_from_user(&timeval, (const void *)tv, sizeof(timeval))) return -EFAULT;
    timer_realtime_set_ns(timeval.tv_sec * 1000000000LL + timeval.tv_usec * 1000LL);
    return 0;
}

/* ======================================================================
 *  settimeofday + clock_adjtime
 * ====================================================================== */

int64_t sys_clock_adjtime_impl(uint64_t clockid, uint64_t txc, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)clockid; (void)txc; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0;
}

/* ======================================================================
 *  acct
 * ====================================================================== */

int64_t sys_acct_impl(uint64_t filename, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)filename; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    return 0;  /* process accounting: accept but do nothing */
}

/* ======================================================================
 *  security / vserver / uselib / ustat / sysfs / vhangup / modify_ldt /
 *  pivot_root / _sysctl / iopl / ioperm / create_module / get_kernel_syms /
 *  query_module / quotactl / nfsservctl / getpmsg / putpmsg / afs_syscall /
 *  tuxcall / lookup_dcookie / remap_file_pages / kexec_load /
 *  add_key / request_key / keyctl / migrate_pages / move_pages /
 *  mbind / set_mempolicy / get_mempolicy / name_to_handle_at /
 *  open_by_handle_at / setns / kexec_file_load / seccomp / bpf /
 *  userfaultfd / io_uring_setup / io_uring_enter / io_uring_register /
 *  open_tree / move_mount / fsopen / fsconfig / fsmount / fspick /
 *  fanotify_init / fanotify_mark / get_thread_area / set_thread_area /
 *  io_setup / io_destroy / io_getevents / io_submit / io_cancel
 * ======================================================================
 *
 * These are either deprecated, highly complex, or require kernel subsystems
 * that don't exist yet.  They remain as sys_stub (return -ENOSYS).
 * See syscall.c for the table entries.
 */

/* ======================================================================
 *  openat2 (437)
 *  Modern openat with extensible how argument.
 *  For now, delegate to openat.
 * ====================================================================== */

struct open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

int64_t sys_openat2_impl(uint64_t dirfd, uint64_t path, uint64_t how, uint64_t usize, uint64_t arg4, uint64_t arg5)
{
    (void)arg4; (void)arg5;
    if (!path || !how) return -EFAULT;
    if (usize < sizeof(struct open_how)) return -EINVAL;
    if (usize > sizeof(struct open_how)) return -E2BIG;

    struct open_how oh;
    if (copy_from_user(&oh, (const void *)how, sizeof(oh))) return -EFAULT;

    if (oh.resolve & ~31ULL) return -EINVAL;

    /* Delegate to the same logic as openat: open a file by dirfd+path */
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    char input[VFS_PATH_MAX];
    int ret = copy_path_from_user_fwd(path, input, sizeof(input));
    if (ret) return ret;

    char resolved[VFS_PATH_MAX];
    ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
    if (ret) return ret;

    vfs_node_t node = vfs_open(resolved);
    if (!node) {
        return -ENOENT;
    }

    (void)oh.resolve; /* resolve flags not enforced yet */
    return process_fd_install(proc, node, oh.flags);
}

/* ======================================================================
 *  pidfd_getfd (438)
 *  Get a duplicate of another process's file descriptor via pidfd.
 * ====================================================================== */

int64_t sys_pidfd_getfd_impl(uint64_t pidfd, uint64_t targetfd, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    if (flags) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* pidfd is a file descriptor pointing to a process */
    process_file_t *pf = process_fd_get(proc, (int)pidfd);
    process_t *target = NULL;

    if (pf && pf->node && pf->node->handle) {
        target = (process_t *)pf->node->handle;
    }
    if (!target) {
        if (pf) process_file_put(pf);
        target = process_find_get((pid_t)pidfd);
        if (!target) return -ESRCH;
    }

    /* Get the target's fd */
    process_file_t *tf = process_fd_get(target, (int)targetfd);
    if (!tf) {
        if (pf) process_file_put(pf);
        else process_put(target);
        return -EBADF;
    }

    /* Install in current process */
    int newfd = process_fd_install(proc, tf->node, tf->flags);
    process_file_put(tf);
    if (pf) process_file_put(pf);
    else process_put(target);

    return newfd;
}
