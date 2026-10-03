/*
 *
 *      syscall_basic.h
 *      Declarations for basic syscall implementations
 *
 *      2026/8/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_BASIC_H_
#define INCLUDE_SYSCALL_BASIC_H_

#include <libs/std/stdint.h>

/* Generic 6-argument syscall implementations shared by the dispatch table. */
int64_t sys_getitimer_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setitimer`. */
int64_t sys_setitimer_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `alarm`. */
int64_t sys_alarm_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `getgroups`. */
int64_t sys_getgroups_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setgroups`. */
int64_t sys_setgroups_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `capget`. */
int64_t sys_capget_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `capset`. */
int64_t sys_capset_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `flock`. */
int64_t sys_flock_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `utime`. */
int64_t sys_utime_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `utimes`. */
int64_t sys_utimes_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `getpriority`. */
int64_t sys_getpriority_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setpriority`. */
int64_t sys_setpriority_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_getparam`. */
int64_t sys_sched_getparam_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_getscheduler`. */
int64_t sys_sched_getscheduler_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_get_priority_max`. */
int64_t sys_sched_get_priority_max_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_get_priority_min`. */
int64_t sys_sched_get_priority_min_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_rr_get_interval`. */
int64_t sys_sched_rr_get_interval_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_setaffinity`. */
int64_t sys_sched_setaffinity_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_getaffinity`. */
int64_t sys_sched_getaffinity_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_setattr`. */
int64_t sys_sched_setattr_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sched_getattr`. */
int64_t sys_sched_getattr_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sethostname`. */
int64_t sys_sethostname_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setdomainname`. */
int64_t sys_setdomainname_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `set_robust_list`. */
int64_t sys_set_robust_list_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `get_robust_list`. */
int64_t sys_get_robust_list_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `fchownat`. */
int64_t sys_fchownat_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `futimesat`. */
int64_t sys_futimesat_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `fchmodat`. */
int64_t sys_fchmodat_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `times`. */
int64_t sys_times_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setuid`. */
int64_t sys_setuid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setgid`. */
int64_t sys_setgid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setreuid`. */
int64_t sys_setreuid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setregid`. */
int64_t sys_setregid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setresuid`. */
int64_t sys_setresuid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setresgid`. */
int64_t sys_setresgid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setfsuid`. */
int64_t sys_setfsuid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setfsgid`. */
int64_t sys_setfsgid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `getresuid`. */
int64_t sys_getresuid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `getresgid`. */
int64_t sys_getresgid_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `umask`. */
int64_t sys_umask_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `chdir`. */
int64_t sys_chdir_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `fchdir`. */
int64_t sys_fchdir_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `truncate`. */
int64_t sys_truncate_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `ftruncate`. */
int64_t sys_ftruncate_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sync`. */
int64_t sys_sync_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `listxattr`. */
int64_t sys_listxattr_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setxattr`. */
int64_t sys_setxattr_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `getxattr`. */
int64_t sys_getxattr_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `removexattr`. */
int64_t sys_removexattr_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `tkill_wrap`. */
int64_t sys_tkill_wrap(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pread64`. */
int64_t sys_pread64_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pwrite64`. */
int64_t sys_pwrite64_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `getcpu`. */
int64_t sys_getcpu_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `getrandom`. */
int64_t sys_getrandom_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `renameat2`. */
int64_t sys_renameat2_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `clock_gettime`. */
int64_t sys_clock_gettime_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `clock_getres`. */
int64_t sys_clock_getres_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `utimensat`. */
int64_t sys_utimensat_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `fallocate`. */
int64_t sys_fallocate_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sync_file_range`. */
int64_t sys_sync_file_range_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `set_tid_address`. */
int64_t sys_set_tid_address_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `mknodat`. */
int64_t sys_mknodat_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `sendfile`. */
int64_t sys_sendfile_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `preadv`. */
int64_t sys_preadv_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pwritev`. */
int64_t sys_pwritev_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `preadv2`. */
int64_t sys_preadv2_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pwritev2`. */
int64_t sys_pwritev2_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pkey_alloc`. */
int64_t sys_pkey_alloc_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pkey_free`. */
int64_t sys_pkey_free_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `io_pgetevents`. */
int64_t sys_io_pgetevents_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pidfd_send_signal`. */
int64_t sys_pidfd_send_signal_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `process_vm_readv`. */
int64_t sys_process_vm_readv_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `process_vm_writev`. */
int64_t sys_process_vm_writev_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `unshare`. */
int64_t sys_unshare_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `setns`. */
int64_t sys_setns_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `splice`. */
int64_t sys_splice_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `tee`. */
int64_t sys_tee_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `vmsplice`. */
int64_t sys_vmsplice_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `ioprio_get`. */
int64_t sys_ioprio_get_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `settimeofday`. */
int64_t sys_settimeofday_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `openat2`. */
int64_t sys_openat2_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* System call handler for `pidfd_getfd`. */
int64_t sys_pidfd_getfd_impl(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

#endif // INCLUDE_SYSCALL_BASIC_H_
