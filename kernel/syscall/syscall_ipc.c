/*
 *
 *      syscall_ipc.c
 *      Pipes, sockets, System V IPC, POSIX message queues, futex, epoll and event fds
 *
 *      2026/11/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/inotify.h>
#include <ipc/epoll.h>
#include <ipc/eventfd.h>
#include <ipc/futex.h>
#include <ipc/pipe.h>
#include <ipc/posix_mq.h>
#include <ipc/sysv_ipc.h>
#include <kernel/abi.h>
#include <kernel/timer/timerfd.h>
#include <libs/std/errno.h>
#include <libs/std/stdint.h>
#include <net/socket.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <sync/signalfd.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>

/* eventfd, timerfd, signalfd wrappers */
int64_t sys_eventfd_wrap(uint64_t initval, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_eventfd((unsigned int)initval, 0);
}

/* System call handler for `eventfd2`. */
int64_t sys_eventfd2_wrap(uint64_t initval, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_eventfd2((unsigned int)initval, (int)flags);
}

/* System call handler for `timerfd_create`. */
int64_t sys_timerfd_create_wrap(uint64_t clockid, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_timerfd_create((int)clockid, (int)flags);
}

/* System call handler for `timerfd_settime`. */
int64_t sys_timerfd_settime_wrap(uint64_t fd, uint64_t flags, uint64_t new_value, uint64_t old_value, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_timerfd_settime((int)fd, (int)flags, (const void *)new_value, (void *)old_value);
}

/* System call handler for `timerfd_gettime`. */
int64_t sys_timerfd_gettime_wrap(uint64_t fd, uint64_t curr_value, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_timerfd_gettime((int)fd, (void *)curr_value);
}

/* System call handler for `signalfd`. */
int64_t sys_signalfd_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_signalfd4((int)fd, (const void *)mask, (size_t)sizemask, 0);
}

/* System call handler for `signalfd4`. */
int64_t sys_signalfd4_wrap(uint64_t fd, uint64_t mask, uint64_t sizemask, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_signalfd4((int)fd, (const void *)mask, (size_t)sizemask, (int)flags);
}

/* System call handler for `inotify_init`. */
int64_t sys_inotify_init_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_init();
}

/* System call handler for `inotify_init1`. */
int64_t sys_inotify_init1_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_init1((int)flags);
}

/* System call handler for `inotify_add_watch`. */
int64_t sys_inotify_add_watch_wrap(uint64_t fd, uint64_t pathname, uint64_t mask, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_add_watch((int)fd, (const char *)pathname, (uint32_t)mask);
}

/* System call handler for `inotify_rm_watch`. */
int64_t sys_inotify_rm_watch_wrap(uint64_t fd, uint64_t wd, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_inotify_rm_watch((int)fd, (int)wd);
}

/* epoll_pwait2 syscall: wait with a nanosecond timeout */
int64_t sys_epoll_pwait2_impl(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t tsp, uint64_t sigmask, uint64_t sigsetsize)
{
    int timeout_ms = -1; // infinite

    if (tsp) {
        linux_timespec_t ts;
        if (copy_from_user(&ts, (const void *)tsp, sizeof(ts))) return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || (uint64_t)ts.tv_nsec >= 1000000000ULL) return -EINVAL;

        /* Convert to milliseconds, rounding up */
        int64_t ms = (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
        if (ts.tv_nsec % 1000000) ms++;
        if (ms > (int64_t)INT32_MAX) ms = INT32_MAX;
        timeout_ms = (int)ms;
    }

    return sys_epoll_pwait((int)epfd, (epoll_event_t *)events, (int)maxevents, timeout_ms, (const void *)sigmask, (size_t)sigsetsize);
}

/* Socket wrappers */
int64_t sys_socket_wrap(uint64_t family, uint64_t type, uint64_t protocol, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_socket((uint32_t)family, (uint32_t)type, (uint32_t)protocol);
}

/* System call handler for `bind`. */
int64_t sys_bind_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_bind((int)fd, (const sockaddr_t *)addr, (uint32_t)addrlen);
}

/* System call handler for `listen`. */
int64_t sys_listen_wrap(uint64_t fd, uint64_t backlog, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_listen((int)fd, (int)backlog);
}

/* System call handler for `accept`. */
int64_t sys_accept_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_accept((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen, 0);
}

/* System call handler for `accept4`. */
int64_t sys_accept4_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_accept((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen, (int)flags);
}

/* System call handler for `connect`. */
int64_t sys_connect_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_connect((int)fd, (const sockaddr_t *)addr, (uint32_t)addrlen);
}

/* System call handler for `sendto`. */
int64_t sys_sendto_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    return sys_sendto((int)fd, (const void *)buf, (size_t)len, (int)flags, (const sockaddr_t *)addr, (uint32_t)addrlen);
}

/* System call handler for `recvfrom`. */
int64_t sys_recvfrom_wrap(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    return sys_recvfrom((int)fd, (void *)buf, (size_t)len, (int)flags, (sockaddr_t *)addr, (uint32_t *)addrlen);
}

/* System call handler for `sendmsg`. */
int64_t sys_sendmsg_wrap(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_sendmsg((int)fd, (const msghdr_t *)msg, (int)flags);
}

/* System call handler for `recvmsg`. */
int64_t sys_recvmsg_wrap(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_recvmsg((int)fd, (msghdr_t *)msg, (int)flags);
}

/* System call handler for `shutdown`. */
int64_t sys_shutdown_wrap(uint64_t fd, uint64_t how, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shutdown((int)fd, (int)how);
}

/* System call handler for `socketpair`. */
int64_t sys_socketpair_wrap(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_socketpair((int)domain, (int)type, (int)protocol, (int *)sv);
}

/* System call handler for `getsockname`. */
int64_t sys_getsockname_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getsockname((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen);
}

/* System call handler for `getpeername`. */
int64_t sys_getpeername_wrap(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_getpeername((int)fd, (sockaddr_t *)addr, (uint32_t *)addrlen);
}

/* System call handler for `setsockopt`. */
int64_t sys_setsockopt_wrap(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen, uint64_t arg5)
{
    (void)arg5;
    return sys_setsockopt((int)fd, (int)level, (int)optname, (const void *)optval, (uint32_t)optlen);
}

/* System call handler for `getsockopt`. */
int64_t sys_getsockopt_wrap(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen, uint64_t arg5)
{
    (void)arg5;
    return sys_getsockopt((int)fd, (int)level, (int)optname, (void *)optval, (uint32_t *)optlen);
}

/* System call handler for `sendmmsg`. */
int64_t sys_sendmmsg_wrap(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_sendmmsg((int)fd, (void *)msgvec, (uint32_t)vlen, (int)flags);
}

/* System call handler for `recvmmsg`. */
int64_t sys_recvmmsg_wrap(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags, uint64_t timeout, uint64_t arg5)
{
    (void)arg5;
    return sys_recvmmsg((int)fd, (void *)msgvec, (uint32_t)vlen, (int)flags, (void *)timeout);
}

/* Pipe wrappers */
int64_t sys_pipe_wrap(uint64_t pipefd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_pipe((int *)pipefd);
}

/* System call handler for `pipe2`. */
int64_t sys_pipe2_wrap(uint64_t pipefd, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_pipe2((int *)pipefd, (int)flags);
}

/* System V IPC wrappers */
int64_t sys_semget_wrap(uint64_t key, uint64_t nsems, uint64_t semflg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_semget((key_t)key, (int)nsems, (int)semflg);
}

/* System call handler for `semop`. */
int64_t sys_semop_wrap(uint64_t semid, uint64_t sops, uint64_t nsops, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_semop((int)semid, (sembuf_t *)sops, (size_t)nsops);
}

/* System call handler for `semtimedop`. */
int64_t sys_semtimedop_wrap(uint64_t semid, uint64_t sops, uint64_t nsops, uint64_t timeout, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_semtimedop((int)semid, (sembuf_t *)sops, (size_t)nsops, (const void *)timeout);
}

/* System call handler for `semctl`. */
int64_t sys_semctl_wrap(uint64_t semid, uint64_t semnum, uint64_t cmd, uint64_t arg, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_semctl((int)semid, (int)semnum, (int)cmd, arg);
}

/* System call handler for `shmget`. */
int64_t sys_shmget_wrap(uint64_t key, uint64_t size, uint64_t shmflg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmget((key_t)key, (size_t)size, (int)shmflg);
}

/* System call handler for `shmat`. */
int64_t sys_shmat_wrap(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmat((int)shmid, (const void *)shmaddr, (int)shmflg);
}

/* System call handler for `shmdt`. */
int64_t sys_shmdt_wrap(uint64_t shmaddr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmdt((const void *)shmaddr);
}

/* System call handler for `shmctl`. */
int64_t sys_shmctl_wrap(uint64_t shmid, uint64_t cmd, uint64_t buf, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_shmctl((int)shmid, (int)cmd, (void *)buf);
}

/* System call handler for `msgget`. */
int64_t sys_msgget_wrap(uint64_t key, uint64_t msgflg, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_msgget((key_t)key, (int)msgflg);
}

/* System call handler for `msgsnd`. */
int64_t sys_msgsnd_wrap(uint64_t msqid, uint64_t msgp, uint64_t msgsz, uint64_t msgflg, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_msgsnd((int)msqid, (const void *)msgp, (size_t)msgsz, (int)msgflg);
}

/* System call handler for `msgrcv`. */
int64_t sys_msgrcv_wrap(uint64_t msqid, uint64_t msgp, uint64_t msgsz, uint64_t msgtyp, uint64_t msgflg, uint64_t arg5)
{
    (void)arg5;
    return sys_msgrcv((int)msqid, (void *)msgp, (size_t)msgsz, (int64_t)msgtyp, (int)msgflg);
}

/* System call handler for `msgctl`. */
int64_t sys_msgctl_wrap(uint64_t msqid, uint64_t cmd, uint64_t buf, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_msgctl((int)msqid, (int)cmd, (void *)buf);
}

/* POSIX MQ wrappers */
int64_t sys_mq_open_wrap(uint64_t name, uint64_t oflag, uint64_t mode, uint64_t attr, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_mq_open((const char *)name, (int)oflag, (uint32_t)mode, (mq_attr_t *)attr);
}

/* System call handler for `mq_unlink`. */
int64_t sys_mq_unlink_wrap(uint64_t name, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mq_unlink((const char *)name);
}

/* System call handler for `mq_timedsend`. */
int64_t sys_mq_timedsend_wrap(uint64_t mqdes, uint64_t msg_ptr, uint64_t msg_len, uint64_t msg_prio, uint64_t abs_timeout, uint64_t arg5)
{
    (void)arg5;
    return sys_mq_timedsend((int)mqdes, (const char *)msg_ptr, (size_t)msg_len, (uint32_t)msg_prio, (const void *)abs_timeout);
}

/* System call handler for `mq_timedreceive`. */
int64_t sys_mq_timedreceive_wrap(uint64_t mqdes, uint64_t msg_ptr, uint64_t msg_len, uint64_t msg_prio, uint64_t abs_timeout, uint64_t arg5)
{
    (void)arg5;
    return sys_mq_timedreceive((int)mqdes, (char *)msg_ptr, (size_t)msg_len, (uint32_t *)msg_prio, (const void *)abs_timeout);
}

/* System call handler for `mq_notify`. */
int64_t sys_mq_notify_wrap(uint64_t mqdes, uint64_t notification, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mq_notify((int)mqdes, (const sigevent_t *)notification);
}

/* System call handler for `mq_getsetattr`. */
int64_t sys_mq_getsetattr_wrap(uint64_t mqdes, uint64_t newattr, uint64_t oldattr, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mq_getsetattr((int)mqdes, (const mq_attr_t *)newattr, (mq_attr_t *)oldattr);
}

/* Futex wrapper */
int64_t sys_futex_wrap(uint64_t uaddr, uint64_t futex_op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint64_t val3)
{
    return sys_futex((uint32_t *)uaddr, (int)futex_op, (uint32_t)val, timeout, (uint32_t *)uaddr2, (uint32_t)val3);
}

/* Epoll wrappers */
int64_t sys_epoll_create_wrap(uint64_t size, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_epoll_create((int)size);
}

/* System call handler for `epoll_create1`. */
int64_t sys_epoll_create1_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_epoll_create1((int)flags);
}

/* System call handler for `epoll_ctl`. */
int64_t sys_epoll_ctl_wrap(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_epoll_ctl((int)epfd, (int)op, (int)fd, (epoll_event_t *)event);
}

/* System call handler for `epoll_wait`. */
int64_t sys_epoll_wait_wrap(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;
    return sys_epoll_wait((int)epfd, (epoll_event_t *)events, (int)maxevents, (int)timeout);
}

/* System call handler for `epoll_pwait`. */
int64_t sys_epoll_pwait_wrap(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize)
{
    return sys_epoll_pwait((int)epfd, (epoll_event_t *)events, (int)maxevents, (int)timeout, (const void *)sigmask, (size_t)sigsetsize);
}
