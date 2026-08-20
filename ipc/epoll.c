/*
 *
 *      epoll.c
 *      Epoll event notification implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <ipc/epoll.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/poll.h>
#include <syscall/syscall.h>

/* Constants */

#ifndef EPOLL_MAX_FDS
#    define EPOLL_MAX_FDS 1024
#endif
#define EPOLL_TICKS_PER_SEC TIMER_HZ

/* Internal structures */

typedef struct epoll_instance epoll_instance_t;

typedef struct epoll_item {
        int                     fd;
        uint32_t                events;
        uint32_t                revents;
        epoll_data_t            data;
        epoll_instance_t       *epi;
        int                     active;
        uint32_t                last_revents;     // previous poll result for edge-triggered
        int                     oneshot_disabled; // EPOLLONESHOT re-arm flag
        process_file_t         *file;
        vfs_poll_source_t      *event_source;
        vfs_poll_subscription_t subscription;
        vfs_poll_subscription_t close_subscription;
        uint32_t                pending_events;
        bool                    target_closed;
} epoll_item_t;

typedef struct epoll_instance {
        epoll_item_t    *items[EPOLL_MAX_FDS];
        int              fd_count;
        int              max_fd;
        wait_queue_t     wq;
        spinlock_t       lock;
        struct vfs_node *node;
        uint32_t         refcount;
        uint64_t         event_generation;
} epoll_instance_t;

/* Static filesystem ID */

static int epoll_fsid = -1;

/*
 * Map a process_fd_poll result (which returns POLLIN/POLLOUT/POLLERR/POLLHUP)
 * to the corresponding EPOLL bits.  Only bits that are set in both the
 * requested mask (events) and the actual poll result are returned.
 */
static uint32_t epoll_map_poll_result(int poll_result, uint32_t requested)
{
    uint32_t revents = 0;

    if (poll_result & POLLIN) revents |= EPOLLIN;
    if (poll_result & POLLOUT) revents |= EPOLLOUT;
    if (poll_result & POLLERR) revents |= EPOLLERR;
    if (poll_result & POLLHUP) revents |= EPOLLHUP;

    /* Always report EPOLLERR and EPOLLHUP regardless of request */
    revents |= (revents & (EPOLLERR | EPOLLHUP));

    /* Mask with requested events (plus error/hup which are always reported) */
    return revents & (requested | EPOLLERR | EPOLLHUP);
}

/* Notify the epoll instance that a watched fd became ready. */
static void epoll_item_notify(vfs_poll_subscription_t *subscription, uint32_t events)
{
    epoll_item_t *item = subscription->context;
    if (__atomic_load_n(&item->active, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_or(&item->pending_events, events, __ATOMIC_RELEASE);
        __atomic_add_fetch(&item->epi->event_generation, 1, __ATOMIC_RELEASE);
        wait_queue_wake_all(&item->epi->wq);
        /*
         * An epoll fd is itself pollable.  libinput exposes its internal
         * epoll fd to Xorg, which then watches it from Xorg's outer epoll.
         * Propagate target readiness to that outer poll source as well as to
         * threads directly blocked in epoll_wait() on this instance.
         */
        if (item->epi->node) vfs_poll_notify(item->epi->node, POLLIN);
    }
}

/* Handle closure of a watched fd by publishing EPOLLHUP. */
static void epoll_target_close(vfs_poll_subscription_t *subscription, uint32_t events)
{
    (void)events;
    epoll_item_t *item = subscription->context;
    __atomic_store_n(&item->target_closed, true, __ATOMIC_RELEASE);
    __atomic_fetch_or(&item->pending_events, EPOLLHUP, __ATOMIC_RELEASE);
    vfs_poll_source_unsubscribe(item->event_source, &item->subscription);
    __atomic_add_fetch(&item->epi->event_generation, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(&item->epi->wq);
    if (item->epi->node) vfs_poll_notify(item->epi->node, POLLIN);
}

/* Find an epoll_item by fd.  Must be called with epi->lock held. */
static epoll_item_t *epoll_item_find(epoll_instance_t *epi, int fd)
{
    if (fd < 0 || fd >= EPOLL_MAX_FDS) return NULL;
    return epi->items[fd];
}

/*
 * Add a new fd to the epoll set.  Must be called with epi->lock held.
 * Returns NULL on error (fd already present, or OOM).
 */
static epoll_item_t *epoll_item_add(epoll_instance_t *epi, int fd, process_file_t *file, const epoll_event_t *event)
{
    if (fd < 0 || fd >= EPOLL_MAX_FDS) return NULL;
    if (epi->items[fd]) return NULL; // already present

    epoll_item_t *item = malloc(sizeof(epoll_item_t));
    if (!item) {
        plogk("epoll: Item allocation failed (fd %d)\n", fd);
        return NULL;
    }
    memset(item, 0, sizeof(epoll_item_t));

    item->fd               = fd;
    item->events           = event->events;
    item->revents          = 0;
    item->data             = event->data;
    item->epi              = epi;
    item->active           = 1;
    item->last_revents     = 0;
    item->oneshot_disabled = 0;
    item->file             = file;

    epi->items[fd] = item;
    epi->fd_count++;
    if (fd > epi->max_fd) epi->max_fd = fd;

    return item;
}

/*
 * Delete an fd from the epoll set.  Must be called with epi->lock held.
 * Returns 0 on success, -ENOENT if not found.
 */
static epoll_item_t *epoll_item_del(epoll_instance_t *epi, int fd)
{
    if (fd < 0 || fd >= EPOLL_MAX_FDS) return NULL;

    epoll_item_t *item = epi->items[fd];
    if (!item) return NULL;

    epi->items[fd] = NULL;
    epi->fd_count--;
    if (fd == epi->max_fd)
        while (epi->max_fd >= 0 && !epi->items[epi->max_fd]) epi->max_fd--;
    __atomic_store_n(&item->active, 0, __ATOMIC_RELEASE);
    return item;
}

/*
 * Modify an existing fd registration.  Must be called with epi->lock held.
 * Returns 0 on success, -ENOENT if not found.
 */
static int epoll_item_mod(epoll_instance_t *epi, int fd, const epoll_event_t *event)
{
    epoll_item_t *item = epoll_item_find(epi, fd);
    if (!item) return -ENOENT;

    item->events           = event->events;
    item->data             = event->data;
    item->oneshot_disabled = 0; // re-arm after EPOLL_CTL_MOD

    return EOK;
}

/* Polling: check all registered fds for readiness */

/*
 * Poll all registered fds and update their revents.
 * Must be called with epi->lock held.
 * Returns the number of ready fds.
 */
static int epoll_poll_all(epoll_instance_t *epi)
{
    int ready = 0;

    for (int fd = 0; fd <= epi->max_fd; fd++) {
        epoll_item_t *item = epi->items[fd];
        if (!item || !__atomic_load_n(&item->active, __ATOMIC_ACQUIRE)) continue;
        if (__atomic_load_n(&item->target_closed, __ATOMIC_ACQUIRE)) {
            item->revents = EPOLLHUP;
            ready++;
            continue;
        }

        /* Skip one-shot items that have been disabled after reporting */
        if (item->oneshot_disabled) continue;

        int      poll_result = process_file_poll(item->file, (size_t)(item->events | POLLERR | POLLHUP));
        uint32_t current     = epoll_map_poll_result(poll_result, item->events);

        if (item->events & EPOLLET) {
            /*
             * Edge-triggered: only report events that transitioned
             * from not-ready to ready since the last poll.
             */
            uint32_t changed = __atomic_exchange_n(&item->pending_events, 0, __ATOMIC_ACQ_REL);
            item->last_revents &= ~changed;
            uint32_t new_ready = current & ~item->last_revents;
            item->last_revents = current;
            /*
             * Preserve an edge that was found by an earlier scan but could
             * not be returned because maxevents was already exhausted.
             */
            item->revents |= new_ready;
        } else {
            /* Level-triggered: report all currently ready events */
            item->revents = current;
        }

        if (item->revents) ready++;
    }

    return ready;
}

/*
 * Check whether an epoll fd is readable without consuming edge state.
 * This is used when the epoll fd is itself watched by poll or another epoll
 * instance (libinput's epoll fd inside Xorg's epoll).  Only epoll_wait() may
 * exchange pending_events and advance last_revents.
 */
static bool epoll_has_ready(epoll_instance_t *epi)
{
    for (int fd = 0; fd <= epi->max_fd; fd++) {
        epoll_item_t *item = epi->items[fd];
        if (!item || !__atomic_load_n(&item->active, __ATOMIC_ACQUIRE) || item->oneshot_disabled) continue;
        if (__atomic_load_n(&item->target_closed, __ATOMIC_ACQUIRE) || item->revents) return true;

        int      poll_result = process_file_poll(item->file, (size_t)(item->events | POLLERR | POLLHUP));
        uint32_t current     = epoll_map_poll_result(poll_result, item->events);
        if (!(item->events & EPOLLET)) {
            if (current) return true;
            continue;
        }

        uint32_t pending  = __atomic_load_n(&item->pending_events, __ATOMIC_ACQUIRE);
        uint32_t observed = item->last_revents & ~pending;
        if (current & ~observed) return true;
    }
    return false;
}

/*
 * Collect ready events from the epoll set and copy them to user space.
 * Must be called with epi->lock held.
 * Returns number of events collected (0..maxevents), or -EFAULT on copy error.
 */
static int epoll_collect_events(epoll_instance_t *epi, epoll_event_t *user_events, int maxevents)
{
    int collected = 0;

    for (int fd = 0; fd <= epi->max_fd && collected < maxevents; fd++) {
        epoll_item_t *item = epi->items[fd];
        if (!item || !item->active) continue;
        if (item->revents == 0) continue;

        epoll_event_t ev;
        ev.events = item->revents;
        ev.data   = item->data;

        if (copy_to_user(&user_events[collected], &ev, sizeof(epoll_event_t))) return -EFAULT;

        collected++;

        /* Handle EPOLLONESHOT: disable this fd after reporting */
        if (item->events & EPOLLONESHOT) item->oneshot_disabled = 1;

        /* Clear revents for level-triggered; edge-triggered already cleared */
        item->revents = 0;
    }

    return collected;
}

/* VFS open callback; epoll nodes carry no name-based open state. */
static void epoll_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

/*
 * Close callback: wake all blocked waiters and decrement refcount.
 * Actual cleanup happens in epoll_vfs_free when refcount reaches 0.
 */
static void epoll_vfs_close(void *current)
{
    epoll_instance_t *epi = (epoll_instance_t *)current;
    if (!epi) return;

    spin_lock(&epi->lock);
    wait_queue_wake_all(&epi->wq);
    spin_unlock(&epi->lock);
}

/* Unsubscribe from the target's poll sources and free the item. */
static void epoll_item_release(epoll_item_t *item)
{
    if (!item) return;
    vfs_poll_source_unsubscribe(item->event_source, &item->subscription);
    vfs_poll_source_unsubscribe(&item->file->close_source, &item->close_subscription);
    process_file_put(item->file);
    free(item);
}

/* epoll does not support read(); return -EINVAL. */
static size_t epoll_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/* epoll does not support write(); return -EINVAL. */
static size_t epoll_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/* Poll callback: report POLLIN if any registered fd is ready. */
static int epoll_vfs_poll(void *file, size_t events)
{
    epoll_instance_t *epi = (epoll_instance_t *)file;
    if (!epi) return 0;

    int revents = 0;

    spin_lock(&epi->lock);
    if (epoll_has_ready(epi)) revents |= POLLIN;
    spin_unlock(&epi->lock);

    return revents & (int)events;
}

/* Free the epoll instance and all its items. */
static int epoll_vfs_free(void *handle)
{
    epoll_instance_t *epi = (epoll_instance_t *)handle;
    if (!epi) return -EINVAL;

    for (int fd = 0; fd <= epi->max_fd; fd++) {
        spin_lock(&epi->lock);
        epoll_item_t *item = epoll_item_del(epi, fd);
        spin_unlock(&epi->lock);
        epoll_item_release(item);
    }

    free(epi);
    return EOK;
}

/* Stubs for unused VFS callbacks */
static int epoll_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

static void epoll_stub_unmount(void *root)
{
    (void)root;
}

static size_t epoll_stub_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

static int epoll_stub_mk(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -ENOSYS;
}

static int epoll_stub_stat(void *file, vfs_node_t node)
{
    (void)file;
    (void)node;
    return EOK;
}

static int epoll_stub_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOSYS;
}

static vfs_node_t epoll_stub_dup(vfs_node_t node)
{
    (void)node;
    return NULL;
}

static int epoll_stub_del(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -ENOSYS;
}

static int epoll_stub_rename(const vfs_rename_context_t *context)
{
    (void)context;
    return -ENOSYS;
}

/* Allocate an epoll instance and wrap it in a VFS node. */
static vfs_node_t epoll_node_create(void)
{
    if (epoll_fsid < 0) return NULL;

    epoll_instance_t *epi = malloc(sizeof(epoll_instance_t));
    if (!epi) {
        plogk("epoll: Instance allocation failed.\n");
        return NULL;
    }
    memset(epi, 0, sizeof(epoll_instance_t));

    epi->fd_count = 0;
    epi->max_fd   = -1;
    epi->refcount = 1;
    wait_queue_init(&epi->wq);

    vfs_node_t node = vfs_node_alloc(NULL, "[epoll]");
    if (!node) {
        plogk("epoll: Node allocation failed.\n");
        free(epi);
        return NULL;
    }

    node->type   = file_epoll;
    node->handle = epi;
    node->fsid   = epoll_fsid;
    node->size   = 0;
    node->mode   = O_RDWR;

    epi->node = node;

    return node;
}

/*
 * Look up an epoll instance from an epoll fd within the current process.
 * Verifies the fd is valid and points to an epoll node.
 * Returns the epoll_instance_t * on success, NULL on error.
 * The caller is responsible for releasing the process_file reference.
 */
static epoll_instance_t *epoll_resolve_fd(int epfd, process_t *proc, process_file_t **out_file)
{
    if (!proc) return NULL;

    spin_lock(&proc->fd_lock);
    process_file_t *file = NULL;
    if (epfd >= 0 && epfd < PROCESS_MAX_FD) {
        file = proc->fds[epfd];
        if (file) process_file_get(file);
    }
    spin_unlock(&proc->fd_lock);

    if (!file) return NULL;
    if (!file->node || !(file->node->type & file_epoll) || !file->node->handle) {
        process_file_put(file);
        return NULL;
    }

    *out_file = file;
    return (epoll_instance_t *)file->node->handle;
}

/* Syscall: epoll_create */
int64_t sys_epoll_create(int size)
{
    (void)size;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = epoll_node_create();
    if (!node) return -ENOMEM;

    int fd = process_fd_install(proc, node, O_RDWR);
    if (fd < 0) {
        vfs_close(node);
        return fd;
    }

    return fd;
}

/* Syscall: epoll_create1 */
int64_t sys_epoll_create1(int flags)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = epoll_node_create();
    if (!node) return -ENOMEM;

    uint64_t fd_flags = O_RDWR;
    if (flags & EPOLL_CLOEXEC) fd_flags |= EPOLL_CLOEXEC;

    int fd = process_fd_install(proc, node, fd_flags);
    if (fd < 0) {
        vfs_close(node);
        return fd;
    }

    return fd;
}

/* Syscall: epoll_ctl */
int64_t sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *event)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    if (epfd == fd) return -EINVAL;

    process_file_t   *ep_file = NULL;
    epoll_instance_t *epi     = epoll_resolve_fd(epfd, proc, &ep_file);
    if (!epi) return -EBADF;

    epoll_event_t   ev;
    int64_t         ret;
    process_file_t *target        = NULL;
    epoll_item_t   *release       = NULL;
    bool            publish_ready = false;

    if (op == EPOLL_CTL_ADD) {
        target = process_fd_get(proc, fd);
        if (!target) {
            process_file_put(ep_file);
            return -EBADF;
        }
    }

    spin_lock(&epi->lock);

    switch (op) {
        case EPOLL_CTL_ADD : {
            if (!event) {
                ret = -EFAULT;
                break;
            }
            if (copy_from_user(&ev, event, sizeof(epoll_event_t))) {
                ret = -EFAULT;
                break;
            }

            epoll_item_t *old = epoll_item_find(epi, fd);
            if (old && __atomic_load_n(&old->target_closed, __ATOMIC_ACQUIRE)) release = epoll_item_del(epi, fd);
            epoll_item_t *item = epoll_item_add(epi, fd, target, &ev);
            if (!item) {
                ret = -EEXIST;
                break;
            }
            target             = NULL;
            item->event_source = vfs_file_poll_source(item->file->node, item->file->private_data);
            vfs_poll_source_subscribe(item->event_source, &item->subscription, UINT32_MAX, epoll_item_notify, item);
            vfs_poll_source_subscribe(&item->file->close_source, &item->close_subscription, UINT32_MAX, epoll_target_close, item);

            /* Poll immediately for initial readiness */
            int      poll_result = process_file_poll(item->file, (size_t)(item->events | POLLERR | POLLHUP));
            uint32_t current     = epoll_map_poll_result(poll_result, item->events);
            item->last_revents   = 0;
            item->pending_events = current;
            item->revents        = current;

            /* Wake any waiters if this fd is immediately ready */
            if (item->revents) {
                __atomic_add_fetch(&epi->event_generation, 1, __ATOMIC_RELEASE);
                wait_queue_wake_all(&epi->wq);
                publish_ready = true;
            }

            ret = EOK;
            break;
        }

        case EPOLL_CTL_DEL : {
            release = epoll_item_del(epi, fd);
            ret     = release ? EOK : -ENOENT;
            break;
        }

        case EPOLL_CTL_MOD : {
            if (!event) {
                ret = -EFAULT;
                break;
            }
            if (copy_from_user(&ev, event, sizeof(epoll_event_t))) {
                ret = -EFAULT;
                break;
            }

            ret = epoll_item_mod(epi, fd, &ev);
            if (ret != EOK) break;

            /* Re-poll for readiness after modification */
            int      poll_result = process_file_poll(epi->items[fd]->file, (size_t)(ev.events | POLLERR | POLLHUP));
            uint32_t current     = epoll_map_poll_result(poll_result, ev.events);

            epoll_item_t *item = epoll_item_find(epi, fd);
            if (item) {
                /*
                 * EPOLL_CTL_MOD re-arms the descriptor.  In particular,
                 * users such as Xorg add an EPOLLET fd with no read/write
                 * interest and then enable EPOLLIN after data may already
                 * have arrived.  Treat readiness observed during MOD as a
                 * fresh edge; recording it in last_revents here would make
                 * the following epoll_wait silently consume that edge.
                 */
                item->last_revents = 0;
                __atomic_store_n(&item->pending_events, current, __ATOMIC_RELEASE);
                item->revents = current;

                if (item->revents) {
                    __atomic_add_fetch(&epi->event_generation, 1, __ATOMIC_RELEASE);
                    wait_queue_wake_all(&epi->wq);
                    publish_ready = true;
                }
            }

            ret = EOK;
            break;
        }
        default :
            ret = -EINVAL;
            break;
    }

    spin_unlock(&epi->lock);
    if (publish_ready && epi->node) vfs_poll_notify(epi->node, POLLIN);
    if (target) process_file_put(target);
    epoll_item_release(release);
    process_file_put(ep_file);

    return ret;
}

/* Syscall: epoll_wait */
int64_t sys_epoll_wait(int epfd, epoll_event_t *events, int maxevents, int timeout)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    if (!events && maxevents > 0) return -EFAULT;
    if (maxevents <= 0 || maxevents > EPOLL_MAX_EVENTS) return -EINVAL;

    process_file_t   *ep_file = NULL;
    epoll_instance_t *epi     = epoll_resolve_fd(epfd, proc, &ep_file);
    if (!epi) return -EBADF;

    int64_t ret;

    uint64_t deadline = 0;
    if (timeout > 0) {
        uint64_t ticks = ((uint64_t)timeout * EPOLL_TICKS_PER_SEC + 999) / 1000;
        deadline       = sched_ticks() + ticks;
    }

    spin_lock(&epi->lock);

    for (;;) {
        uint64_t generation = __atomic_load_n(&epi->event_generation, __ATOMIC_ACQUIRE);
        /* Poll all registered fds */
        int ready = epoll_poll_all(epi);

        if (ready > 0) {
            /* Collect events into user buffer */
            ret = epoll_collect_events(epi, events, maxevents);
            break;
        }

        if (timeout == 0 || (deadline && sched_ticks() >= deadline)) {
            ret = 0;
            break;
        }

        spin_lock(&proc->signal.lock);
        bool interrupted = signal_has_interrupting_pending(&proc->signal);
        spin_unlock(&proc->signal.lock);
        if (interrupted) {
            ret = -ERESTARTSYS;
            break;
        }

        wait_queue_prepare(&epi->wq);
        if (__atomic_load_n(&epi->event_generation, __ATOMIC_ACQUIRE) != generation) wait_queue_wake_all(&epi->wq);
        spin_unlock(&epi->lock);
        if (deadline)
            (void)wait_queue_wait_timed(&epi->wq, deadline);
        else
            wait_queue_sleep();
        spin_lock(&epi->lock);
    }

    spin_unlock(&epi->lock);
    process_file_put(ep_file);

    return ret;
}

/* Syscall: epoll_pwait */
int64_t sys_epoll_pwait(int epfd, epoll_event_t *events, int maxevents, int timeout, const void *sigmask, size_t sigsetsize)
{
    process_t      *proc = process_current();
    task_t         *task = current_task();
    sigset_t        old_blocked;
    if (!proc || !task || task->process != proc) return -ESRCH;
    if (sigmask && sigsetsize != sizeof(sigset_t)) return -EINVAL;
    signal_state_t *sig = &proc->signal;

    if (sigmask) {
        sigset_t new_blocked;
        if (copy_from_user(&new_blocked, sigmask, sizeof(sigset_t))) return -EFAULT;

        sigdelset(&new_blocked, SIGKILL);
        sigdelset(&new_blocked, SIGSTOP);

        spin_lock(&sig->lock);
        old_blocked         = task->signal_blocked;
        task->signal_blocked = new_blocked;
        spin_unlock(&sig->lock);
    }

    int64_t ret = sys_epoll_wait(epfd, events, maxevents, timeout);

    if (sigmask) {
        spin_lock(&sig->lock);
        if (ret == -ERESTARTSYS && signal_has_pending(sig)) {
            task->signal_saved_mask   = old_blocked;
            task->signal_restore_mask = true;
        } else {
            task->signal_blocked = old_blocked;
        }
        spin_unlock(&sig->lock);
    }

    return ret;
}

/* Initialization */
void epoll_init(void)
{
    vfs_callback_t cb = malloc(sizeof(struct vfs_callback));
    if (!cb) {
        plogk("epoll: Failed to allocate VFS callback structure.\n");
        return;
    }
    memset(cb, 0, sizeof(struct vfs_callback));

    cb->mount    = epoll_stub_mount;
    cb->unmount  = epoll_stub_unmount;
    cb->open     = epoll_vfs_open;
    cb->close    = epoll_vfs_close;
    cb->read     = epoll_vfs_read;
    cb->write    = epoll_vfs_write;
    cb->readlink = epoll_stub_readlink;
    cb->mkdir    = epoll_stub_mk;
    cb->mkfile   = epoll_stub_mk;
    cb->link     = epoll_stub_mk;
    cb->symlink  = epoll_stub_mk;
    cb->stat     = epoll_stub_stat;
    cb->ioctl    = epoll_stub_ioctl;
    cb->dup      = epoll_stub_dup;
    cb->poll     = epoll_vfs_poll;
    cb->free     = epoll_vfs_free;
    cb->delete   = epoll_stub_del;
    cb->rename   = epoll_stub_rename;

    epoll_fsid = vfs_regist(cb);
    if (epoll_fsid < 0) {
        plogk("epoll: Failed to register VFS callback (err=%d)\n", epoll_fsid);
        free(cb);
        return;
    }

    plogk("epoll: Epoll subsystem registered (fsid=%d, max_fds=%d)\n", epoll_fsid, EPOLL_MAX_FDS);
}
