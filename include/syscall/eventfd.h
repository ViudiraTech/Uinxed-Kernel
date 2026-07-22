/*
 *
 *      eventfd.h
 *      Eventfd file descriptor header file
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EVENTFD_H_
#define INCLUDE_EVENTFD_H_

#include <fs/vfs.h>
#include <libs/std/stdint.h>
#include <proc/task.h>
#include <sync/spin_lock.h>

#define EFD_SEMAPHORE (1 << 0)
#define EFD_CLOEXEC   (1 << 19)
#define EFD_NONBLOCK  (1 << 11)

typedef struct eventfd_ctx {
        uint64_t     count;
        uint64_t     flags;
        spinlock_t   lock;
        wait_queue_t wq;
} eventfd_ctx_t;

/* Create a new eventfd file descriptor for the current process */
int sys_eventfd(unsigned int initval, int flags);

/* Create a new eventfd2 file descriptor (alias, same as eventfd with flags) */
int sys_eventfd2(unsigned int initval, int flags);

/* Initialize the eventfd subsystem */
void eventfd_init(void);

/* Get the installed eventfd callback */
extern vfs_callback_t eventfd_callback_installed;

#endif /* INCLUDE_EVENTFD_H_ */