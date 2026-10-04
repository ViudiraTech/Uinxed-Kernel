/*
 *
 *      posix_timer.h
 *      Per-process POSIX interval timers (timer_create and friends)
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_POSIX_TIMER_H_
#define INCLUDE_POSIX_TIMER_H_

#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

struct process;

/* Deferred bottom-half hooks, driven from the timer tick. */
bool posix_timer_deferred_due(uint64_t monotonic_ns);
void posix_timer_tick(void);

/* Release every timer owned by a process that is exiting. */
void posix_timer_release_process(struct process *proc);

int64_t sys_timer_create(uint64_t clockid, uint64_t sigevent, uint64_t timerid_ptr, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_timer_settime(uint64_t timerid, uint64_t flags, uint64_t new_value, uint64_t old_value, uint64_t arg4, uint64_t arg5);
int64_t sys_timer_gettime(uint64_t timerid, uint64_t cur_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_timer_getoverrun(uint64_t timerid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_timer_delete(uint64_t timerid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif // INCLUDE_POSIX_TIMER_H_
