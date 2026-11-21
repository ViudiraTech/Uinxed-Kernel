/*
 *
 *      syscall.c
 *      System call dispatch
 *
 *      2026/7/20 By Rainy101112 & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/eflags.h>
#include <arch/cpu/smp.h>
#include <arch/misc/common.h>
#include <ipc/futex.h>
#include <ipc/poll.h>
#include <kernel/debug/debug.h>
#include <kernel/debug/printk.h>
#include <kernel/interrupt/interrupt.h>
#include <libs/std/errno.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/memfd.h>
#include <process/namespace.h>
#include <process/process.h>
#include <process/ptrace.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <security/seccomp.h>
#include <sync/signal.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>
#include <syscall/syscall_table.h>

/* Two-step stringify so an offset macro (e.g. SYSCALL_CPU_USER_RSP_OFFSET) expands before #value. */
#define SYSCALL_STRINGIFY_INNER(value) #value
#define SYSCALL_STRINGIFY(value)       SYSCALL_STRINGIFY_INNER(value)

/* SYSRET #GPs when RIP >= 2^47 (upper half, vsyscall page); AC must be clear, so SMAP needs IRET. */
#define X86_64_CANONICAL_BOUNDARY 0x0000800000000000ULL

_Static_assert(offsetof(syscall_frame_t, r15) == 0, "syscall frame first register");
_Static_assert(offsetof(syscall_frame_t, rip) == 15 * sizeof(uint64_t), "syscall frame RIP offset");
_Static_assert(offsetof(syscall_frame_t, rsp) == 18 * sizeof(uint64_t), "syscall frame RSP offset");
_Static_assert(sizeof(syscall_frame_t) == 20 * sizeof(uint64_t), "syscall frame size");

/* Probes retry unavailable syscalls; serial/fb logging is far slower than -ENOSYS, so log each number once. */
static uint64_t syscall_missing_logged[(SYS_MAX + 63U) / 64U];
static uint8_t  syscall_unknown_logged;

/* Generic unimplemented syscall: dispatch logs it once, then returns -ENOSYS. */
static int64_t sys_unimplemented(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS;
}

/* Syscall dispatch table: syscall number -> handler. */
static const syscall_fn_t syscall_table[SYS_MAX] = {
    [SYS_READ]                    = sys_read,
    [SYS_WRITE]                   = sys_write,
    [SYS_OPEN]                    = sys_open,
    [SYS_CLOSE]                   = sys_close,
    [SYS_STAT]                    = sys_stat,
    [SYS_FSTAT]                   = sys_fstat,
    [SYS_LSTAT]                   = sys_stat,
    [SYS_POLL]                    = sys_poll,
    [SYS_LSEEK]                   = sys_lseek,
    [SYS_MMAP]                    = sys_mmap,
    [SYS_MPROTECT]                = sys_mprotect_wrap,
    [SYS_MUNMAP]                  = sys_munmap,
    [SYS_BRK]                     = sys_brk,
    [SYS_RT_SIGACTION]            = sys_rt_sigaction_wrap,
    [SYS_RT_SIGPROCMASK]          = sys_rt_sigprocmask_wrap,
    [SYS_RT_SIGRETURN]            = sys_rt_sigreturn_wrap,
    [SYS_IOCTL]                   = sys_ioctl,
    [SYS_PREAD64]                 = sys_pread64_impl,
    [SYS_PWRITE64]                = sys_pwrite64_impl,
    [SYS_READV]                   = sys_readv_wrap,
    [SYS_WRITEV]                  = sys_writev_wrap,
    [SYS_ACCESS]                  = sys_access_impl,
    [SYS_PIPE]                    = sys_pipe_wrap,
    [SYS_SELECT]                  = sys_select,
    [SYS_SCHED_YIELD]             = sys_sched_yield,
    [SYS_MREMAP]                  = sys_mremap_wrap,
    [SYS_MSYNC]                   = sys_msync_wrap,
    [SYS_MINCORE]                 = sys_mincore_wrap,
    [SYS_MADVISE]                 = sys_madvise_wrap,
    [SYS_SHMGET]                  = sys_shmget_wrap,
    [SYS_SHMAT]                   = sys_shmat_wrap,
    [SYS_SHMCTL]                  = sys_shmctl_wrap,
    [SYS_DUP]                     = sys_dup,
    [SYS_DUP2]                    = sys_dup2,
    [SYS_PAUSE]                   = sys_pause_wrap,
    [SYS_NANOSLEEP]               = sys_nanosleep,
    [SYS_GETITIMER]               = sys_getitimer_impl,
    [SYS_ALARM]                   = sys_alarm_impl,
    [SYS_SETITIMER]               = sys_setitimer_impl,
    [SYS_GETPID]                  = sys_getpid,
    [SYS_SENDFILE]                = sys_sendfile_impl,
    [SYS_SOCKET]                  = sys_socket_wrap,
    [SYS_CONNECT]                 = sys_connect_wrap,
    [SYS_ACCEPT]                  = sys_accept_wrap,
    [SYS_SENDTO]                  = sys_sendto_wrap,
    [SYS_RECVFROM]                = sys_recvfrom_wrap,
    [SYS_SENDMSG]                 = sys_sendmsg_wrap,
    [SYS_RECVMSG]                 = sys_recvmsg_wrap,
    [SYS_SHUTDOWN]                = sys_shutdown_wrap,
    [SYS_BIND]                    = sys_bind_wrap,
    [SYS_LISTEN]                  = sys_listen_wrap,
    [SYS_GETSOCKNAME]             = sys_getsockname_wrap,
    [SYS_GETPEERNAME]             = sys_getpeername_wrap,
    [SYS_SOCKETPAIR]              = sys_socketpair_wrap,
    [SYS_SETSOCKOPT]              = sys_setsockopt_wrap,
    [SYS_GETSOCKOPT]              = sys_getsockopt_wrap,
    [SYS_CLONE]                   = NULL,
    [SYS_FORK]                    = NULL,
    [SYS_VFORK]                   = NULL,
    [SYS_EXECVE]                  = sys_execve_wrap,
    [SYS_EXIT]                    = sys_exit,
    [SYS_WAIT4]                   = sys_wait4,
    [SYS_KILL]                    = sys_kill,
    [SYS_UNAME]                   = sys_uname,
    [SYS_SEMGET]                  = sys_semget_wrap,
    [SYS_SEMOP]                   = sys_semop_wrap,
    [SYS_SEMCTL]                  = sys_semctl_wrap,
    [SYS_SHMDT]                   = sys_shmdt_wrap,
    [SYS_MSGGET]                  = sys_msgget_wrap,
    [SYS_MSGSND]                  = sys_msgsnd_wrap,
    [SYS_MSGRCV]                  = sys_msgrcv_wrap,
    [SYS_MSGCTL]                  = sys_msgctl_wrap,
    [SYS_FCNTL]                   = sys_fcntl_wrap,
    [SYS_FLOCK]                   = sys_flock_impl,
    [SYS_FSYNC]                   = sys_fsync_impl,
    [SYS_FDATASYNC]               = sys_fdatasync_impl,
    [SYS_TRUNCATE]                = sys_truncate_impl,
    [SYS_FTRUNCATE]               = sys_ftruncate_impl,
    [SYS_GETDENTS]                = sys_getdents64_wrap,
    [SYS_GETCWD]                  = sys_getcwd,
    [SYS_CHDIR]                   = sys_chdir_impl,
    [SYS_FCHDIR]                  = sys_fchdir_impl,
    [SYS_RENAME]                  = sys_rename,
    [SYS_MKDIR]                   = sys_mkdir,
    [SYS_RMDIR]                   = sys_rmdir,
    [SYS_CREAT]                   = sys_creat,
    [SYS_LINK]                    = sys_link,
    [SYS_UNLINK]                  = sys_unlink,
    [SYS_SYMLINK]                 = sys_symlink,
    [SYS_READLINK]                = sys_readlink,
    [SYS_CHMOD]                   = sys_chmod_impl,
    [SYS_FCHMOD]                  = sys_fchmod_impl,
    [SYS_CHOWN]                   = sys_chown_impl,
    [SYS_FCHOWN]                  = sys_fchown_impl,
    [SYS_LCHOWN]                  = sys_lchown_impl,
    [SYS_UMASK]                   = sys_umask_impl,
    [SYS_GETTIMEOFDAY]            = sys_gettimeofday,
    [SYS_GETRLIMIT]               = sys_getrlimit_impl,
    [SYS_GETRUSAGE]               = sys_getrusage_impl,
    [SYS_SYSINFO]                 = sys_sysinfo_impl,
    [SYS_TIMES]                   = sys_times_impl,
    [SYS_PTRACE]                  = sys_ptrace_wrap,
    [SYS_GETUID]                  = sys_getuid,
    [SYS_SYSLOG]                  = sys_unimplemented,
    [SYS_GETGID]                  = sys_getgid,
    [SYS_SETUID]                  = sys_setuid_impl,
    [SYS_SETGID]                  = sys_setgid_impl,
    [SYS_GETEUID]                 = sys_getuid,
    [SYS_GETEGID]                 = sys_getgid,
    [SYS_SETPGID]                 = sys_setpgid_wrap,
    [SYS_GETPPID]                 = sys_getppid,
    [SYS_GETPGRP]                 = sys_getpgrp_wrap,
    [SYS_SETSID]                  = sys_setsid_wrap,
    [SYS_SETREUID]                = sys_setreuid_impl,
    [SYS_SETREGID]                = sys_setregid_impl,
    [SYS_GETGROUPS]               = sys_getgroups_impl,
    [SYS_SETGROUPS]               = sys_setgroups_impl,
    [SYS_SETRESUID]               = sys_setresuid_impl,
    [SYS_GETRESUID]               = sys_getresuid_impl,
    [SYS_SETRESGID]               = sys_setresgid_impl,
    [SYS_GETRESGID]               = sys_getresgid_impl,
    [SYS_GETPGID]                 = sys_getpgid_wrap,
    [SYS_SETFSUID]                = sys_setfsuid_impl,
    [SYS_SETFSGID]                = sys_setfsgid_impl,
    [SYS_GETSID]                  = sys_getsid_wrap,
    [SYS_CAPGET]                  = sys_capget_impl,
    [SYS_CAPSET]                  = sys_capset_impl,
    [SYS_RT_SIGPENDING]           = sys_rt_sigpending_wrap,
    [SYS_RT_SIGTIMEDWAIT]         = sys_rt_sigtimedwait_wrap,
    [SYS_RT_SIGQUEUEINFO]         = sys_rt_sigqueueinfo_wrap,
    [SYS_RT_SIGSUSPEND]           = sys_rt_sigsuspend_wrap,
    [SYS_SIGALTSTACK]             = sys_sigaltstack_wrap,
    [SYS_UTIME]                   = sys_utime_impl,
    [SYS_MKNOD]                   = sys_mknod_impl,
    [SYS_USELIB]                  = sys_unimplemented,
    [SYS_PERSONALITY]             = sys_personality_impl,
    [SYS_USTAT]                   = sys_unimplemented,
    [SYS_STATFS]                  = sys_statfs_impl,
    [SYS_FSTATFS]                 = sys_fstatfs_impl,
    [SYS_SYSFS]                   = sys_unimplemented,
    [SYS_GETPRIORITY]             = sys_getpriority_impl,
    [SYS_SETPRIORITY]             = sys_setpriority_impl,
    [SYS_SCHED_SETPARAM]          = sys_unimplemented,
    [SYS_SCHED_GETPARAM]          = sys_sched_getparam_impl,
    [SYS_SCHED_SETSCHEDULER]      = sys_unimplemented,
    [SYS_SCHED_GETSCHEDULER]      = sys_sched_getscheduler_impl,
    [SYS_SCHED_GET_PRIORITY_MAX]  = sys_sched_get_priority_max_impl,
    [SYS_SCHED_GET_PRIORITY_MIN]  = sys_sched_get_priority_min_impl,
    [SYS_SCHED_RR_GET_INTERVAL]   = sys_sched_rr_get_interval_impl,
    [SYS_MLOCK]                   = sys_mlock_wrap,
    [SYS_MUNLOCK]                 = sys_munlock_wrap,
    [SYS_MLOCKALL]                = sys_mlockall_wrap,
    [SYS_MUNLOCKALL]              = sys_munlockall_wrap,
    [SYS_VHANGUP]                 = sys_unimplemented,
    [SYS_MODIFY_LDT]              = sys_unimplemented,
    [SYS_PIVOT_ROOT]              = sys_unimplemented,
    [SYS__SYSCTL]                 = sys_unimplemented,
    [SYS_PRCTL]                   = sys_prctl_impl,
    [SYS_ARCH_PRCTL]              = sys_arch_prctl,
    [SYS_ADJTIMEX]                = sys_unimplemented,
    [SYS_SETRLIMIT]               = sys_setrlimit_impl,
    [SYS_CHROOT]                  = sys_chroot_wrap,
    [SYS_SYNC]                    = sys_sync_impl,
    [SYS_ACCT]                    = sys_unimplemented,
    [SYS_SETTIMEOFDAY]            = sys_settimeofday_impl,
    [SYS_MOUNT]                   = sys_mount,
    [SYS_UMOUNT2]                 = sys_umount2,
    [SYS_SWAPON]                  = sys_swapon,
    [SYS_SWAPOFF]                 = sys_swapoff,
    [SYS_REBOOT]                  = sys_reboot_impl,
    [SYS_SETHOSTNAME]             = sys_sethostname_impl,
    [SYS_SETDOMAINNAME]           = sys_setdomainname_impl,
    [SYS_IOPL]                    = sys_unimplemented,
    [SYS_IOPERM]                  = sys_unimplemented,
    [SYS_CREATE_MODULE]           = sys_unimplemented,
    [SYS_INIT_MODULE]             = sys_init_module_impl,
    [SYS_DELETE_MODULE]           = sys_delete_module_impl,
    [SYS_GET_KERNEL_SYMS]         = sys_unimplemented,
    [SYS_QUERY_MODULE]            = sys_unimplemented,
    [SYS_QUOTACTL]                = sys_unimplemented,
    [SYS_NFSSERVCTL]              = sys_unimplemented,
    [SYS_GETPMSG]                 = sys_unimplemented,
    [SYS_PUTPMSG]                 = sys_unimplemented,
    [SYS_AFS_SYSCALL]             = sys_unimplemented,
    [SYS_TUXCALL]                 = sys_unimplemented,
    [SYS_SECURITY]                = sys_unimplemented,
    [SYS_GETTID]                  = sys_gettid,
    [SYS_READAHEAD]               = sys_readahead,
    [SYS_SETXATTR]                = sys_setxattr_impl,
    [SYS_LSETXATTR]               = sys_setxattr_impl,
    [SYS_FSETXATTR]               = sys_setxattr_impl,
    [SYS_GETXATTR]                = sys_getxattr_impl,
    [SYS_LGETXATTR]               = sys_getxattr_impl,
    [SYS_FGETXATTR]               = sys_getxattr_impl,
    [SYS_LISTXATTR]               = sys_listxattr_impl,
    [SYS_LLISTXATTR]              = sys_listxattr_impl,
    [SYS_FLISTXATTR]              = sys_listxattr_impl,
    [SYS_REMOVEXATTR]             = sys_removexattr_impl,
    [SYS_LREMOVEXATTR]            = sys_removexattr_impl,
    [SYS_FREMOVEXATTR]            = sys_removexattr_impl,
    [SYS_TKILL]                   = sys_tkill_wrap,
    [SYS_TIME]                    = sys_time,
    [SYS_FUTEX]                   = sys_futex_wrap,
    [SYS_SCHED_SETAFFINITY]       = sys_sched_setaffinity_impl,
    [SYS_SCHED_GETAFFINITY]       = sys_sched_getaffinity_impl,
    [SYS_SET_THREAD_AREA]         = sys_unimplemented,
    [SYS_IO_SETUP]                = sys_unimplemented,
    [SYS_IO_DESTROY]              = sys_unimplemented,
    [SYS_IO_GETEVENTS]            = sys_unimplemented,
    [SYS_IO_SUBMIT]               = sys_unimplemented,
    [SYS_IO_CANCEL]               = sys_unimplemented,
    [SYS_GET_THREAD_AREA]         = sys_unimplemented,
    [SYS_LOOKUP_DCOOKIE]          = sys_unimplemented,
    [SYS_EPOLL_CREATE]            = sys_epoll_create_wrap,
    [SYS_EPOLL_CTL_OLD]           = sys_epoll_ctl_wrap,
    [SYS_EPOLL_WAIT_OLD]          = sys_epoll_wait_wrap,
    [SYS_REMAP_FILE_PAGES]        = sys_unimplemented,
    [SYS_GETDENTS64]              = sys_getdents64_wrap,
    [SYS_SET_TID_ADDRESS]         = sys_set_tid_address_impl,
    [SYS_RESTART_SYSCALL]         = sys_restart_syscall,
    [SYS_SEMTIMEDOP]              = sys_semtimedop_wrap,
    [SYS_FADVISE64]               = sys_fadvise64,
    [SYS_TIMER_CREATE]            = sys_unimplemented,
    [SYS_TIMER_SETTIME]           = sys_unimplemented,
    [SYS_TIMER_GETTIME]           = sys_unimplemented,
    [SYS_TIMER_GETOVERRUN]        = sys_unimplemented,
    [SYS_TIMER_DELETE]            = sys_unimplemented,
    [SYS_CLOCK_SETTIME]           = sys_clock_settime_impl,
    [SYS_CLOCK_GETTIME]           = sys_clock_gettime_impl,
    [SYS_CLOCK_GETRES]            = sys_clock_getres_impl,
    [SYS_CLOCK_NANOSLEEP]         = sys_clock_nanosleep_impl,
    [SYS_EXIT_GROUP]              = sys_exit_group,
    [SYS_EPOLL_WAIT]              = sys_epoll_wait_wrap,
    [SYS_EPOLL_CTL]               = sys_epoll_ctl_wrap,
    [SYS_TGKILL]                  = sys_tgkill_wrap,
    [SYS_UTIMES]                  = sys_utimes_impl,
    [SYS_VSERVER]                 = sys_unimplemented,
    [SYS_MBIND]                   = sys_unimplemented,
    [SYS_SET_MEMPOLICY]           = sys_unimplemented,
    [SYS_GET_MEMPOLICY]           = sys_unimplemented,
    [SYS_MQ_OPEN]                 = sys_mq_open_wrap,
    [SYS_MQ_UNLINK]               = sys_mq_unlink_wrap,
    [SYS_MQ_TIMEDSEND]            = sys_mq_timedsend_wrap,
    [SYS_MQ_TIMEDRECEIVE]         = sys_mq_timedreceive_wrap,
    [SYS_MQ_NOTIFY]               = sys_mq_notify_wrap,
    [SYS_MQ_GETSETATTR]           = sys_mq_getsetattr_wrap,
    [SYS_KEXEC_LOAD]              = sys_unimplemented,
    [SYS_WAITID]                  = sys_waitid_impl,
    [SYS_ADD_KEY]                 = sys_unimplemented,
    [SYS_REQUEST_KEY]             = sys_unimplemented,
    [SYS_KEYCTL]                  = sys_unimplemented,
    [SYS_IOPRIO_SET]              = sys_unimplemented,
    [SYS_IOPRIO_GET]              = sys_ioprio_get_impl,
    [SYS_INOTIFY_INIT]            = sys_inotify_init_wrap,
    [SYS_INOTIFY_ADD_WATCH]       = sys_inotify_add_watch_wrap,
    [SYS_INOTIFY_RM_WATCH]        = sys_inotify_rm_watch_wrap,
    [SYS_MIGRATE_PAGES]           = sys_unimplemented,
    [SYS_OPENAT]                  = sys_openat,
    [SYS_MKDIRAT]                 = sys_mkdirat,
    [SYS_MKNODAT]                 = sys_mknodat_impl,
    [SYS_FCHOWNAT]                = sys_fchownat_impl,
    [SYS_FUTIMESAT]               = sys_futimesat_impl,
    [SYS_NEWFSTATAT]              = sys_newfstatat,
    [SYS_UNLINKAT]                = sys_unlinkat,
    [SYS_RENAMEAT]                = sys_renameat,
    [SYS_LINKAT]                  = sys_linkat,
    [SYS_SYMLINKAT]               = sys_symlinkat,
    [SYS_READLINKAT]              = sys_readlinkat,
    [SYS_FCHMODAT]                = sys_fchmodat_impl,
    [SYS_FACCESSAT]               = sys_faccessat_impl,
    [SYS_PSELECT6]                = sys_pselect6,
    [SYS_PPOLL]                   = sys_ppoll,
    [SYS_UNSHARE]                 = sys_unshare_impl,
    [SYS_SET_ROBUST_LIST]         = sys_set_robust_list_impl,
    [SYS_GET_ROBUST_LIST]         = sys_get_robust_list_impl,
    [SYS_SPLICE]                  = sys_splice_impl,
    [SYS_TEE]                     = sys_tee_impl,
    [SYS_SYNC_FILE_RANGE]         = sys_sync_file_range_impl,
    [SYS_VMSPLICE]                = sys_vmsplice_impl,
    [SYS_MOVE_PAGES]              = sys_unimplemented,
    [SYS_UTIMENSAT]               = sys_utimensat_impl,
    [SYS_EPOLL_PWAIT]             = sys_epoll_pwait_wrap,
    [SYS_SIGNALFD]                = sys_signalfd_wrap,
    [SYS_TIMERFD_CREATE]          = sys_timerfd_create_wrap,
    [SYS_EVENTFD]                 = sys_eventfd_wrap,
    [SYS_FALLOCATE]               = sys_fallocate_impl,
    [SYS_TIMERFD_SETTIME]         = sys_timerfd_settime_wrap,
    [SYS_TIMERFD_GETTIME]         = sys_timerfd_gettime_wrap,
    [SYS_ACCEPT4]                 = sys_accept4_wrap,
    [SYS_SIGNALFD4]               = sys_signalfd4_wrap,
    [SYS_EVENTFD2]                = sys_eventfd2_wrap,
    [SYS_EPOLL_CREATE1]           = sys_epoll_create1_wrap,
    [SYS_DUP3]                    = sys_dup3,
    [SYS_PIPE2]                   = sys_pipe2_wrap,
    [SYS_INOTIFY_INIT1]           = sys_inotify_init1_wrap,
    [SYS_PREADV]                  = sys_preadv_impl,
    [SYS_PWRITEV]                 = sys_pwritev_impl,
    [SYS_RT_TGSIGQUEUEINFO]       = sys_rt_tgsigqueueinfo_wrap,
    [SYS_PERF_EVENT_OPEN]         = sys_unimplemented,
    [SYS_RECVMMSG]                = sys_recvmmsg_wrap,
    [SYS_FANOTIFY_INIT]           = sys_unimplemented,
    [SYS_FANOTIFY_MARK]           = sys_unimplemented,
    [SYS_PRLIMIT64]               = sys_prlimit64_impl,
    [SYS_NAME_TO_HANDLE_AT]       = sys_name_to_handle_at_impl,
    [SYS_OPEN_BY_HANDLE_AT]       = sys_open_by_handle_at_impl,
    [SYS_CLOCK_ADJTIME]           = sys_unimplemented,
    [SYS_SYNCFS]                  = sys_syncfs_impl,
    [SYS_SENDMMSG]                = sys_sendmmsg_wrap,
    [SYS_SETNS]                   = sys_setns_impl,
    [SYS_GETCPU]                  = sys_getcpu_impl,
    [SYS_PROCESS_VM_READV]        = sys_process_vm_readv_impl,
    [SYS_PROCESS_VM_WRITEV]       = sys_process_vm_writev_impl,
    [SYS_KCMP]                    = sys_unimplemented,
    [SYS_FINIT_MODULE]            = sys_finit_module_impl,
    [SYS_SCHED_SETATTR]           = sys_sched_setattr_impl,
    [SYS_SCHED_GETATTR]           = sys_sched_getattr_impl,
    [SYS_RENAMEAT2]               = sys_renameat2_impl,
    [SYS_SECCOMP]                 = sys_seccomp,
    [SYS_GETRANDOM]               = sys_getrandom_impl,
    [SYS_MEMFD_CREATE]            = sys_memfd_create,
    [SYS_KEXEC_FILE_LOAD]         = sys_unimplemented,
    [SYS_BPF]                     = sys_unimplemented,
    [SYS_EXECVEAT]                = sys_execveat_wrap,
    [SYS_USERFAULTFD]             = sys_unimplemented,
    [SYS_MEMBARRIER]              = sys_membarrier_wrap,
    [SYS_MLOCK2]                  = sys_mlock2_wrap,
    [SYS_COPY_FILE_RANGE]         = sys_copy_file_range_wrap,
    [SYS_PREADV2]                 = sys_preadv2_impl,
    [SYS_PWRITEV2]                = sys_pwritev2_impl,
    [SYS_PKEY_MPROTECT]           = sys_pkey_mprotect_wrap,
    [SYS_PKEY_ALLOC]              = sys_pkey_alloc_impl,
    [SYS_PKEY_FREE]               = sys_pkey_free_impl,
    [SYS_STATX]                   = sys_statx,
    [SYS_IO_PGETEVENTS]           = sys_io_pgetevents_impl,
    [SYS_RSEQ]                    = sys_rseq_impl,
    [SYS_PIDFD_SEND_SIGNAL]       = sys_pidfd_send_signal_impl,
    [SYS_IO_URING_SETUP]          = sys_unimplemented,
    [SYS_IO_URING_ENTER]          = sys_unimplemented,
    [SYS_IO_URING_REGISTER]       = sys_unimplemented,
    [SYS_OPEN_TREE]               = sys_unimplemented,
    [SYS_MOVE_MOUNT]              = sys_unimplemented,
    [SYS_FSOPEN]                  = sys_unimplemented,
    [SYS_FSCONFIG]                = sys_unimplemented,
    [SYS_FSMOUNT]                 = sys_unimplemented,
    [SYS_FSPICK]                  = sys_unimplemented,
    [SYS_PIDFD_OPEN]              = sys_pidfd_open_impl,
    [SYS_CLONE3]                  = NULL, // frame-aware: dispatched in syscall_dispatch
    [SYS_CLOSE_RANGE]             = sys_close_range,
    [SYS_OPENAT2]                 = sys_openat2_impl,
    [SYS_PIDFD_GETFD]             = sys_pidfd_getfd_impl,
    [SYS_FACCESSAT2]              = sys_faccessat2_impl,
    [SYS_PROCESS_MADVISE]         = sys_process_madvise_impl,
    [SYS_EPOLL_PWAIT2]            = sys_epoll_pwait2_impl,
    [SYS_MOUNT_SETATTR]           = sys_unimplemented,
    [SYS_QUOTACTL_FD]             = sys_unimplemented,
    [SYS_LANDLOCK_CREATE_RULESET] = sys_unimplemented,
    [SYS_LANDLOCK_ADD_RULE]       = sys_unimplemented,
    [SYS_LANDLOCK_RESTRICT_SELF]  = sys_unimplemented,
    [SYS_MEMFD_SECRET]            = sys_unimplemented,
    [SYS_PROCESS_MRELEASE]        = sys_unimplemented,
    [SYS_FUTEX_WAITV]             = sys_futex_waitv,
    [SYS_FUTEX_WAKE]              = sys_futex_wake,
    [SYS_FUTEX_WAIT]              = sys_futex_wait,
    [SYS_FUTEX_REQUEUE]           = sys_futex_requeue,
    [SYS_CACHESTAT]               = sys_unimplemented,
    [SYS_FCHMODAT2]               = sys_fchmodat2_impl,
};

/* Syscall log missing once. */
static bool syscall_log_missing_once(uint64_t num)
{
    if (num >= SYS_MAX) return !__atomic_exchange_n(&syscall_unknown_logged, 1, __ATOMIC_RELAXED);
    uint64_t mask = 1ULL << (num & 63U);
    return !(__atomic_fetch_or(&syscall_missing_logged[num >> 6], mask, __ATOMIC_RELAXED) & mask);
}

/* Helper: check if a return value is a kernel-internal restart code. */
static int is_restart_code(int64_t ret)
{
    return ret == -ERESTARTSYS || ret == -ERESTARTNOINTR || ret == -ERESTARTNOHAND || ret == -ERESTART_RESTARTBLOCK || ret == -ERESTART;
}

/* Cheap return-to-user test which avoids the signal slow path on most calls. */
static bool syscall_signal_work_pending(task_t *task)
{
    if (!task || !task->process) return false;
    signal_state_t *state = &task->process->signal;
    if (__atomic_load_n(&state->group_exit, __ATOMIC_ACQUIRE)) return true;

    sigset_t pending = __atomic_load_n(&state->pending, __ATOMIC_ACQUIRE) | __atomic_load_n(&task->signal_pending, __ATOMIC_ACQUIRE);
    sigset_t blocked = __atomic_load_n(&task->signal_blocked, __ATOMIC_RELAXED);
    return (pending & ~blocked) || __atomic_load_n(&task->signal_restore_mask, __ATOMIC_ACQUIRE);
}

/* Dispatch a syscall from a saved register frame (used: called from inline asm) */
__attribute__((used)) int syscall_dispatch(syscall_frame_t *frame)
{
    uint64_t num    = frame->rax;
    int64_t  retval = 0;

    /* syscall_entry has already swapped to the kernel per-CPU GS window. */
    task_t *dispatch_task = percpu_gs_current();
    bool    traced        = dispatch_task && __atomic_load_n(&dispatch_task->ptrace.tracer_pid, __ATOMIC_ACQUIRE);
    bool    force_iret    = traced;

    if (traced) ptrace_syscall_enter(frame, num);
    num = frame->rax;

    if (dispatch_task && __atomic_load_n(&dispatch_task->seccomp_mode, __ATOMIC_ACQUIRE) != SECCOMP_MODE_DISABLED && !seccomp_enforce(frame, &num, &retval)) {
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }
    frame->rax = num;

    if (num == SYS_CLONE && (frame->rdi & CLONE_THREAD)) {
        uint64_t flags = frame->rdi;
        if ((flags & CLONE_PTHREAD_REQUIRED) != CLONE_PTHREAD_REQUIRED || (flags & ~CLONE_PTHREAD_ALLOWED)) {
            frame->rax = (uint64_t)-EINVAL;
            goto check_signals;
        }
        if (((flags & CLONE_PARENT_SETTID) && !frame->rdx) || ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !frame->r10)) {
            frame->rax = (uint64_t)-EFAULT;
            goto check_signals;
        }

        int     error = EOK;
        task_t *child = process_clone_thread(frame, frame->rsi, (flags & CLONE_PARENT_SETTID) ? frame->rdx : 0, (flags & CLONE_CHILD_SETTID) ? frame->r10 : 0,
                                             (flags & CLONE_CHILD_CLEARTID) ? frame->r10 : 0, (flags & CLONE_SETTLS) ? frame->r8 : current_task()->thread.fs_base, &error);
        retval        = child ? (int64_t)child->pid : error;
        frame->rax    = (uint64_t)retval;

        if (child) ptrace_fork_event(frame, PTRACE_EVENT_CLONE, child->pid);
        goto check_signals;
    }
    if (num == SYS_FORK || num == SYS_VFORK || num == SYS_CLONE) {
        uint64_t clone_flags = num == SYS_CLONE ? frame->rdi : SIGCHLD;
        bool     vfork       = num == SYS_VFORK || (clone_flags & CLONE_VFORK);
        uint64_t tid_flags   = CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;

        /*
         * Be permissive: systemd's safe_fork_full() passes namespace/sandbox flags
         * (sd-mkdcreds uses CLONE_NEWNS|CLONE_NEWUSER), clear_child_tid, set_tls and
         * stack; all become a normal fork, since mount/user namespaces are not isolated yet.
         */
        uint64_t supported = SIGCHLD | tid_flags | CLONE_DETACHED | CLONE_VM | CLONE_VFORK | CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET
                             | CLONE_PARENT | CLONE_UNTRACED | CLONE_SYSVSEM | CLONE_FILES | CLONE_FS | CLONE_SIGHAND | CLONE_SETTLS | CLONE_CHILD_CLEARTID;

        if (num == SYS_CLONE && (clone_flags & ~supported)) {
            frame->rax = (uint64_t)-EINVAL;
            goto check_signals;
        }
        if ((num == SYS_CLONE && vfork && !(clone_flags & CLONE_VM)) || ((clone_flags & 0xff) != SIGCHLD) || (vfork && num == SYS_CLONE && !frame->rsi)
            || ((clone_flags & CLONE_PARENT_SETTID) && !frame->rdx) || ((clone_flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !frame->r10)) {
            frame->rax = (uint64_t)-EINVAL;
            goto check_signals;
        }
        if (((clone_flags & CLONE_PARENT_SETTID) && !user_access_ok((void *)frame->rdx, sizeof(uint32_t), 1))
            || ((clone_flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !user_access_ok((void *)frame->r10, sizeof(uint32_t), 1))) {
            frame->rax = (uint64_t)-EFAULT;
            goto check_signals;
        }

        int        error = EOK;
        uint32_t   event = vfork ? PTRACE_EVENT_VFORK : PTRACE_EVENT_FORK;
        process_t *child = process_fork_status_event_mode(&error, event, vfork);

        if (child) {
            uint64_t        kstack_top  = (uint64_t)(child->kernel_stack + CONFIG_PROCESS_KERNEL_STACK);
            uint64_t       *kstack      = (uint64_t *)ALIGN_DOWN(kstack_top, 16ULL);
            syscall_frame_t child_frame = *frame;
            child_frame.rax             = 0;

            if (vfork && num == SYS_CLONE) child_frame.rsp = frame->rsi;
            kstack -= sizeof(syscall_frame_t) / sizeof(uint64_t);
            memcpy(kstack, &child_frame, sizeof(syscall_frame_t));

            *(--kstack)              = (uint64_t)syscall_return;
            child->task->context.rsp = (uint64_t)kstack;
            uint32_t child_tid       = (uint32_t)child->task->pid;
            int      tid_error       = 0;

            if ((clone_flags & CLONE_PARENT_SETTID) && copy_to_user((void *)frame->rdx, &child_tid, sizeof(child_tid))) tid_error = -EFAULT;
            if (!tid_error && (clone_flags & CLONE_CHILD_SETTID)
                && (!user_access_ok_process(child, (void *)frame->r10, sizeof(child_tid), 1) || copy_to_user_process_nofault(child, (void *)frame->r10, &child_tid, sizeof(child_tid))))
                tid_error = -EFAULT;
            if (tid_error) {
                process_fork_discard(child);
                child = NULL;
                error = tid_error;
            } else if (clone_flags & CLONE_CHILD_CLEARTID) {
                child->task->clear_child_tid = frame->r10;
            }
        }
        if (child) {
            /*
             * Apply CLONE_NEW* to the child (sd-mkdcreds needs CLONE_NEWNS|CLONE_NEWUSER
             * for its mount sandbox); the parent does it before the child's seeded frame runs.
             */
            uint64_t ns_flags = clone_flags & (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET);
            if (ns_flags && child->task) {
                int        ns_err = EOK;
                nsproxy_t *new_ns = nsproxy_clone(child->task->nsproxy, ns_flags, &ns_err);
                if (new_ns) {
                    if (child->task->nsproxy) nsproxy_put(child->task->nsproxy);
                    child->task->nsproxy = new_ns;

                    if (child->nsproxy) nsproxy_put(child->nsproxy);
                    child->nsproxy = nsproxy_get(new_ns);
                }
            }
        }
        if (child) process_fork_publish(child);
        retval     = child ? (int64_t)child->task->pid : (int64_t)error;
        frame->rax = (uint64_t)retval;

        if (child) ptrace_fork_event(frame, event, child->task->pid);
        if (child && vfork) {
            process_vfork_wait(child);
            ptrace_fork_event(frame, PTRACE_EVENT_VFORK_DONE, child->task->pid);
        }
        goto check_signals;
    }

    /* clone3 needs the frame, not the table ABI: it seeds the child's frame and clones threads from the parent. */
    if (num == SYS_CLONE3) {
        retval     = sys_clone3_impl(frame, frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }

    /* read/write dominate streaming; the task is already known, so skip the extra lookup and indirect call. */
    if (num == SYS_READ) {
        retval     = sys_read_task(dispatch_task, frame->rdi, frame->rsi, frame->rdx);
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }
    if (num == SYS_WRITE) {
        retval     = sys_write_task(dispatch_task, frame->rdi, frame->rsi, frame->rdx);
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }
    if (num >= SYS_MAX || !syscall_table[num]) {
        if (syscall_log_missing_once(num))
            plogk("syscall: syscall %llu is not implemented (pid %llu, %s) (not in table)\n", num, dispatch_task ? dispatch_task->pid : 0ULL, dispatch_task ? dispatch_task->name : "?");
        retval     = -ENOSYS;
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }
    if (syscall_table[num] == sys_unimplemented) {
        if (syscall_log_missing_once(num))
            plogk("syscall: syscall %llu is not implemented (pid %llu, %s)\n", num, dispatch_task ? dispatch_task->pid : 0ULL, dispatch_task ? dispatch_task->name : "?");
        retval     = -ENOSYS;
        frame->rax = (uint64_t)retval;
        goto check_signals;
    }
    if (num == SYS_EXECVE) {
        retval     = do_execve((const char *)frame->rdi, (char *const *)frame->rsi, (char *const *)frame->rdx, frame);
        frame->rax = (uint64_t)retval;
        if (!retval) ptrace_exec_event(frame);
        goto check_signals;
    }
    if (num == SYS_EXECVEAT) {
        retval     = do_execveat(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame);
        frame->rax = (uint64_t)retval;
        if (!retval) ptrace_exec_event(frame);
        goto check_signals;
    }
    if (num == SYS_RT_SIGRETURN) {
        /*
         * do_rt_sigreturn fills in ALL fields (rax too) from the user-stack signal frame,
         * so never override frame->rax here; the restored mask may unblock signals.
         */
        int64_t sr_ret = do_rt_sigreturn(frame);
        if (sr_ret != 0) {
            /* Bad signal frame is fatal: resuming the restorer runs the stub's tail and raises a bogus #GP. */
            process_exit_group(-SIGSEGV);
            return 0;
        }

        /* Success: frame fully restored, so skip the retval/restart paths and only check pending signals. */
        disable_intr();
        if (frame->cs & 0x3) (void)signal_deliver_for_process(dispatch_task ? dispatch_task->process : NULL, frame);
        return 0;
    }

    retval     = syscall_table[num](frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
    frame->rax = (uint64_t)retval;
check_signals:
    if (traced) {
        force_iret = true;
        ptrace_syscall_exit(frame, (int64_t)frame->rax);
    }
    retval = (int64_t)frame->rax;

    /* Bodies run with IRQs on; close the exit-work race first, as later IPIs stay pending until SYSRET. */
    disable_intr();

    /*
     * Deliver pending signals first: handler frames are installed (RIP/RSP redirected)
     * but frame->rax stays untouched.  Then decide the restart: a restart code
     * (-ERESTARTSYS, ...) with a handler installed is re-run (RIP moved back before the
     * syscall) or turned into -EINTR; a completed call keeps its return value.
     */
    if ((frame->cs & 0x3) && syscall_signal_work_pending(dispatch_task)) {
        uint64_t saved_rip = frame->rip;
        int      sig_ret   = signal_deliver_for_process(dispatch_task ? dispatch_task->process : NULL, frame);

        if (frame->rip != saved_rip) force_iret = true;
        if (sig_ret == 1) {
            /* Killed by signal default action (signal_deliver_if_pending already called process_exit) */
            task_exit();
            return 0;
        }

        /* Handle syscall restart if the syscall was interrupted */
        if (is_restart_code(retval)) {
            /* Interrupted by a pending signal (now delivered); restart the call or return -EINTR. */
            bool restart = false;

            if (retval == -ERESTARTNOINTR || retval == -ERESTART_RESTARTBLOCK) {
                /* Always restart regardless of SA_RESTART */
                restart = true;
            } else if (retval == -ERESTARTSYS) {
                /*
                 * -ERESTARTSYS wants "restart only if SA_RESTART", but the signal number is
                 * not plumbed back from signal_deliver_if_pending().  A changed RIP proves a
                 * user handler ran, so without SA_RESTART knowledge fall back to -EINTR.
                 */
                if (frame->rip == saved_rip) {
                    restart = true;
                } else {
                    restart = false;
                }
            } else if (retval == -ERESTARTNOHAND) {
                /* Restart only if no user handler was set up */
                restart = (frame->rip == saved_rip);
            } else if (retval == -ERESTART) {
                /* Legacy ERESTART: restart if SA_RESTART */
                if (frame->rip == saved_rip) restart = true;
            }
            if (restart) {
                /*
                 * Rewind to re-execute: RAX = original number, RIP = the int 0x80/syscall
                 * site.  Both encodings are 2 bytes, so the saved RIP sits 2 bytes past it.
                 */
                frame->rax = num;
                frame->rip = saved_rip - 2;
            } else {
                /* Not restarting: return -EINTR to userspace */
                frame->rax = (uint64_t)-EINTR;
            }
        }
    }

    /*
     * ERESTART* is kernel-internal: this final guard stops a future early exit or
     * signal-path change from leaking errno 512+ to libc (unknown/no-detail recvfrom);
     * a real restart already replaced RAX with the original syscall number.
     */
    if (is_restart_code((int64_t)frame->rax)) frame->rax = (uint64_t)-EINTR;

    /*
     * Let freshly woken input/compositor peers run before returning; test need_resched
     * inline rather than calling sched_maybe_preempt() (a call + CPU lookup) per syscall.
     */
    uint32_t cpu_id = percpu_gs_cpu_id();
    if (cpu_rqs && cpu_id < cpu_scheduler_count && __atomic_load_n(&cpu_rqs[cpu_id].need_resched, __ATOMIC_ACQUIRE)) sched_maybe_preempt();

    /*
     * Pick SYSRET (1) vs IRETQ (0); the stubs test %eax.  IRET is forced by non-user
     * selectors (CS 0x33/SS 0x2b), RIP/RSP outside the canonical lower half (SYSRET #GPs
     * there and on vsyscall), AC set (SMAP), or a restore that must bring back RCX/R11.
     */
    if (force_iret || frame->cs != 0x33 || frame->ss != 0x2b || frame->rip >= X86_64_CANONICAL_BOUNDARY || frame->rsp >= X86_64_CANONICAL_BOUNDARY || (frame->rflags & X86_EFLAGS_AC)) return 0;
    return 1;
}

/*
 * Restore registers and return via IRETQ: swapgs puts back the user GS (%gs is per-CPU
 * in the kernel) and cli keeps timers out of the swapgs..iretq window (iretq restores IF);
 * also the fork/clone/thread resume path that context_switch() reaches with IF set.
 */
__attribute__((naked, used)) void syscall_return(void)
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
                     "cli\n\t"
                     "swapgs\n\t"
                     "iretq\n\t");
}

/* Restore registers and return via SYSRET */
__attribute__((naked, used)) void syscall_return_sysret(void)
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
                     "movq 0(%rsp), %rcx\n\t"
                     "movq 16(%rsp), %r11\n\t"
                     "cli\n\t"
                     "movq 24(%rsp), %rsp\n\t"
                     "swapgs\n\t"
                     "sysretq\n\t");
}

/* Interrupt-based syscall entry point (int 0x80, always from user mode) */
__attribute__((naked)) void syscall_entry(void)
{
    __asm__ volatile("swapgs\n\t"
                     "cld\n\t"
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
                     "sti\n\t"
                     "call syscall_dispatch\n\t"
                     "cli\n\t"
                     "jmp syscall_return\n\t");
}

/* SYSCALL-instruction entry point */
__attribute__((naked)) static void syscall_entry_syscall(void)
{
    __asm__ volatile(
        "swapgs\n\t"
        "movq %rsp, %gs:" SYSCALL_STRINGIFY(
            SYSCALL_CPU_USER_RSP_OFFSET) "\n\t"
                                         "movq %gs:" SYSCALL_STRINGIFY(
                                             SYSCALL_CPU_KERNEL_RSP_OFFSET) ", %rsp\n\t"
                                                                            "cld\n\t"
                                                                            "pushq $0x2B\n\t"
                                                                            "pushq %gs:" SYSCALL_STRINGIFY(
                                                                                SYSCALL_CPU_USER_RSP_OFFSET) "\n\t"
                                                                                                             "pushq %r11\n\t"
                                                                                                             "pushq $0x33\n\t"
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
                                                                                                             "sti\n\t"
                                                                                                             "call syscall_dispatch\n\t"
                                                                                                             "cli\n\t"

                                                                                                             /* eax = syscall_dispatch(): 0 -> IRETQ, 1 -> SYSRET (see its exit criteria) */
                                                                                                             "testl %eax, %eax\n\t"
                                                                                                             "jz syscall_return\n\t"
                                                                                                             "jmp syscall_return_sysret\n\t");
}

/* Program this CPU's SYSCALL MSRs; cpu_gs_install in smp.c sets the kernel GS base before sched_init(). */
void syscall_init_cpu(void)
{
    uint64_t star = rdmsr(0xC0000081);
    star &= 0x00000000FFFFFFFFULL;

    /* SYSRET adds 16 to STAR[63:48] for CS and 8 for SS. */
    star |= ((uint64_t)0x08 << 32) | ((uint64_t)0x23 << 48);
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (uint64_t)syscall_entry_syscall);
    wrmsr(0xC0000084, X86_EFLAGS_IF);
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);
}

/* Install the syscall entry points */
void syscall_init(void)
{
    register_interrupt_handler(SYSCALL_VECTOR, (void *)syscall_entry, 0, 0xee);

    cpu_processor_t *cpu = get_current_cpu();
    if (!cpu) panic("syscall: BSP per-CPU state is unavailable.");

    syscall_init_cpu();
    plogk("syscall: int 0x%02x interface initialized.\n", SYSCALL_VECTOR);
}
