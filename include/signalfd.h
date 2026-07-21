/*
 *
 *      signalfd.h
 *      Signalfd file descriptor header file
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SIGNALFD_H_
#define INCLUDE_SIGNALFD_H_

#include <signal.h>
#include <spin_lock.h>
#include <stdint.h>
#include <task.h>
#include <vfs.h>

#define SFD_CLOEXEC  (1 << 19)
#define SFD_NONBLOCK (1 << 11)

/* Linux signalfd_siginfo structure (128 bytes) */
typedef struct signalfd_siginfo {
        uint32_t ssi_signo;
        int32_t  ssi_errno;
        int32_t  ssi_code;
        uint32_t ssi_pid;
        uint32_t ssi_uid;
        int32_t  ssi_fd;
        uint32_t ssi_tid;
        uint32_t ssi_band;
        uint32_t ssi_overrun;
        uint32_t ssi_trapno;
        int32_t  ssi_status;
        int32_t  ssi_int;
        uint64_t ssi_ptr;
        uint64_t ssi_utime;
        uint64_t ssi_stime;
        uint64_t ssi_addr;
        uint16_t ssi_addr_lsb;
        uint16_t __pad0;
        int32_t  ssi_syscall;
        uint64_t ssi_call_addr;
        uint32_t ssi_arch;
        uint8_t  __pad1[28];
} signalfd_siginfo_t;

#define SIG_PENDING_MAX 16

typedef struct signalfd_ctx {
        sigset_t           sigmask;
        uint64_t           flags;
        spinlock_t         lock;
        wait_queue_t       wq;
        signalfd_siginfo_t pending[SIG_PENDING_MAX];
        int                pending_head;
        int                pending_tail;
        int                pending_count;
} signalfd_ctx_t;

/* Create or update a signalfd */
int sys_signalfd(int fd, const void *mask, int flags);

/* Create or update a signalfd (with flags) */
int sys_signalfd4(int fd, const void *mask, size_t sizemask, int flags);

/* Deliver a signal to all signalfd instances in a process */
void signalfd_deliver(process_t *proc, int sig);

/* Initialize the signalfd subsystem */
void signalfd_init(void);

#endif /* INCLUDE_SIGNALFD_H_ */