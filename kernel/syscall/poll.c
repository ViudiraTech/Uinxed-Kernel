/*
 *
 *      poll.c
 *      Linux-compatible poll, select, pselect6, and ppoll syscalls
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <syscall/poll.h>

/*
 * Overview
 * poll.c implements poll(2)/select(2)/ppoll(2). It collects the
 * file descriptors to watch, evaluates each one's readiness through
 * the VFS poll callback, and blocks in the scheduler until an event
 * or the timeout expires.
 */

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

#define POLL_ALWAYS_MASK   (POLLERR | POLLHUP | POLLNVAL)
#define POLL_REQUEST_MASK  (POLLIN | POLLPRI | POLLOUT | POLLRDNORM | POLLRDBAND | POLLWRNORM | POLLWRBAND | POLLMSG | POLLRDHUP)
#define SELECT_READ_MASK   (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP | POLLHUP | POLLERR)
#define SELECT_WRITE_MASK  (POLLOUT | POLLWRNORM | POLLWRBAND | POLLERR)
#define SELECT_EXCEPT_MASK POLLPRI

#define POLL_NFDS_MAX      65536ULL
#define SELECT_NFDS_MAX    1024ULL
#define SELECT_FD_SET_SIZE (SELECT_NFDS_MAX / 8)

typedef struct {
        int32_t fd;
        int16_t events;
        int16_t revents;
} linux_pollfd_t;

typedef struct {
        int64_t tv_sec;
        int64_t tv_nsec;
} linux_timespec_t;

typedef struct {
        int64_t tv_sec;
        int64_t tv_usec;
} linux_timeval_t;

typedef struct {
        uint64_t sigmask;
        uint64_t sigsetsize;
} linux_pselect_sigarg_t;

typedef struct poll_wait_context poll_wait_context_t;

typedef struct {
        process_file_t         *file;
        poll_wait_context_t    *context;
        vfs_poll_source_t      *event_source;
        vfs_poll_subscription_t event_subscription;
        vfs_poll_subscription_t close_subscription;
        bool                    closed;
} poll_watch_t;

typedef struct poll_wait_context {
        wait_queue_t wq;
        uint64_t     generation;
} poll_wait_context_t;

typedef struct {
        bool     infinite;
        uint64_t duration_ns;
        uint64_t start_tick;
        uint64_t deadline_tick;
} poll_timeout_t;

typedef struct {
        signal_state_t *state;
        sigset_t        old_mask;
        bool            active;
} poll_sigmask_guard_t;

static bool fdset_test(const uint8_t *set, uint64_t fd)
{
    return (set[fd / 8] & (uint8_t)(1U << (fd % 8))) != 0;
}

static void fdset_set(uint8_t *set, uint64_t fd)
{
    set[fd / 8] |= (uint8_t)(1U << (fd % 8));
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t saturating_mul_u64(uint64_t left, uint64_t right)
{
    if (left && right > UINT64_MAX / left) return UINT64_MAX;
    return left * right;
}

static void poll_timeout_init(poll_timeout_t *timeout, bool infinite, uint64_t duration_ns)
{
    timeout->infinite      = infinite;
    timeout->duration_ns   = duration_ns;
    timeout->start_tick    = sched_ticks();
    timeout->deadline_tick = infinite ? 0 : saturating_add_u64(timeout->start_tick, timer_ns_to_ticks_ceil(duration_ns));
}

static uint64_t poll_timeout_remaining_ns(const poll_timeout_t *timeout)
{
    if (timeout->infinite) return 0;
    uint64_t now     = sched_ticks();
    uint64_t elapsed = now >= timeout->start_tick ? now - timeout->start_tick : 0;
    uint64_t used_ns = saturating_mul_u64(elapsed, TIMER_TICK_NS);
    return used_ns < timeout->duration_ns ? timeout->duration_ns - used_ns : 0;
}

static bool poll_signal_pending(process_t *proc)
{
    bool pending;
    spin_lock(&proc->signal.lock);
    pending = signal_has_pending(&proc->signal) != 0;
    spin_unlock(&proc->signal.lock);
    return pending;
}

static void poll_event_notify(vfs_poll_subscription_t *subscription, uint32_t events)
{
    (void)events;
    poll_watch_t *watch = subscription->context;
    __atomic_add_fetch(&watch->context->generation, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(&watch->context->wq);
}

static void poll_close_notify(vfs_poll_subscription_t *subscription, uint32_t events)
{
    (void)events;
    poll_watch_t *watch = subscription->context;
    __atomic_store_n(&watch->closed, true, __ATOMIC_RELEASE);
    vfs_poll_source_unsubscribe(watch->event_source, &watch->event_subscription);
    __atomic_add_fetch(&watch->context->generation, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(&watch->context->wq);
}

static void poll_watches_release(poll_watch_t *watches, uint64_t nfds)
{
    if (!watches) return;
    for (uint64_t i = 0; i < nfds; i++) {
        poll_watch_t *watch = &watches[i];
        if (!watch->file) continue;
        vfs_poll_source_unsubscribe(watch->event_source, &watch->event_subscription);
        vfs_poll_source_unsubscribe(&watch->file->close_source, &watch->close_subscription);
        process_file_put(watch->file);
    }
    free(watches);
}

static int poll_watches_create(process_t *proc, linux_pollfd_t *fds, uint64_t nfds, bool invalid_is_error, poll_wait_context_t *context,
                               poll_watch_t **result)
{
    poll_watch_t *watches = nfds ? calloc((size_t)nfds, sizeof(*watches)) : NULL;
    if (nfds && !watches) {
        plogk("poll: Watch array alloc failed (nfds=%lu)\n", (unsigned long)nfds);
        return -ENOMEM;
    }

    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;

        process_file_t *file = process_fd_get(proc, fds[i].fd);
        if (!file) {
            if (invalid_is_error) {
                poll_watches_release(watches, nfds);
                return -EBADF;
            }
            continue;
        }

        poll_watch_t *watch = &watches[i];
        watch->file         = file;
        watch->context      = context;
        watch->event_source = vfs_file_poll_source(file->node, file->private_data);
        uint32_t events     = ((uint16_t)fds[i].events & POLL_REQUEST_MASK) | POLL_ALWAYS_MASK;
        vfs_poll_source_subscribe(watch->event_source, &watch->event_subscription, events, poll_event_notify, watch);
        vfs_poll_source_subscribe(&file->close_source, &watch->close_subscription, UINT32_MAX, poll_close_notify, watch);
    }

    *result = watches;
    return EOK;
}

static int poll_scan(linux_pollfd_t *fds, poll_watch_t *watches, uint64_t nfds)
{
    int ready = 0;

    for (uint64_t i = 0; i < nfds; i++) {
        linux_pollfd_t *pfd   = &fds[i];
        poll_watch_t   *watch = &watches[i];
        pfd->revents          = 0;
        if (pfd->fd < 0) continue;

        if (!watch->file || __atomic_load_n(&watch->closed, __ATOMIC_ACQUIRE)) {
            pfd->revents = POLLNVAL;
        } else {
            uint32_t requested = (uint16_t)pfd->events & POLL_REQUEST_MASK;
            int      result    = process_file_poll(watch->file, requested | POLLERR | POLLHUP);
            if (result < 0)
                pfd->revents = POLLERR;
            else
                pfd->revents = (int16_t)((uint32_t)result & (requested | POLL_ALWAYS_MASK));
        }
        if (pfd->revents) ready++;
    }
    return ready;
}

static int poll_wait(process_t *proc, linux_pollfd_t *fds, uint64_t nfds, poll_timeout_t *timeout, bool invalid_is_error)
{
    poll_wait_context_t context;
    memset(&context, 0, sizeof(context));
    wait_queue_init(&context.wq);

    poll_watch_t *watches = NULL;
    int           ret     = poll_watches_create(proc, fds, nfds, invalid_is_error, &context, &watches);
    if (ret != EOK) return ret;

    for (;;) {
        uint64_t generation = __atomic_load_n(&context.generation, __ATOMIC_ACQUIRE);
        ret                 = poll_scan(fds, watches, nfds);
        if (ret > 0) break;
        if (!timeout->infinite && sched_ticks() >= timeout->deadline_tick) {
            ret = 0;
            break;
        }
        if (poll_signal_pending(proc)) {
            ret = -EINTR;
            break;
        }

        wait_queue_prepare(&context.wq);
        if (__atomic_load_n(&context.generation, __ATOMIC_ACQUIRE) != generation || poll_signal_pending(proc)
            || (!timeout->infinite && sched_ticks() >= timeout->deadline_tick)) {
            wait_queue_wake_all(&context.wq);
        }

        if (timeout->infinite)
            wait_queue_sleep();
        else
            (void)wait_queue_wait_timed(&context.wq, timeout->deadline_tick);
    }

    poll_watches_release(watches, nfds);
    return ret;
}

static int poll_sigmask_install(process_t *proc, const sigset_t *new_mask, poll_sigmask_guard_t *guard)
{
    memset(guard, 0, sizeof(*guard));
    if (!new_mask) return EOK;

    sigset_t mask = *new_mask;
    sigdelset(&mask, SIGKILL);
    sigdelset(&mask, SIGSTOP);

    guard->state = &proc->signal;
    spin_lock(&guard->state->lock);
    guard->old_mask       = guard->state->blocked;
    guard->state->blocked = mask;
    guard->active         = true;
    spin_unlock(&guard->state->lock);
    return EOK;
}

static void poll_sigmask_finish(poll_sigmask_guard_t *guard, bool interrupted)
{
    if (!guard->active) return;
    spin_lock(&guard->state->lock);
    if (interrupted && signal_has_pending(guard->state)) {
        guard->state->saved_mask   = guard->old_mask;
        guard->state->restore_mask = true;
    } else {
        guard->state->blocked = guard->old_mask;
    }
    spin_unlock(&guard->state->lock);
}

static int copy_sigmask(uint64_t address, uint64_t size, sigset_t *mask)
{
    if (!address) return EOK;
    if (size != sizeof(sigset_t)) return -EINVAL;
    return copy_from_user(mask, (const void *)address, sizeof(*mask)) ? -EFAULT : EOK;
}

static int64_t do_poll(uint64_t user_fds, uint64_t nfds, poll_timeout_t *timeout, const sigset_t *mask)
{
    process_t *proc = process_current();
    if (!proc) {
        plogk("poll: No current process for poll.\n");
        return -ESRCH;
    }
    if (nfds > POLL_NFDS_MAX) {
        plogk("poll: nfds %lu exceeds limit %llu\n", (unsigned long)nfds, POLL_NFDS_MAX);
        return -EINVAL;
    }
    if (nfds && !user_fds) {
        plogk("poll: nfds %lu with null fds pointer.\n", (unsigned long)nfds);
        return -EFAULT;
    }

    linux_pollfd_t *fds = nfds ? calloc((size_t)nfds, sizeof(*fds)) : NULL;
    if (nfds && !fds) {
        plogk("poll: fd array alloc failed (nfds=%lu)\n", (unsigned long)nfds);
        return -ENOMEM;
    }
    if (nfds && copy_from_user(fds, (const void *)user_fds, (size_t)nfds * sizeof(*fds))) {
        free(fds);
        plogk("poll: Copy of fd array from user failed (nfds=%lu, fds=%p)\n", (unsigned long)nfds, (const void *)user_fds);
        return -EFAULT;
    }

    poll_sigmask_guard_t guard;
    poll_sigmask_install(proc, mask, &guard);
    int  ret         = poll_wait(proc, fds, nfds, timeout, false);
    bool interrupted = ret == -EINTR;

    if (ret >= 0 && nfds && copy_to_user((void *)user_fds, fds, (size_t)nfds * sizeof(*fds))) ret = -EFAULT;
    free(fds);
    poll_sigmask_finish(&guard, interrupted);
    return ret;
}

static int64_t do_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, poll_timeout_t *timeout, const sigset_t *mask)
{
    process_t *proc = process_current();
    if (!proc) {
        plogk("poll: Select with no current process.\n");
        return -ESRCH;
    }
    if (nfds > SELECT_NFDS_MAX) {
        plogk("poll: Select nfds %lu exceeds limit %llu\n", (unsigned long)nfds, SELECT_NFDS_MAX);
        return -EINVAL;
    }

    size_t  set_size                       = (size_t)(((nfds + 63) / 64) * sizeof(uint64_t));
    uint8_t in_read[SELECT_FD_SET_SIZE]    = {0};
    uint8_t in_write[SELECT_FD_SET_SIZE]   = {0};
    uint8_t in_except[SELECT_FD_SET_SIZE]  = {0};
    uint8_t out_read[SELECT_FD_SET_SIZE]   = {0};
    uint8_t out_write[SELECT_FD_SET_SIZE]  = {0};
    uint8_t out_except[SELECT_FD_SET_SIZE] = {0};

    if ((readfds && copy_from_user(in_read, (const void *)readfds, set_size))
        || (writefds && copy_from_user(in_write, (const void *)writefds, set_size))
        || (exceptfds && copy_from_user(in_except, (const void *)exceptfds, set_size))) {
        plogk("poll: Select copy of fd sets from user failed (nfds=%lu)\n", (unsigned long)nfds);
        return -EFAULT;
    }

    linux_pollfd_t *fds = nfds ? calloc((size_t)nfds, sizeof(*fds)) : NULL;
    if (nfds && !fds) {
        plogk("poll: Select fd array alloc failed (nfds=%lu)\n", (unsigned long)nfds);
        return -ENOMEM;
    }
    for (uint64_t fd = 0; fd < nfds; fd++) {
        uint16_t events = 0;
        if (readfds && fdset_test(in_read, fd)) events |= POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP;
        if (writefds && fdset_test(in_write, fd)) events |= POLLOUT | POLLWRNORM | POLLWRBAND;
        if (exceptfds && fdset_test(in_except, fd)) events |= POLLPRI;
        fds[fd].fd     = events ? (int32_t)fd : -1;
        fds[fd].events = (int16_t)events;
    }

    poll_sigmask_guard_t guard;
    poll_sigmask_install(proc, mask, &guard);
    int  ret         = poll_wait(proc, fds, nfds, timeout, true);
    bool interrupted = ret == -EINTR;

    if (ret >= 0) {
        int bit_count = 0;
        for (uint64_t fd = 0; fd < nfds; fd++) {
            uint16_t revents = (uint16_t)fds[fd].revents;
            if (readfds && fdset_test(in_read, fd) && (revents & SELECT_READ_MASK)) {
                fdset_set(out_read, fd);
                bit_count++;
            }
            if (writefds && fdset_test(in_write, fd) && (revents & SELECT_WRITE_MASK)) {
                fdset_set(out_write, fd);
                bit_count++;
            }
            if (exceptfds && fdset_test(in_except, fd) && (revents & SELECT_EXCEPT_MASK)) {
                fdset_set(out_except, fd);
                bit_count++;
            }
        }
        ret = bit_count;
        if ((readfds && copy_to_user((void *)readfds, out_read, set_size)) || (writefds && copy_to_user((void *)writefds, out_write, set_size))
            || (exceptfds && copy_to_user((void *)exceptfds, out_except, set_size)))
            ret = -EFAULT;
    }

    free(fds);
    poll_sigmask_finish(&guard, interrupted);
    return ret;
}

int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout_ms, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    int32_t        timeout_value = (int32_t)timeout_ms;
    poll_timeout_t timeout;
    poll_timeout_init(&timeout, timeout_value < 0, timeout_value > 0 ? (uint64_t)timeout_value * 1000000ULL : 0);
    return do_poll(fds, nfds, &timeout, NULL);
}

int64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout_ptr, uint64_t arg5)
{
    (void)arg5;
    linux_timeval_t tv = {0};
    if (timeout_ptr) {
        if (copy_from_user(&tv, (const void *)timeout_ptr, sizeof(tv))) {
            plogk("poll: Select timeout copy from user failed (ptr=%p)\n", (const void *)timeout_ptr);
            return -EFAULT;
        }
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
            plogk("poll: Select invalid timeout (sec=%ld, usec=%ld)\n", (long)tv.tv_sec, (long)tv.tv_usec);
            return -EINVAL;
        }
    }
    uint64_t duration
        = timeout_ptr ? saturating_add_u64(saturating_mul_u64((uint64_t)tv.tv_sec, 1000000000ULL), (uint64_t)tv.tv_usec * 1000ULL) : 0;
    poll_timeout_t timeout;
    poll_timeout_init(&timeout, timeout_ptr == 0, duration);
    int64_t ret = do_select(nfds, readfds, writefds, exceptfds, &timeout, NULL);

    if (timeout_ptr) {
        uint64_t remaining = poll_timeout_remaining_ns(&timeout);
        tv.tv_sec          = (int64_t)(remaining / 1000000000ULL);
        tv.tv_usec         = (int64_t)((remaining % 1000000000ULL) / 1000ULL);
        if (copy_to_user((void *)timeout_ptr, &tv, sizeof(tv))) return -EFAULT;
    }
    return ret;
}

int64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout_ptr, uint64_t sigarg_ptr)
{
    linux_timespec_t ts = {0};
    if (timeout_ptr) {
        if (copy_from_user(&ts, (const void *)timeout_ptr, sizeof(ts))) {
            plogk("poll: pselect6 timeout copy from user failed (ptr=%p)\n", (const void *)timeout_ptr);
            return -EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) {
            plogk("poll: pselect6 invalid timeout (sec=%ld, nsec=%ld)\n", (long)ts.tv_sec, (long)ts.tv_nsec);
            return -EINVAL;
        }
    }

    sigset_t  mask;
    sigset_t *mask_ptr = NULL;
    if (sigarg_ptr) {
        linux_pselect_sigarg_t arg;
        if (copy_from_user(&arg, (const void *)sigarg_ptr, sizeof(arg))) {
            plogk("poll: pselect6 sigarg copy from user failed (ptr=%p)\n", (const void *)sigarg_ptr);
            return -EFAULT;
        }
        int err = copy_sigmask(arg.sigmask, arg.sigsetsize, &mask);
        if (err != EOK) {
            plogk("poll: pselect6 sigmask error (err=%d, sigsetsize=%lu)\n", err, (unsigned long)arg.sigsetsize);
            return err;
        }
        if (arg.sigmask) mask_ptr = &mask;
    }

    uint64_t       duration = timeout_ptr ? saturating_add_u64(saturating_mul_u64((uint64_t)ts.tv_sec, 1000000000ULL), (uint64_t)ts.tv_nsec) : 0;
    poll_timeout_t timeout;
    poll_timeout_init(&timeout, timeout_ptr == 0, duration);
    int64_t ret = do_select(nfds, readfds, writefds, exceptfds, &timeout, mask_ptr);

    if (timeout_ptr) {
        uint64_t remaining = poll_timeout_remaining_ns(&timeout);
        ts.tv_sec          = (int64_t)(remaining / 1000000000ULL);
        ts.tv_nsec         = (int64_t)(remaining % 1000000000ULL);
        if (copy_to_user((void *)timeout_ptr, &ts, sizeof(ts))) return -EFAULT;
    }
    return ret;
}

int64_t sys_ppoll(uint64_t fds, uint64_t nfds, uint64_t timeout_ptr, uint64_t sigmask_ptr, uint64_t sigsetsize, uint64_t arg5)
{
    (void)arg5;
    linux_timespec_t ts = {0};
    if (timeout_ptr) {
        if (copy_from_user(&ts, (const void *)timeout_ptr, sizeof(ts))) {
            plogk("poll: ppoll timeout copy from user failed (ptr=%p)\n", (const void *)timeout_ptr);
            return -EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) {
            plogk("poll: ppoll invalid timeout (sec=%ld, nsec=%ld)\n", (long)ts.tv_sec, (long)ts.tv_nsec);
            return -EINVAL;
        }
    }

    sigset_t  mask;
    sigset_t *mask_ptr = NULL;
    int       err      = copy_sigmask(sigmask_ptr, sigsetsize, &mask);
    if (err != EOK) {
        plogk("poll: ppoll sigmask error (err=%d, sigsetsize=%lu)\n", err, (unsigned long)sigsetsize);
        return err;
    }
    if (sigmask_ptr) mask_ptr = &mask;

    uint64_t       duration = timeout_ptr ? saturating_add_u64(saturating_mul_u64((uint64_t)ts.tv_sec, 1000000000ULL), (uint64_t)ts.tv_nsec) : 0;
    poll_timeout_t timeout;
    poll_timeout_init(&timeout, timeout_ptr == 0, duration);
    int64_t ret = do_poll(fds, nfds, &timeout, mask_ptr);

    if (timeout_ptr) {
        uint64_t remaining = poll_timeout_remaining_ns(&timeout);
        ts.tv_sec          = (int64_t)(remaining / 1000000000ULL);
        ts.tv_nsec         = (int64_t)(remaining % 1000000000ULL);
        if (copy_to_user((void *)timeout_ptr, &ts, sizeof(ts))) return -EFAULT;
    }
    return ret;
}
