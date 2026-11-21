/*
 *
 *      abi.h
 *      User/kernel ABI memory layouts shared by the syscall, network and ptrace layers
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ABI_H_
#define INCLUDE_ABI_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/*
 * One name per ABI concept.  The kernel has no 32-bit compat layout, so the
 * legacy and the 64-bit spellings of these structures never differ and must
 * not be duplicated under two names.
 */

/* Linux UAPI struct timespec. */
typedef struct {
        int64_t tv_sec;
        int64_t tv_nsec;
} linux_timespec_t;

_Static_assert(sizeof(linux_timespec_t) == 16, "Linux x86_64 timespec ABI size");

/* Linux UAPI struct timeval. */
typedef struct {
        int64_t tv_sec;
        int64_t tv_usec;
} linux_timeval_t;

_Static_assert(sizeof(linux_timeval_t) == 16, "Linux x86_64 timeval ABI size");

/* Linux UAPI struct iovec, used by readv/writev, sendmsg/recvmsg and ptrace regsets. */
typedef struct iovec {
        void  *iov_base;
        size_t iov_len;
} iovec_t;

_Static_assert(sizeof(iovec_t) == 16, "Linux x86_64 iovec ABI size");

/* Linux UAPI struct stat. */
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

_Static_assert(sizeof(linux_stat_t) == 144, "Linux x86_64 struct stat ABI size");

/* Linux UAPI struct new_utsname. */
typedef struct {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
} linux_utsname_t;

_Static_assert(sizeof(linux_utsname_t) == 390, "Linux x86_64 new_utsname ABI size");

/* Linux UAPI struct statx_timestamp. */
typedef struct {
        int64_t  tv_sec;
        uint32_t tv_nsec;
        int32_t  __reserved;
} linux_statx_timestamp_t;

_Static_assert(sizeof(linux_statx_timestamp_t) == 16, "Linux x86_64 statx_timestamp ABI size");

/* Linux UAPI struct statx. */
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
        uint64_t                stx_mnt_id;
        uint32_t                stx_dio_mem_align;
        uint32_t                stx_dio_offset_align;
        uint64_t                stx_subvol;
        uint32_t                stx_atomic_write_unit_min;
        uint32_t                stx_atomic_write_unit_max;
        uint32_t                stx_atomic_write_segments_max;
        uint32_t                stx_dio_read_offset_align;
        uint64_t                __spare3[9];
} linux_statx_t;

_Static_assert(sizeof(linux_statx_t) == 256, "Linux x86_64 statx ABI size");

/* Linux UAPI struct sysinfo. */
typedef struct linux_sysinfo {
        int64_t  uptime;
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
        char     _f[];
} linux_sysinfo_t;

_Static_assert(sizeof(linux_sysinfo_t) == 112, "Linux x86_64 sysinfo ABI size");

/* Linux UAPI struct statfs. */
typedef struct linux_statfs {
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
} linux_statfs_t;

_Static_assert(sizeof(linux_statfs_t) == 120, "Linux x86_64 statfs ABI size");

/* Linux UAPI struct rusage. */
typedef struct linux_rusage {
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
} linux_rusage_t;

_Static_assert(sizeof(linux_rusage_t) == 144, "Linux x86_64 rusage ABI size");

/* Linux UAPI struct rlimit64. */
typedef struct linux_rlimit64 {
        uint64_t rlim_cur;
        uint64_t rlim_max;
} linux_rlimit64_t;

_Static_assert(sizeof(linux_rlimit64_t) == 16, "Linux x86_64 rlimit64 ABI size");

/* Linux UAPI struct clone_args. */
typedef struct clone3_args {
        uint64_t flags;
        uint64_t pidfd;
        uint64_t child_tid;
        uint64_t parent_tid;
        uint64_t exit_signal;
        uint64_t stack;
        uint64_t stack_size;
        uint64_t tls;
        uint64_t set_tid;
        uint64_t set_tid_size;
        uint64_t cgroup;
} clone3_args_t;

_Static_assert(sizeof(clone3_args_t) == 88, "Linux x86_64 struct clone_args ABI size");

/* Linux UAPI struct linux_dirent64. */
typedef struct linux_dirent64 {
        uint64_t       d_ino;
        int64_t        d_off;
        unsigned short d_reclen;
        unsigned char  d_type;
        char           d_name[];
} linux_dirent64_t;

_Static_assert(offsetof(linux_dirent64_t, d_name) == 19, "Linux x86_64 dirent64 d_name offset");

/* sizeof is 24 (tail padding); Linux sizes a record from offsetof(d_name) = 19. */
_Static_assert(sizeof(linux_dirent64_t) == 24, "x86_64 dirent64 current size");

#endif // INCLUDE_ABI_H_
