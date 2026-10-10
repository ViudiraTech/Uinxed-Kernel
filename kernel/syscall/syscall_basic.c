/*
 *
 *      syscall_basic.c
 *      Basic syscall implementations - upgrading stubs to real code
 *
 *      2026/8/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/smp.h>
#include <drivers/char/random.h>
#include <fs/core/vfs.h>
#include <ipc/pipe.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/namespace.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <security/capability.h>
#include <syscall/fcntl.h>
#include <syscall/memfd.h>
#include <syscall/syscall.h>

/* utime / utimes / futimesat / utimensat */

#define LINUX_UTIME_NOW  1073741823LL
#define LINUX_UTIME_OMIT 1073741822LL

/* process credentials */
#define CREDENTIAL_ID_UNCHANGED UINT32_MAX

/* getitimer / setitimer / alarm */
typedef struct linux_itimerval {
        int64_t it_interval_sec;
        int64_t it_interval_usec;
        int64_t it_value_sec;
        int64_t it_value_usec;
} linux_itimerval_t;

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

typedef struct linux_utimbuf {
        int64_t actime;
        int64_t modtime;
} linux_utimbuf_t;

/* times */
typedef struct linux_tms {
        int64_t tms_utime;
        int64_t tms_stime;
        int64_t tms_cutime;
        int64_t tms_cstime;
} linux_tms_t;

/*
 * security / vserver / uselib / ustat / sysfs / vhangup / modify_ldt /
 * pivot_root / _sysctl / iopl / ioperm / create_module / get_kernel_syms /
 * query_module / quotactl / nfsservctl / getpmsg / putpmsg / afs_syscall /
 * tuxcall / lookup_dcookie / remap_file_pages / kexec_load /
 * add_key / request_key / keyctl / migrate_pages / move_pages /
 * kexec_file_load / bpf /
 * userfaultfd / io_uring_setup / io_uring_enter / io_uring_register /
 * open_tree / move_mount / fsopen / fsconfig / fsmount / fspick /
 * fanotify_init / fanotify_mark / get_thread_area / set_thread_area /
 * io_setup / io_destroy / io_getevents / io_submit / io_cancel
 *
 * These are either deprecated, highly complex, or require kernel subsystems
 * that don't exist yet.  They remain as sys_unimplemented (return -ENOSYS).
 * See syscall.c for the table entries.
 */

/* openat2 syscall: openat with an extensible how argument */
typedef struct open_how {
        uint64_t flags;
        uint64_t mode;
        uint64_t resolve;
} open_how_t;

/* Convert seconds+microseconds to timer ticks */
static int itimer_time_to_ticks(int64_t sec, int64_t usec, uint64_t *ticks)
{
    if (!ticks || sec < 0 || usec < 0 || usec >= 1000000) return -EINVAL;
    uint64_t sub_ticks = ((uint64_t)usec * CONFIG_TIMER_HZ + 999999ULL) / 1000000ULL;
    if ((uint64_t)sec > (UINT64_MAX - sub_ticks) / CONFIG_TIMER_HZ) return -EINVAL;
    *ticks = ((uint64_t)sec * CONFIG_TIMER_HZ) + sub_ticks;
    return 0;
}

/* Convert ticks back to an itimerval */
static linux_itimerval_t itimer_ticks_to_value(uint64_t remaining, uint64_t interval)
{
    linux_itimerval_t value = {
        .it_interval_sec  = (int64_t)(interval / CONFIG_TIMER_HZ),
        .it_interval_usec = (int64_t)((interval % CONFIG_TIMER_HZ) * 1000000ULL / CONFIG_TIMER_HZ),
        .it_value_sec     = (int64_t)(remaining / CONFIG_TIMER_HZ),
        .it_value_usec    = (int64_t)((remaining % CONFIG_TIMER_HZ) * 1000000ULL / CONFIG_TIMER_HZ),
    };
    return value;
}

/* getitimer syscall: read an interval timer */
int64_t sys_getitimer_impl(uint64_t which, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (which > 2) return -EINVAL;
    if (!curr_value) return -EFAULT;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    uint64_t remaining = 0, interval = 0;
    signal_itimer_get(proc, (unsigned int)which, &remaining, &interval);
    linux_itimerval_t tv = itimer_ticks_to_value(remaining, interval);
    return copy_to_user((void *)curr_value, &tv, sizeof(tv)) ? -EFAULT : 0;
}

/* setitimer syscall: arm or disarm an interval timer */
int64_t sys_setitimer_impl(uint64_t which, uint64_t new_value, uint64_t old_value, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (which > 2) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    /* A NULL new_value disarms the timer. */
    linux_itimerval_t requested = {0};
    if (new_value && copy_from_user(&requested, (const void *)new_value, sizeof(requested))) return -EFAULT;
    uint64_t value_ticks = 0, interval_ticks = 0;
    int      ret = itimer_time_to_ticks(requested.it_value_sec, requested.it_value_usec, &value_ticks);
    if (ret) return ret;
    ret = itimer_time_to_ticks(requested.it_interval_sec, requested.it_interval_usec, &interval_ticks);
    if (ret) return ret;

    uint64_t old_remaining = 0, old_interval = 0;
    signal_itimer_set(proc, (unsigned int)which, value_ticks, interval_ticks, &old_remaining, &old_interval);
    if (old_value) {
        linux_itimerval_t previous = itimer_ticks_to_value(old_remaining, old_interval);
        if (copy_to_user((void *)old_value, &previous, sizeof(previous))) return -EFAULT;
    }
    return 0;
}

/* alarm syscall: schedule a SIGALRM after N seconds */
int64_t sys_alarm_impl(uint64_t seconds, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    uint64_t value         = seconds > UINT64_MAX / CONFIG_TIMER_HZ ? UINT64_MAX : seconds * CONFIG_TIMER_HZ;
    uint64_t old_remaining = 0;
    signal_itimer_set(proc, 0, value, 0, &old_remaining, NULL);
    if (!old_remaining) return 0;
    uint64_t rounded = (old_remaining / CONFIG_TIMER_HZ) + (old_remaining % CONFIG_TIMER_HZ != 0);
    return rounded > UINT32_MAX ? UINT32_MAX : (int64_t)rounded;
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
    uint32_t groups[CONFIG_PROCESS_MAX_GROUPS];
    for (uint16_t i = 0; i < count; i++) groups[i] = user_ns_unmap_id(user_namespace_current(), proc->supplementary_groups[i], true);
    if (count && copy_to_user((void *)list, groups, (size_t)count * sizeof(uint32_t))) return -EFAULT;
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
    if (!user_namespace_current()->setgroups_allowed || !capability_ns(current_task(), user_namespace_current(), CAP_SETGID)) return -EPERM;
    if (size > CONFIG_PROCESS_MAX_GROUPS) return -EINVAL;
    if (size && !list) return -EFAULT;

    uint32_t groups[CONFIG_PROCESS_MAX_GROUPS];
    if (size && copy_from_user(groups, (const void *)list, (size_t)size * sizeof(uint32_t))) return -EFAULT;
    for (uint64_t i = 0; i < size; i++) {
        groups[i] = user_ns_map_id(user_namespace_current(), groups[i], true);
        if (groups[i] == UINT32_MAX) return -EINVAL;
    }
    if (size) memcpy(proc->supplementary_groups, groups, (size_t)size * sizeof(uint32_t));
    proc->supplementary_group_count = (uint16_t)size;
    return 0;
}

/* Decode all Linux capability ABI versions and negotiate an unknown version. */
static int capability_header(uint64_t user, linux_cap_header_t *header, size_t *words)
{
    if (!user || copy_from_user(header, (const void *)user, sizeof(*header))) return -EFAULT;
    if (header->version == 0x19980330)
        *words = 1;
    else if (header->version == 0x20071026 || header->version == 0x20080522)
        *words = 2;
    else {
        header->version = 0x20080522;
        return copy_to_user((void *)user, header, sizeof(*header)) ? -EFAULT : -EINVAL;
    }
    return 0;
}

/* capget exposes both 32-bit words, including capabilities 32..40. */
int64_t sys_capget_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    linux_cap_header_t hdr;
    size_t             words;
    int                result = capability_header(header, &hdr, &words);
    if (result) return result;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    task_t    *target   = current_task();
    process_t *owner    = NULL;
    bool       retained = hdr.pid != 0 && hdr.pid != (int32_t)target->pid;
    if (retained) {
        target = pid_find_task_get((uint64_t)hdr.pid);
        if (!target) return -ESRCH;
        owner = process_find_get((pid_t)target->tgid);
        if (!owner || target->process != owner) {
            if (owner) process_put(owner);
            task_put(target);
            return -ESRCH;
        }
    }
    linux_cap_data_t caps[2] = {{0}, {0}};
    uint64_t         effective, permitted, inheritable;
    capability_get(target, &effective, &permitted, &inheritable);
    for (size_t word = 0; word < words; word++) {
        caps[word].effective   = (uint32_t)(effective >> (word * 32));
        caps[word].permitted   = (uint32_t)(permitted >> (word * 32));
        caps[word].inheritable = (uint32_t)(inheritable >> (word * 32));
    }
    if (retained) {
        process_put(owner);
        task_put(target);
    }
    return data && copy_to_user((void *)data, caps, words * sizeof(caps[0])) ? -EFAULT : 0;
}

/* A service that changed UID may still clear its already-empty capabilities. */
int64_t sys_capset_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    linux_cap_header_t hdr;
    size_t             words;
    int                result = capability_header(header, &hdr, &words);
    if (result) return result;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (hdr.pid && hdr.pid != (int32_t)current_task()->pid) return -EPERM;
    linux_cap_data_t caps[2] = {{0}, {0}};
    if (!data || copy_from_user(caps, (const void *)data, words * sizeof(caps[0]))) return -EFAULT;
    uint64_t effective   = caps[0].effective | ((uint64_t)caps[1].effective << 32);
    uint64_t permitted   = caps[0].permitted | ((uint64_t)caps[1].permitted << 32);
    uint64_t inheritable = caps[0].inheritable | ((uint64_t)caps[1].inheritable << 32);
    return capability_set(current_task(), effective, permitted, inheritable);
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
    uint64_t        mount_id = 0;
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
                if (ret == EOK) node = vfs_open_checked_at(resolved, false, &ret, &mount_id);
            } else {
                file = process_fd_get(proc, dirfd);
                if (!file) return -EBADF;
                node = file->node;
            }
        } else {
            char resolved[CONFIG_VFS_PATH_MAX];
            ret = process_resolve_path_at(proc, dirfd, input, resolved, sizeof(resolved));
            if (ret == EOK) node = vfs_open_checked_at(resolved, (flags & AT_SYMLINK_NOFOLLOW) != 0, &ret, &mount_id);
        }
    }

    if (!node) {
        if (file) process_file_put(file);
        return ret == EOK ? -ENOENT : ret;
    }
    vfs_update(node);
    ret = vfs_set_times_process_at(node, atime, mtime, time_flags, proc, file ? file->mount_id : mount_id);
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
    if ((int64_t)niceval < 0 && !namespace_initial_root(proc)) return -EACCES;
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

/* sched_setaffinity: permission checks match ownership, with a bounded user copy. */
int64_t sys_sched_setaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!mask) return -EFAULT;
    if (!cpusetsize) return -EINVAL;
    cpumask_t affinity = {0};
    size_t    bytes    = cpusetsize < sizeof(affinity) ? (size_t)cpusetsize : sizeof(affinity);
    if (copy_from_user(&affinity, (const void *)mask, bytes)) return -EFAULT;
    task_t *task = pid ? pid_find_task_get(pid) : current_task();
    if (!task) return -ESRCH;
    if (!pid) task_ref(task);
    process_t *caller = process_current();
    int        result = -EPERM;
    if (caller && task->process && (!caller->uid || caller->uid == task->process->uid)) result = sched_setaffinity(task, &affinity);
    task_put(task);
    return result;
}

/* Return only the kernel's rounded CPU-mask length, never cpusetsize bytes. */
int64_t sys_sched_getaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    size_t bytes = (sched_cpu_count() + 63) / 64 * sizeof(uint64_t);
    if (cpusetsize < bytes || cpusetsize % sizeof(uint64_t)) return -EINVAL;
    if (!mask) return -EFAULT;
    task_t *task = pid ? pid_find_task_get(pid) : current_task();
    if (!task) return -ESRCH;
    if (!pid) task_ref(task);
    cpumask_t affinity;
    int       result = sched_getaffinity(task, &affinity);
    task_put(task);
    if (result) return result;
    return copy_to_user((void *)mask, &affinity, bytes) ? -EFAULT : (int64_t)bytes;
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
    if (!proc || !namespace_initial_root(proc)) return -EPERM;
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
    if (!proc || !capability_ns(current_task(), uts_namespace_current()->ns.owner, CAP_SYS_ADMIN)) return -EPERM;

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
    if (!proc || !capability_ns(current_task(), uts_namespace_current()->ns.owner, CAP_SYS_ADMIN)) return -EPERM;

    char buffer[65];
    if (copy_from_user(buffer, (const void *)name, len)) return -EFAULT;
    buffer[len] = '\0';

    uts_namespace_t *uts = uts_namespace_current();
    spin_lock(&uts->ns.lock);
    memcpy(uts->domainname, buffer, len + 1);
    spin_unlock(&uts->ns.lock);
    return 0;
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
    if (task) __atomic_store_n(&task->robust_list, head, __ATOMIC_RELAXED);
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
    if (!head_ptr || !len_ptr) return -EFAULT;
    task_t *target   = current_task();
    bool    retained = pid != 0 && pid != target->pid;
    if (retained) {
        target = pid_find_task_get(pid);
        if (!target) return -ESRCH;
        process_t *owner = process_find_get((pid_t)target->tgid);
        if (!owner || target->process != owner || (owner->uid != proc->uid && !capability_has(current_task(), CAP_SYS_PTRACE))) {
            if (owner) process_put(owner);
            task_put(target);
            return -EPERM;
        }
        process_put(owner);
    }
    uint64_t head = __atomic_load_n(&target->robust_list, __ATOMIC_RELAXED);
    if (head_ptr && copy_to_user((void *)head_ptr, &head, sizeof(head))) {
        if (retained) task_put(target);
        return -EFAULT;
    }
    if (len_ptr) {
        uint64_t sz = 24;
        if (copy_to_user((void *)len_ptr, &sz, sizeof(sz))) {
            if (retained) task_put(target);
            return -EFAULT;
        }
    }
    if (retained) task_put(target);
    return 0;
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
    uint64_t        mount_id = 0;
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
                if (ret == EOK) node = vfs_open_checked_at(resolved, false, &ret, &mount_id);
            } else {
                file = process_fd_get(proc, (int)dirfd);
                if (!file) return -EBADF;
                node = file->node;
            }
        } else {
            char resolved[CONFIG_VFS_PATH_MAX];
            ret = process_resolve_path_at(proc, (int)dirfd, input, resolved, sizeof(resolved));
            if (ret == EOK) node = vfs_open_checked_at(resolved, (flags & AT_SYMLINK_NOFOLLOW) != 0, &ret, &mount_id);
        }
    }

    if (!node) {
        if (file) process_file_put(file);
        return ret == EOK ? -ENOENT : ret;
    }
    vfs_update(node);
    ret = vfs_chown_process_at(node, (uint32_t)owner, (uint32_t)group, proc, file ? file->mount_id : mount_id);
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
    uint64_t mount_id = 0;
    vfs_node_t node = vfs_open_checked_at(resolved, false, &lookup_error, &mount_id);
    if (!node) return lookup_error;
    int result = vfs_chmod_process_at(node, (uint16_t)mode, proc, mount_id);
    vfs_close(node);
    return result;
}

/* times syscall: return process CPU time */
int64_t sys_times_impl(uint64_t tms, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tms) return -EFAULT;
    int64_t     now = (int64_t)timer_ticks_to_user_ticks(sched_ticks());
    linux_tms_t buf = {.tms_utime = now, .tms_stime = 0, .tms_cutime = 0, .tms_cstime = 0};
    if (copy_to_user((void *)tms, &buf, sizeof(buf))) return -EFAULT;
    return now;
}

/* Check whether a UID transition is permitted */
/* Unprivileged callers may only restate one of their three user IDs. */
static bool credential_uid_allowed(const process_t *proc, uint32_t uid)
{
    return uid == CREDENTIAL_ID_UNCHANGED || capability_ns(current_task(), user_namespace_current(), CAP_SETUID) || uid == proc->uid || uid == proc->ruid || uid == proc->suid;
}

/* setfsuid(2) additionally accepts the current filesystem ID. */
static bool credential_fsuid_allowed(const process_t *proc, uint32_t uid)
{
    return credential_uid_allowed(proc, uid) || uid == proc->fsuid;
}

/* Apply the capability transition for a credential change and commit it. */
static void credential_uid_commit(process_t *proc, const uid_set_t *new_ids)
{
    uid_set_t old_ids = {proc->ruid, proc->uid, proc->suid};
    capability_uid_change(current_task(), &old_ids, new_ids);
    proc->ruid  = new_ids->real;
    proc->uid   = new_ids->effective;
    proc->suid  = new_ids->saved;
    proc->fsuid = new_ids->effective;
}

/* Check whether a GID transition is permitted */
static bool credential_gid_allowed(const process_t *proc, uint32_t gid)
{
    return gid == CREDENTIAL_ID_UNCHANGED || capability_ns(current_task(), user_namespace_current(), CAP_SETGID) || gid == proc->gid || gid == proc->rgid || gid == proc->sgid || gid == proc->fsgid;
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
    if ((uint32_t)uid != CREDENTIAL_ID_UNCHANGED) {
        uid = user_ns_map_id(user_namespace_current(), (uint32_t)uid, false);
        if (uid == UINT32_MAX) return -EINVAL;
    }
    uint32_t requested = (uint32_t)uid;
    if (requested == CREDENTIAL_ID_UNCHANGED) return -EINVAL;
    if (!credential_uid_allowed(proc, requested)) return -EPERM;
    /* setuid(2) sets all three IDs for a privileged caller. */
    uid_set_t ids = {proc->ruid, requested, proc->suid};
    if (capability_ns(current_task(), user_namespace_current(), CAP_SETUID)) ids.real = ids.saved = requested;
    credential_uid_commit(proc, &ids);
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
    if ((uint32_t)gid != CREDENTIAL_ID_UNCHANGED) {
        gid = user_ns_map_id(user_namespace_current(), (uint32_t)gid, true);
        if (gid == UINT32_MAX) return -EINVAL;
    }
    uint32_t requested = (uint32_t)gid;
    if (requested == CREDENTIAL_ID_UNCHANGED) return -EINVAL;
    if (!credential_gid_allowed(proc, requested)) return -EPERM;
    if (capability_ns(current_task(), user_namespace_current(), CAP_SETGID)) proc->rgid = proc->sgid = requested;
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
    if ((uint32_t)ruid != CREDENTIAL_ID_UNCHANGED) {
        ruid = user_ns_map_id(user_namespace_current(), (uint32_t)ruid, false);
        if (ruid == UINT32_MAX) return -EINVAL;
    }
    if ((uint32_t)euid != CREDENTIAL_ID_UNCHANGED) {
        euid = user_ns_map_id(user_namespace_current(), (uint32_t)euid, false);
        if (euid == UINT32_MAX) return -EINVAL;
    }
    uint32_t real = (uint32_t)ruid, effective = (uint32_t)euid;
    if (!credential_uid_allowed(proc, real) || !credential_uid_allowed(proc, effective)) return -EPERM;
    uid_set_t new_ids = {proc->ruid, proc->uid, proc->suid};
    if (real != CREDENTIAL_ID_UNCHANGED) new_ids.real = real;
    if (effective != CREDENTIAL_ID_UNCHANGED) {
        new_ids.effective = effective;
        /* Setting the effective ID or a different real ID refreshes the saved ID. */
        if (real != CREDENTIAL_ID_UNCHANGED || effective != proc->ruid) new_ids.saved = effective;
    }
    credential_uid_commit(proc, &new_ids);
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
    if ((uint32_t)rgid != CREDENTIAL_ID_UNCHANGED) {
        rgid = user_ns_map_id(user_namespace_current(), (uint32_t)rgid, true);
        if (rgid == UINT32_MAX) return -EINVAL;
    }
    if ((uint32_t)egid != CREDENTIAL_ID_UNCHANGED) {
        egid = user_ns_map_id(user_namespace_current(), (uint32_t)egid, true);
        if (egid == UINT32_MAX) return -EINVAL;
    }
    uint32_t real = (uint32_t)rgid, effective = (uint32_t)egid;
    if (!credential_gid_allowed(proc, real) || !credential_gid_allowed(proc, effective)) return -EPERM;
    if (real != CREDENTIAL_ID_UNCHANGED) proc->rgid = real;
    if (effective != CREDENTIAL_ID_UNCHANGED) {
        if (real != CREDENTIAL_ID_UNCHANGED || effective != proc->rgid) proc->sgid = effective;
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
    if ((uint32_t)ruid != CREDENTIAL_ID_UNCHANGED) {
        ruid = user_ns_map_id(user_namespace_current(), (uint32_t)ruid, false);
        if (ruid == UINT32_MAX) return -EINVAL;
    }
    if ((uint32_t)euid != CREDENTIAL_ID_UNCHANGED) {
        euid = user_ns_map_id(user_namespace_current(), (uint32_t)euid, false);
        if (euid == UINT32_MAX) return -EINVAL;
    }
    if ((uint32_t)suid != CREDENTIAL_ID_UNCHANGED) {
        suid = user_ns_map_id(user_namespace_current(), (uint32_t)suid, false);
        if (suid == UINT32_MAX) return -EINVAL;
    }
    uint32_t real = (uint32_t)ruid, effective = (uint32_t)euid, saved = (uint32_t)suid;
    if (!credential_uid_allowed(proc, real) || !credential_uid_allowed(proc, effective) || !credential_uid_allowed(proc, saved)) return -EPERM;
    uid_set_t new_ids = {proc->ruid, proc->uid, proc->suid};
    if (real != CREDENTIAL_ID_UNCHANGED) new_ids.real = real;
    if (effective != CREDENTIAL_ID_UNCHANGED) new_ids.effective = effective;
    if (saved != CREDENTIAL_ID_UNCHANGED) new_ids.saved = saved;
    credential_uid_commit(proc, &new_ids);
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
    if ((uint32_t)rgid != CREDENTIAL_ID_UNCHANGED) {
        rgid = user_ns_map_id(user_namespace_current(), (uint32_t)rgid, true);
        if (rgid == UINT32_MAX) return -EINVAL;
    }
    if ((uint32_t)egid != CREDENTIAL_ID_UNCHANGED) {
        egid = user_ns_map_id(user_namespace_current(), (uint32_t)egid, true);
        if (egid == UINT32_MAX) return -EINVAL;
    }
    if ((uint32_t)sgid != CREDENTIAL_ID_UNCHANGED) {
        sgid = user_ns_map_id(user_namespace_current(), (uint32_t)sgid, true);
        if (sgid == UINT32_MAX) return -EINVAL;
    }
    uint32_t real = (uint32_t)rgid, effective = (uint32_t)egid, saved = (uint32_t)sgid;
    if (!credential_gid_allowed(proc, real) || !credential_gid_allowed(proc, effective) || !credential_gid_allowed(proc, saved)) return -EPERM;
    if (real != CREDENTIAL_ID_UNCHANGED) proc->rgid = real;
    if (saved != CREDENTIAL_ID_UNCHANGED) proc->sgid = saved;
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
    uint32_t old = proc->fsuid, requested = user_ns_map_id(user_namespace_current(), (uint32_t)uid, false);
    if (requested != CREDENTIAL_ID_UNCHANGED && credential_fsuid_allowed(proc, requested)) proc->fsuid = requested;
    return user_ns_unmap_id(user_namespace_current(), old, false);
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
    uint32_t old = proc->fsgid, requested = user_ns_map_id(user_namespace_current(), (uint32_t)gid, true);
    if (requested != CREDENTIAL_ID_UNCHANGED && credential_gid_allowed(proc, requested)) proc->fsgid = requested;
    return user_ns_unmap_id(user_namespace_current(), old, true);
}

/* getresuid syscall */
int64_t sys_getresuid_impl(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    uint32_t ruid_value = user_ns_unmap_id(user_namespace_current(), proc->ruid, false);
    if (copy_to_user((void *)ruid, &ruid_value, sizeof(ruid_value))) return -EFAULT;
    uint32_t euid_value = user_ns_unmap_id(user_namespace_current(), proc->uid, false);
    if (copy_to_user((void *)euid, &euid_value, sizeof(euid_value))) return -EFAULT;
    uint32_t suid_value = user_ns_unmap_id(user_namespace_current(), proc->suid, false);
    if (copy_to_user((void *)suid, &suid_value, sizeof(suid_value))) return -EFAULT;
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
    uint32_t rgid_value = user_ns_unmap_id(user_namespace_current(), proc->rgid, true);
    if (copy_to_user((void *)rgid, &rgid_value, sizeof(rgid_value))) return -EFAULT;
    uint32_t egid_value = user_ns_unmap_id(user_namespace_current(), proc->gid, true);
    if (copy_to_user((void *)egid, &egid_value, sizeof(egid_value))) return -EFAULT;
    uint32_t sgid_value = user_ns_unmap_id(user_namespace_current(), proc->sgid, true);
    if (copy_to_user((void *)sgid, &sgid_value, sizeof(sgid_value))) return -EFAULT;
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
    int  ret = vfs_node_path_at(pf->node, pf->mount_id, path, sizeof(path));
    if (ret != EOK) {
        process_file_put(pf);
        return ret;
    }
    strncpy(proc->cwd, path, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    process_file_put(pf);
    return 0;
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
    uint64_t mount_id = 0;
    vfs_node_t node = vfs_open_checked_at(resolved, false, NULL, &mount_id);
    if (!node) return -ENOENT;
    ret = vfs_truncate_at(node, length, mount_id);
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
    ret = vfs_truncate_at(pf->node, length, pf->mount_id);
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

/* tkill syscall: send a signal to a thread */
int64_t sys_tkill_wrap(uint64_t tid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_tkill_impl((int64_t)tid, (int)sig);
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

/* getcpu syscall */
int64_t sys_getcpu_impl(uint64_t cpu, uint64_t node, uint64_t tcache, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tcache;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    task_t  *task = current_task();
    uint32_t c    = task ? task->cpu_id : 0;
    uint32_t n    = numa_cpu_node(c);
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

    uint8_t  output[256];
    uint64_t done = 0;
    while (done < buflen) {
        size_t count = buflen - done;
        if (count > sizeof(output)) count = sizeof(output);

        mem_random_bytes(output, count);

        if (copy_to_user((void *)(buf + done), output, count)) return done ? (int64_t)done : -EFAULT;
        done += count;
    }
    return (int64_t)done;
}

/* renameat2 syscall */
int64_t sys_renameat2_impl(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5)
{
    (void)arg5;
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2
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

/* clock_gettime syscall */
int64_t sys_clock_gettime_impl(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tp) return -EFAULT;
    linux_timespec_t ts;
    int64_t          ns;
    switch (clockid) {
        case 0 :
        case 5 : // CLOCK_REALTIME / CLOCK_REALTIME_COARSE
            ns = timer_realtime_ns();
            break;
        case 1 :
        case 4 :
        case 6 :
        case 7 : // CLOCK_MONOTONIC / _RAW / _COARSE / BOOTTIME
            ns = (int64_t)timer_monotonic_ns();
            break;
        case 2 :
        case 3 : // PROCESS/THREAD_CPUTIME_ID
            ns = 0;
            break;
        default :
            return -EINVAL;
    }
    ts.tv_sec  = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    return copy_to_user((void *)tp, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/* clock_getres syscall */
int64_t sys_clock_getres_impl(uint64_t clockid, uint64_t res, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!res) return 0;
    linux_timespec_t ts;
    switch (clockid) {
        case CLOCK_REALTIME :
        case CLOCK_MONOTONIC :
        case CLOCK_MONOTONIC_RAW :
        case CLOCK_REALTIME_COARSE :
        case CLOCK_MONOTONIC_COARSE :
        case CLOCK_BOOTTIME :
            ts.tv_sec  = 0;
            ts.tv_nsec = (int64_t)timer_monotonic_resolution_ns();
            break;
        case CLOCK_PROCESS_CPUTIME_ID :
        case CLOCK_THREAD_CPUTIME_ID :
            ts.tv_sec  = 0;
            ts.tv_nsec = TIMER_TICK_NS;
            break;
        default :
            return -EINVAL;
    }
    return copy_to_user((void *)res, &ts, sizeof(ts)) ? -EFAULT : 0;
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
    if (offset + len > pf->node->size) ret = vfs_truncate_at(pf->node, offset + len, pf->mount_id);
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

/* pkey_alloc syscall: unsupported */
int64_t sys_pkey_alloc_impl(uint64_t flags, uint64_t access_rights, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;
    (void)access_rights;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS; // no PKU on this x86 config
}

/* pkey_free syscall: unsupported */
int64_t sys_pkey_free_impl(uint64_t pkey, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pkey;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS;
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

/* settimeofday syscall */
int64_t sys_settimeofday_impl(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)tz;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (!tv) return -EFAULT;

    struct {
            int64_t tv_sec;
            int64_t tv_usec;
    } timeval;

    if (copy_from_user(&timeval, (const void *)tv, sizeof(timeval))) return -EFAULT;
    timer_realtime_set_ns((timeval.tv_sec * 1000000000LL) + (timeval.tv_usec * 1000LL));
    return 0;
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
