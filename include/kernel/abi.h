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

#endif // INCLUDE_ABI_H_
