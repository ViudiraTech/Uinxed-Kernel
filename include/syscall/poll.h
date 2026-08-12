/*
 *
 *      poll.h
 *      Linux-compatible poll/select syscall family definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_POLL_H_
#define INCLUDE_POLL_H_

#include <libs/std/stdint.h>

/* Linux ABI time types used by poll/select and clock syscalls. */
typedef struct {
        int64_t tv_sec;
        int64_t tv_nsec;
} linux_timespec_t;

typedef struct {
        int64_t tv_sec;
        int64_t tv_usec;
} linux_timeval_t;

/* Poll event flags (Linux-compatible) */
#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200
#define POLLMSG    0x0400
#define POLLRDHUP  0x2000

/* Poll and select syscall implementations. */
int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t arg5);
int64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t sigmask);
int64_t sys_ppoll(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize, uint64_t arg5);

#endif // INCLUDE_POLL_H_
