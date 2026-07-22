/*
 *
 *      epoll.h
 *      Epoll event notification header file
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EPOLL_H_
#define INCLUDE_EPOLL_H_

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Epoll constants                                                    */
/* ------------------------------------------------------------------ */

#define EPOLLIN        0x001
#define EPOLLPRI       0x002
#define EPOLLOUT       0x004
#define EPOLLERR       0x008
#define EPOLLHUP       0x010
#define EPOLLRDNORM    0x040
#define EPOLLRDBAND    0x080
#define EPOLLWRNORM    0x100
#define EPOLLWRBAND    0x200
#define EPOLLMSG       0x400
#define EPOLLRDHUP     0x2000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP    (1U << 29)
#define EPOLLONESHOT   (1U << 30)
#define EPOLLET        (1U << 31)

#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_DEL  2
#define EPOLL_CTL_MOD  3

#define EPOLL_CLOEXEC  0x80000

#define EPOLL_MAX_EVENTS 256

/* ------------------------------------------------------------------ */
/*  Epoll structures                                                   */
/* ------------------------------------------------------------------ */

typedef union epoll_data {
        void    *ptr;
        int      fd;
        uint32_t u32;
        uint64_t u64;
} epoll_data_t;

typedef struct epoll_event {
        uint32_t     events;
        epoll_data_t data;
} epoll_event_t;

/* ------------------------------------------------------------------ */
/*  Epoll syscall interface                                            */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_create(int size);
int64_t sys_epoll_create1(int flags);
int64_t sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *event);
int64_t sys_epoll_wait(int epfd, epoll_event_t *events, int maxevents, int timeout);
int64_t sys_epoll_pwait(int epfd, epoll_event_t *events, int maxevents, int timeout,
                        const void *sigmask, size_t sigsetsize);

void epoll_init(void);

#endif /* INCLUDE_EPOLL_H_ */