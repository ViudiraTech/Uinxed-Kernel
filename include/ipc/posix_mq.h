/*
 *
 *      posix_mq.h
 *      POSIX Message Queues header file
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_POSIX_MQ_H_
#define INCLUDE_POSIX_MQ_H_

#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/signal.h>

/* sigev_notify values live in <sync/signal.h> with the rest of the signal ABI. */

/* mq_attr structure */

typedef struct mq_attr {
        int64_t mq_flags;
        int64_t mq_maxmsg;
        int64_t mq_msgsize;
        int64_t mq_curmsgs;
        int64_t __pad[4];
} mq_attr_t;

_Static_assert(sizeof(mq_attr_t) == 64, "Linux x86_64 mq_attr ABI size (four longs plus four padding longs)");

/* sigevent structure (for mq_notify) */

typedef struct sigevent {
        sigval_t sigev_value;
        int32_t  sigev_signo;
        int32_t  sigev_notify;
        void (*sigev_notify_function)(sigval_t);
        void   *sigev_notify_attributes;
        int32_t __pad[12];
} sigevent_t;

/* glibc sigevent is 64 bytes (__pad[8]); mq_notify's copy_from_user over-reads 16. */
_Static_assert(sizeof(sigevent_t) == 80, "sigevent current size");

#if CONFIG_POSIX_MQ

/* Open or create a message queue. */
int64_t sys_mq_open(const char *name, int oflag, uint32_t mode, mq_attr_t *attr);

/* Remove a named message queue. */
int64_t sys_mq_unlink(const char *name);

/* Send a message, blocking until abs_timeout. */
int64_t sys_mq_timedsend(int mqdes, const char *msg_ptr, size_t msg_len, uint32_t msg_prio, const void *abs_timeout);

/* Receive a message, blocking until abs_timeout. */
int64_t sys_mq_timedreceive(int mqdes, char *msg_ptr, size_t msg_len, uint32_t *msg_prio, const void *abs_timeout);

/* Register a notification for message arrival. */
int64_t sys_mq_notify(int mqdes, const sigevent_t *notification);

/* Get or set queue attributes. */
int64_t sys_mq_getsetattr(int mqdes, const mq_attr_t *newattr, mq_attr_t *oldattr);

/* Initialize the POSIX MQ subsystem. */
void posix_mq_init(void);

#else
static inline int64_t sys_mq_open(const char *, int, uint32_t, mq_attr_t *)
{
    return -ENOSYS;
}
static inline int64_t sys_mq_unlink(const char *)
{
    return -ENOSYS;
}
static inline int64_t sys_mq_timedsend(int, const char *, size_t, uint32_t, const void *)
{
    return -ENOSYS;
}
static inline int64_t sys_mq_timedreceive(int, char *, size_t, uint32_t *, const void *)
{
    return -ENOSYS;
}
static inline int64_t sys_mq_notify(int, const sigevent_t *)
{
    return -ENOSYS;
}
static inline int64_t sys_mq_getsetattr(int, const mq_attr_t *, mq_attr_t *)
{
    return -ENOSYS;
}
static inline void posix_mq_init(void) {}
#endif

#endif // INCLUDE_POSIX_MQ_H_
