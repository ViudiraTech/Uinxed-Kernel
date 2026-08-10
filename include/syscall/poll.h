/*
 *
 *      poll.h
 *      Linux-compatible poll/select syscall family definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_POLL_H_
#define INCLUDE_POLL_H_

#include <libs/std/stdint.h>

int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t arg5);
int64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t sigmask);
int64_t sys_ppoll(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize, uint64_t arg5);

#endif // INCLUDE_POLL_H_
