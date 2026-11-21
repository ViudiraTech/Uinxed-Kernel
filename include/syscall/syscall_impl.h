/*
 *
 *      syscall_impl.h
 *      Internal declarations for syscall implementations
 *
 *      2026/8/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_IMPL_H_
#define INCLUDE_SYSCALL_IMPL_H_

#include <libs/std/stdint.h>
#include <process/process.h>
#include <syscall/syscall.h>

/* kernel/syscall/syscall_fs.c */
int64_t sys_fstat(uint64_t fd, uint64_t statbuf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_statx(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mask, uint64_t statbuf, uint64_t arg5);
int64_t sys_stat(uint64_t path, uint64_t statbuf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t statbuf, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_unlink(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rmdir(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rename(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_renameat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t arg4, uint64_t arg5);
int64_t sys_link(uint64_t oldpath, uint64_t newpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_linkat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5);
int64_t sys_symlink(uint64_t target, uint64_t linkpath, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_symlinkat(uint64_t target, uint64_t newdirfd, uint64_t linkpath, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_readlinkat(uint64_t dirfd, uint64_t path, uint64_t buf, uint64_t bufsiz, uint64_t arg4, uint64_t arg5);
int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_name_to_handle_at_impl(uint64_t dirfd, uint64_t path, uint64_t handle, uint64_t mount_id, uint64_t flags, uint64_t arg5);
int64_t sys_open_by_handle_at_impl(uint64_t dirfd, uint64_t handle, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_access_impl(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_faccessat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_faccessat2_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_chmod_impl(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fchmod_impl(uint64_t fd, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fchmodat2_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_chown_impl(uint64_t path, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_lchown_impl(uint64_t path, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fchown_impl(uint64_t fd, uint64_t owner, uint64_t group, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mknod_impl(uint64_t path, uint64_t mode, uint64_t dev, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_statfs_impl(uint64_t path, uint64_t buf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fstatfs_impl(uint64_t fd, uint64_t buf, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_flock_impl(uint64_t fd, uint64_t operation, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_utime_impl(uint64_t filename, uint64_t times, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_utimes_impl(uint64_t filename, uint64_t times, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fchownat_impl(uint64_t dirfd, uint64_t path, uint64_t owner, uint64_t group, uint64_t flags, uint64_t arg5);
int64_t sys_futimesat_impl(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fchmodat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_chdir_impl(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fchdir_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_listxattr_impl(uint64_t path, uint64_t list, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setxattr_impl(uint64_t path, uint64_t name, uint64_t value, uint64_t size, uint64_t flags, uint64_t arg5);
int64_t sys_getxattr_impl(uint64_t path, uint64_t name, uint64_t value, uint64_t size, uint64_t arg4, uint64_t arg5);
int64_t sys_removexattr_impl(uint64_t path, uint64_t name, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_renameat2_impl(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd, uint64_t newpath, uint64_t flags, uint64_t arg5);
int64_t sys_utimensat_impl(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_mknodat_impl(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t dev, uint64_t arg4, uint64_t arg5);

/* kernel/syscall/syscall_io.c */
int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode, uint64_t arg4, uint64_t arg5);
int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_creat(uint64_t path, uint64_t mode, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_close(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_read_task(task_t *task, uint64_t fd, uint64_t buf, uint64_t size);
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_write_task(task_t *task, uint64_t fd, uint64_t buf, uint64_t size);
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t size, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_dup(uint64_t oldfd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fsync_impl(uint64_t fd, uint64_t data_only, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fdatasync_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_syncfs_impl(uint64_t fd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_readahead(uint64_t fd, uint64_t offset, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fadvise64(uint64_t fd, uint64_t offset, uint64_t len, uint64_t advice, uint64_t arg4, uint64_t arg5);
int64_t sys_copy_file_range_wrap(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out, uint64_t len, uint64_t flags);
int64_t sys_getdents64_wrap(uint64_t fd, uint64_t dirent, uint64_t count, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_writev_wrap(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_readv_wrap(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_fcntl_wrap(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_truncate_impl(uint64_t path, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_ftruncate_impl(uint64_t fd, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sync_impl(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pread64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5);
int64_t sys_pwrite64_impl(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset, uint64_t arg4, uint64_t arg5);
int64_t sys_fallocate_impl(uint64_t fd, uint64_t mode, uint64_t offset, uint64_t len, uint64_t arg4, uint64_t arg5);
int64_t sys_sync_file_range_impl(uint64_t fd, uint64_t offset, uint64_t nbytes, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_sendfile_impl(uint64_t out_fd, uint64_t in_fd, uint64_t offset, uint64_t count, uint64_t arg4, uint64_t arg5);
int64_t sys_preadv_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t arg4, uint64_t arg5);
int64_t sys_pwritev_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t arg4, uint64_t arg5);
int64_t sys_preadv2_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t flags, uint64_t arg5);
int64_t sys_pwritev2_impl(uint64_t fd, uint64_t iov, uint64_t iovcnt, uint64_t offset, uint64_t flags, uint64_t arg5);
int64_t sys_splice_impl(uint64_t fd_in, uint64_t off_in, uint64_t fd_out, uint64_t off_out, uint64_t len, uint64_t flags);
int64_t sys_tee_impl(uint64_t fd_in, uint64_t fd_out, uint64_t len, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_vmsplice_impl(uint64_t fd, uint64_t iov, uint64_t nr_segs, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_openat2_impl(uint64_t dirfd, uint64_t path, uint64_t how, uint64_t usize, uint64_t arg4, uint64_t arg5);
int64_t sys_io_pgetevents_impl(uint64_t ctx_id, uint64_t min_nr, uint64_t nr, uint64_t events, uint64_t timeout, uint64_t sigmask);

/* kernel/syscall/syscall_process.c */
int64_t sys_exit(uint64_t status, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_wait4(uint64_t pid, uint64_t exit_code, uint64_t options, uint64_t rusage, uint64_t arg4, uint64_t arg5);
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_exit_group(uint64_t status, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getppid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_gettid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_ptrace_wrap(uint64_t request, uint64_t pid, uint64_t addr, uint64_t data, uint64_t arg4, uint64_t arg5);
int64_t sys_getrusage_impl(uint64_t who, uint64_t usage, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_restart_syscall(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getrlimit_impl(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setrlimit_impl(uint64_t resource, uint64_t rlim, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_prlimit64_impl(uint64_t pid, uint64_t resource, uint64_t new_rlim, uint64_t old_rlim, uint64_t arg4, uint64_t arg5);
int64_t do_execveat(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, syscall_frame_t *frame);
int64_t sys_execveat_wrap(uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags, uint64_t arg5);
int64_t sys_membarrier_wrap(uint64_t cmd, uint64_t flags, uint64_t cpu_id, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rseq_impl(uint64_t rseq_base, uint64_t rseq_len, uint64_t flags, uint64_t sig, uint64_t arg4, uint64_t arg5);
int64_t sys_pidfd_open_impl(uint64_t pid_raw, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_clone3_impl(syscall_frame_t *frame, uint64_t cl_args, uint64_t size, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_sigaction_wrap(uint64_t sig, uint64_t act, uint64_t oact, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_sigprocmask_wrap(uint64_t how, uint64_t set, uint64_t oset, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_sigreturn_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_sigpending_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_sigtimedwait_wrap(uint64_t set, uint64_t info, uint64_t timeout, uint64_t sigsetsize, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_sigqueueinfo_wrap(uint64_t pid, uint64_t sig, uint64_t info, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_sigsuspend_wrap(uint64_t set, uint64_t sigsetsize, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sigaltstack_wrap(uint64_t ss, uint64_t oss, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pause_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_tgkill_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_rt_tgsigqueueinfo_wrap(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t info, uint64_t arg4, uint64_t arg5);
int64_t sys_setpgid_wrap(uint64_t pid, uint64_t pgid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getpgrp_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setsid_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getsid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getpgid_wrap(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_waitid_impl(uint64_t which, uint64_t upid, uint64_t infop, uint64_t options, uint64_t arg4, uint64_t arg5);
int64_t do_execve(const char *path, char *const argv[], char *const envp[], syscall_frame_t *frame);
int64_t sys_execve_wrap(uint64_t path, uint64_t argv, uint64_t envp, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_prctl_impl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_set_robust_list_impl(uint64_t head, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_get_robust_list_impl(uint64_t pid, uint64_t head_ptr, uint64_t len_ptr, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_tkill_wrap(uint64_t tid, uint64_t sig, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_set_tid_address_impl(uint64_t tidptr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pidfd_send_signal_impl(uint64_t pidfd, uint64_t sig, uint64_t info, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_process_vm_readv_impl(uint64_t pid, uint64_t local_iov, uint64_t local_iovcnt, uint64_t remote_iov, uint64_t remote_iovcnt, uint64_t flags);
int64_t sys_process_vm_writev_impl(uint64_t pid, uint64_t local_iov, uint64_t local_iovcnt, uint64_t remote_iov, uint64_t remote_iovcnt, uint64_t flags);
int64_t sys_unshare_impl(uint64_t unshare_flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setns_impl(uint64_t fd, uint64_t nstype, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pidfd_getfd_impl(uint64_t pidfd, uint64_t targetfd, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* kernel/syscall/syscall_system.c */
int64_t sys_uname(uint64_t name, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getuid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getgid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mount(uint64_t source, uint64_t target, uint64_t fstype, uint64_t flags, uint64_t data, uint64_t arg5);
int64_t sys_umount2(uint64_t target, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_reboot_impl(uint64_t magic, uint64_t magic2, uint64_t cmd, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_personality_impl(uint64_t persona, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sysinfo_impl(uint64_t info, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_chroot_wrap(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_init_module_impl(uint64_t image, uint64_t length, uint64_t user_params, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_finit_module_impl(uint64_t fd, uint64_t user_params, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_delete_module_impl(uint64_t user_name, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getgroups_impl(uint64_t size, uint64_t list, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setgroups_impl(uint64_t size, uint64_t list, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_capget_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_capset_impl(uint64_t header, uint64_t data, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getpriority_impl(uint64_t which, uint64_t who, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setpriority_impl(uint64_t which, uint64_t who, uint64_t niceval, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_getparam_impl(uint64_t pid, uint64_t param, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_getscheduler_impl(uint64_t pid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_get_priority_max_impl(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_get_priority_min_impl(uint64_t policy, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_rr_get_interval_impl(uint64_t pid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_setaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_getaffinity_impl(uint64_t pid, uint64_t cpusetsize, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_setattr_impl(uint64_t pid, uint64_t attr, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_getattr_impl(uint64_t pid, uint64_t attr, uint64_t size, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_sethostname_impl(uint64_t name, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setdomainname_impl(uint64_t name, uint64_t len, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setuid_impl(uint64_t uid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setgid_impl(uint64_t gid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setreuid_impl(uint64_t ruid, uint64_t euid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setregid_impl(uint64_t rgid, uint64_t egid, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setresuid_impl(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setresgid_impl(uint64_t rgid, uint64_t egid, uint64_t sgid, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setfsuid_impl(uint64_t uid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setfsgid_impl(uint64_t gid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getresuid_impl(uint64_t ruid, uint64_t euid, uint64_t suid, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getresgid_impl(uint64_t rgid, uint64_t egid, uint64_t sgid, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_umask_impl(uint64_t mask, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getcpu_impl(uint64_t cpu, uint64_t node, uint64_t tcache, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getrandom_impl(uint64_t buf, uint64_t buflen, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_ioprio_get_impl(uint64_t which, uint64_t who, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* kernel/syscall/syscall_ipc.c */
int64_t sys_eventfd_wrap(uint64_t initval, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_eventfd2_wrap(uint64_t initval, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_timerfd_create_wrap(uint64_t clockid, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_timerfd_settime_wrap(uint64_t fd, uint64_t flags, uint64_t new_value, uint64_t old_value, uint64_t arg4, uint64_t arg5);
int64_t sys_timerfd_gettime_wrap(uint64_t fd, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_signalfd_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_signalfd4_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_inotify_init_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_inotify_init1_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_inotify_add_watch_wrap(uint64_t fd, uint64_t pathname, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_inotify_rm_watch_wrap(uint64_t fd, uint64_t wd, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_epoll_pwait2_impl(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t tsp, uint64_t sigmask, uint64_t sigsetsize);
int64_t sys_socket_wrap(uint64_t family, uint64_t type, uint64_t protocol, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_bind_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_listen_wrap(uint64_t fd, uint64_t backlog, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_accept_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_accept4_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_connect_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sendto_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen);
int64_t sys_recvfrom_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen);
int64_t sys_sendmsg_wrap(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_recvmsg_wrap(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_shutdown_wrap(uint64_t fd, uint64_t how, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_socketpair_wrap(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv, uint64_t arg4, uint64_t arg5);
int64_t sys_getsockname_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_getpeername_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setsockopt_wrap(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen, uint64_t arg5);
int64_t sys_getsockopt_wrap(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen, uint64_t arg5);
int64_t sys_sendmmsg_wrap(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags, uint64_t arg4, uint64_t arg5);
int64_t sys_recvmmsg_wrap(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags, uint64_t timeout, uint64_t arg5);
int64_t sys_pipe_wrap(uint64_t pipefd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pipe2_wrap(uint64_t pipefd, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_semget_wrap(uint64_t key, uint64_t nsems, uint64_t semflg, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_semop_wrap(uint64_t semid, uint64_t sops, uint64_t nsops, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_semtimedop_wrap(uint64_t semid, uint64_t sops, uint64_t nsops, uint64_t timeout, uint64_t arg4, uint64_t arg5);
int64_t sys_semctl_wrap(uint64_t semid, uint64_t semnum, uint64_t cmd, uint64_t arg, uint64_t arg4, uint64_t arg5);
int64_t sys_shmget_wrap(uint64_t key, uint64_t size, uint64_t shmflg, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_shmat_wrap(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_shmdt_wrap(uint64_t shmaddr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_shmctl_wrap(uint64_t shmid, uint64_t cmd, uint64_t buf, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_msgget_wrap(uint64_t key, uint64_t msgflg, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_msgsnd_wrap(uint64_t msqid, uint64_t msgp, uint64_t msgsz, uint64_t msgflg, uint64_t arg4, uint64_t arg5);
int64_t sys_msgrcv_wrap(uint64_t msqid, uint64_t msgp, uint64_t msgsz, uint64_t msgtyp, uint64_t msgflg, uint64_t arg5);
int64_t sys_msgctl_wrap(uint64_t msqid, uint64_t cmd, uint64_t buf, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mq_open_wrap(uint64_t name, uint64_t oflag, uint64_t mode, uint64_t attr, uint64_t arg4, uint64_t arg5);
int64_t sys_mq_unlink_wrap(uint64_t name, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mq_timedsend_wrap(uint64_t mqdes, uint64_t msg_ptr, uint64_t msg_len, uint64_t msg_prio, uint64_t abs_timeout, uint64_t arg5);
int64_t sys_mq_timedreceive_wrap(uint64_t mqdes, uint64_t msg_ptr, uint64_t msg_len, uint64_t msg_prio, uint64_t abs_timeout, uint64_t arg5);
int64_t sys_mq_notify_wrap(uint64_t mqdes, uint64_t notification, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mq_getsetattr_wrap(uint64_t mqdes, uint64_t newattr, uint64_t oldattr, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_futex_wrap(uint64_t uaddr, uint64_t futex_op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint64_t val3);
int64_t sys_epoll_create_wrap(uint64_t size, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_epoll_create1_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_epoll_ctl_wrap(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event, uint64_t arg4, uint64_t arg5);
int64_t sys_epoll_wait_wrap(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout, uint64_t arg4, uint64_t arg5);
int64_t sys_epoll_pwait_wrap(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize);

/* kernel/syscall/syscall_time.c */
int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_gettimeofday(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_time(uint64_t tloc, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_clock_settime_impl(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_clock_nanosleep_impl(uint64_t clockid, uint64_t flags, uint64_t req, uint64_t rem, uint64_t arg4, uint64_t arg5);
int64_t sys_getitimer_impl(uint64_t which, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_setitimer_impl(uint64_t which, uint64_t new_value, uint64_t old_value, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_alarm_impl(uint64_t seconds, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_times_impl(uint64_t tms, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_clock_gettime_impl(uint64_t clockid, uint64_t tp, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_clock_getres_impl(uint64_t clockid, uint64_t res, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_settimeofday_impl(uint64_t tv, uint64_t tz, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* kernel/syscall/syscall_mem.c */
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t pgoff);
int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_brk(uint64_t addr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mlock2_wrap(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pkey_mprotect_wrap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t pkey, uint64_t arg4, uint64_t arg5);
int64_t sys_process_madvise_impl(uint64_t pidfd, uint64_t iovec, uint64_t vlen, uint64_t advice, uint64_t flags, uint64_t arg5);
int64_t sys_mprotect_wrap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mremap_wrap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr, uint64_t arg5);
int64_t sys_msync_wrap(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mincore_wrap(uint64_t addr, uint64_t length, uint64_t vec, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_madvise_wrap(uint64_t addr, uint64_t length, uint64_t advice, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_munlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_mlockall_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_munlockall_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_swapon(uint64_t path, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_swapoff(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pkey_alloc_impl(uint64_t flags, uint64_t access_rights, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_pkey_free_impl(uint64_t pkey, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif // INCLUDE_SYSCALL_IMPL_H_
