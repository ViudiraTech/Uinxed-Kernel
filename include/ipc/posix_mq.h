/*
 *
 *      posix_mq.h
 *      POSIX Message Queues header file
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_POSIX_MQ_H_
#define INCLUDE_POSIX_MQ_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/signal.h>

/* ------------------------------------------------------------------ */
/*  POSIX MQ attributes                                                */
/* ------------------------------------------------------------------ */

#define MQ_MAXMSG_DEFAULT  10
#define MQ_MSGSIZE_DEFAULT 8192
#define MQ_MAXMSG_MAX      256
#define MQ_MSGSIZE_MAX     65536
#define MQ_PRIO_MAX        32768

#define MQ_NAME_MAX 256

/* ------------------------------------------------------------------ */
/*  mq_attr structure                                                  */
/* ------------------------------------------------------------------ */

typedef struct mq_attr {
        int64_t mq_flags;
        int64_t mq_maxmsg;
        int64_t mq_msgsize;
        int64_t mq_curmsgs;
        int64_t __pad[4];
} mq_attr_t;

/* ------------------------------------------------------------------ */
/*  sigevent structure (for mq_notify)                                  */
/* ------------------------------------------------------------------ */

typedef struct sigevent {
        sigval_t sigev_value;
        int32_t  sigev_signo;
        int32_t  sigev_notify;
        void (*sigev_notify_function)(sigval_t);
        void   *sigev_notify_attributes;
        int32_t __pad[12];
} sigevent_t;

#define SIGEV_NONE   1
#define SIGEV_SIGNAL 2
#define SIGEV_THREAD 3

/* ------------------------------------------------------------------ */
/*  POSIX MQ syscall interface                                         */
/* ------------------------------------------------------------------ */

int64_t sys_mq_open(const char *name, int oflag, uint32_t mode, mq_attr_t *attr);
int64_t sys_mq_unlink(const char *name);
int64_t sys_mq_timedsend(int mqdes, const char *msg_ptr, size_t msg_len, uint32_t msg_prio, const void *abs_timeout);
int64_t sys_mq_timedreceive(int mqdes, char *msg_ptr, size_t msg_len, uint32_t *msg_prio, const void *abs_timeout);
int64_t sys_mq_notify(int mqdes, const sigevent_t *notification);
int64_t sys_mq_getsetattr(int mqdes, const mq_attr_t *newattr, mq_attr_t *oldattr);

void posix_mq_init(void);

#endif /* INCLUDE_POSIX_MQ_H_ */