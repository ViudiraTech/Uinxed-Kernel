/*
 *
 *      timerfd.h
 *      Timerfd file descriptor header file
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TIMERFD_H_
#define INCLUDE_TIMERFD_H_

#include <fs/core/vfs.h>
#include <libs/list/intrusive_list.h>
#include <libs/std/stdint.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#define TFD_CLOEXEC             (1 << 19)
#define TFD_NONBLOCK            (1 << 11)
#define TFD_TIMER_ABSTIME       (1 << 0)
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)

#define CLOCK_REALTIME       0
#define CLOCK_MONOTONIC      1
#define CLOCK_BOOTTIME       7
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9

typedef struct timerfd_ctx {
        vfs_node_t   node;
        uint64_t     clockid;
        uint64_t     flags;
        uint64_t     expire_count;
        uint64_t     interval_ns;
        uint64_t     deadline_tick;
        int          armed;
        spinlock_t   lock;
        wait_queue_t wq;
        ilist_node_t timers;
} timerfd_ctx_t;

/* Create a new timerfd file descriptor */
int sys_timerfd_create(int clockid, int flags);

/* Arm or disarm the timerfd */
int sys_timerfd_settime(int fd, int flags, const void *new_value, void *old_value);

/* Get the current timerfd status */
int sys_timerfd_gettime(int fd, void *curr_value);

/* Initialize the timerfd subsystem */
void timerfd_init(void);

/* Account armed timerfds from the scheduler's global tick. */
void timerfd_tick(void);

#endif // INCLUDE_TIMERFD_H_
