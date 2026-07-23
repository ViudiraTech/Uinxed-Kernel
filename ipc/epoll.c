/*
 *
 *      epoll.c
 *      Epoll event notification implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/vfs.h>
#include <ipc/epoll.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/syscall.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define EPOLL_MAX_FDS       1024
#define EPOLL_TICKS_PER_SEC 100 /* scheduler tick frequency */

/* poll event bits (matching pipe.c) */
#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010

/* ------------------------------------------------------------------ */
/*  Internal structures                                                 */
/* ------------------------------------------------------------------ */

typedef struct epoll_instance epoll_instance_t;

typedef struct epoll_item {
        int               fd;
        uint32_t          events;
        uint32_t          revents;
        epoll_data_t      data;
        epoll_instance_t *epi;
        int               active;
        uint32_t          last_revents;     /* previous poll result for edge-triggered */
        int               oneshot_disabled; /* EPOLLONESHOT re-arm flag */
} epoll_item_t;

typedef struct epoll_instance {
        epoll_item_t    *items[EPOLL_MAX_FDS];
        int              fd_count;
        wait_queue_t     wq;
        spinlock_t       lock;
        struct vfs_node *node;
        uint32_t         refcount;
} epoll_instance_t;

/* ------------------------------------------------------------------ */
/*  Static filesystem ID                                                */
/* ------------------------------------------------------------------ */

static int epoll_fsid = -1;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Item operations                                                     */
/* ------------------------------------------------------------------ */

/*
 * Find an epoll_item by fd.  Must be called with epi->lock held.
 */
static epoll_item_t *epoll_item_find(epoll_instance_t *epi, int fd)
{
    if (fd < 0 || fd >= EPOLL_MAX_FDS) return NULL;
    return epi->items[fd];
}

/*
 * Add a new fd to the epoll set.  Must be called with epi->lock held.
 * Returns NULL on error (fd already present, or OOM).
 */
static epoll_item_t *epoll_item_add(epoll_instance_t *epi, int fd, const epoll_event_t *event)
{
    if (fd < 0 || fd >= EPOLL_MAX_FDS) return NULL;
    if (epi->items[fd]) return NULL; /* already present */

    epoll_item_t *item = malloc(sizeof(epoll_item_t));
    if (!item) return NULL;
    memset(item, 0, sizeof(epoll_item_t));

    item->fd               = fd;
    item->events           = event->events;
    item->revents          = 0;
    item->data             = event->data;
    item->epi              = epi;
    item->active           = 1;
    item->last_revents     = 0;
    item->oneshot_disabled = 0;

    epi->items[fd] = item;
    epi->fd_count++;

    return item;
}

/*
 * Delete an fd from the epoll set.  Must be called with epi->lock held.
 * Returns 0 on success, -ENOENT if not found.
 */
static int epoll_item_del(epoll_instance_t *epi, int fd)
{
    if (fd < 0 || fd >= EPOLL_MAX_FDS) return -ENOENT;

    epoll_item_t *item = epi->items[fd];
    if (!item) return -ENOENT;

    epi->items[fd] = NULL;
    epi->fd_count--;
    free(item);

    return EOK;
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
    item->oneshot_disabled = 0; /* re-arm after EPOLL_CTL_MOD */

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Polling: check all registered fds for readiness                     */
/* ------------------------------------------------------------------ */

/*
 * Poll all registered fds and update their revents.
 * Must be called with epi->lock held.
 * Returns the number of ready fds.
 */
static int epoll_poll_all(epoll_instance_t *epi)
{
    process_t *proc = process_current();
    if (!proc) return 0;

    int ready = 0;

    for (int fd = 0; fd < EPOLL_MAX_FDS; fd++) {
        epoll_item_t *item = epi->items[fd];
        if (!item || !item->active) continue;

        /* Skip one-shot items that have been disabled after reporting */
        if (item->oneshot_disabled) continue;

        int      poll_result = process_fd_poll(proc, fd, (size_t)item->events);
        uint32_t current     = epoll_map_poll_result(poll_result, item->events);

        if (item->events & EPOLLET) {
            /*
             * Edge-triggered: only report events that transitioned
             * from not-ready to ready since the last poll.
             */
            uint32_t new_ready = current & ~item->last_revents;
            item->last_revents = current;
            item->revents      = new_ready;
        } else {
            /* Level-triggered: report all currently ready events */
            item->revents = current;
        }

        if (item->revents) ready++;
    }

    return ready;
}

/*
 * Collect ready events from the epoll set and copy them to user space.
 * Must be called with epi->lock held.
 * Returns number of events collected (0..maxevents), or -EFAULT on copy error.
 */
static int epoll_collect_events(epoll_instance_t *epi, epoll_event_t *user_events, int maxevents)
{
    int collected = 0;

    for (int fd = 0; fd < EPOLL_MAX_FDS && collected < maxevents; fd++) {
        epoll_item_t *item = epi->items[fd];
        if (!item || !item->active) continue;
        if (item->revents == 0) continue;

        epoll_event_t ev;
        ev.events = item->revents;
        ev.data   = item->data;

        if (copy_to_user(&user_events[collected], &ev, sizeof(epoll_event_t))) { return -EFAULT; }

        collected++;

        /* Handle EPOLLONESHOT: disable this fd after reporting */
        if (item->events & EPOLLONESHOT) { item->oneshot_disabled = 1; }

        /* Clear revents for level-triggered; edge-triggered already cleared */
        item->revents = 0;
    }

    return collected;
}

/* ------------------------------------------------------------------ */
/*  VFS callbacks                                                       */
/* ------------------------------------------------------------------ */

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

/*
 * epoll does not support read(); return -EINVAL.
 */
static size_t epoll_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/*
 * epoll does not support write(); return -EINVAL.
 */
static size_t epoll_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

/*
 * Poll callback: report POLLIN if any registered fd is ready.
 */
static int epoll_vfs_poll(void *file, size_t events)
{
    epoll_instance_t *epi = (epoll_instance_t *)file;
    if (!epi) return 0;

    int revents = 0;

    spin_lock(&epi->lock);
    if (epoll_poll_all(epi) > 0) { revents |= POLLIN; }
    spin_unlock(&epi->lock);

    return revents & (int)events;
}

/*
 * Free the epoll instance and all its items.
 */
static int epoll_vfs_free(void *handle)
{
    epoll_instance_t *epi = (epoll_instance_t *)handle;
    if (!epi) return -EINVAL;

    spin_lock(&epi->lock);

    for (int fd = 0; fd < EPOLL_MAX_FDS; fd++) {
        epoll_item_t *item = epi->items[fd];
        if (item) {
            epi->items[fd] = NULL;
            free(item);
        }
    }

    spin_unlock(&epi->lock);

    free(epi);
    return EOK;
}

/* ---------- Stubs for unused VFS callbacks ---------- */

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

static int epoll_stub_rename(void *current, const char *new_name)
{
    (void)current;
    (void)new_name;
    return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/*  Epoll node creation                                                 */
/* ------------------------------------------------------------------ */

static vfs_node_t epoll_node_create(void)
{
    if (epoll_fsid < 0) return NULL;

    epoll_instance_t *epi = malloc(sizeof(epoll_instance_t));
    if (!epi) return NULL;
    memset(epi, 0, sizeof(epoll_instance_t));

    epi->fd_count = 0;
    epi->refcount = 1;
    wait_queue_init(&epi->wq);

    vfs_node_t node = vfs_node_alloc(NULL, "[epoll]");
    if (!node) {
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
        if (file) {
            spin_lock(&file->lock);
            file->refcount++;
            spin_unlock(&file->lock);
        }
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

/* ------------------------------------------------------------------ */
/*  Syscall: epoll_create                                               */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_create(int size)
{
    (void)size;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = epoll_node_create();
    if (!node) return -ENOMEM;

    int fd = process_fd_install(proc, node, O_RDWR);
    if (fd < 0) {
        epoll_vfs_free(node->handle);
        vfs_free(node);
        return fd;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/*  Syscall: epoll_create1                                              */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_create1(int flags)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = epoll_node_create();
    if (!node) return -ENOMEM;

    uint64_t fd_flags = O_RDWR;
    if (flags & EPOLL_CLOEXEC) { fd_flags |= EPOLL_CLOEXEC; }

    int fd = process_fd_install(proc, node, fd_flags);
    if (fd < 0) {
        epoll_vfs_free(node->handle);
        vfs_free(node);
        return fd;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/*  Syscall: epoll_ctl                                                  */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *event)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    if (epfd == fd) return -EINVAL;

    process_file_t   *ep_file = NULL;
    epoll_instance_t *epi     = epoll_resolve_fd(epfd, proc, &ep_file);
    if (!epi) return -EBADF;

    epoll_event_t ev;
    int64_t       ret;

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

            epoll_item_t *item = epoll_item_add(epi, fd, &ev);
            if (!item) {
                ret = -EEXIST;
                break;
            }

            /* Poll immediately for initial readiness */
            int      poll_result = process_fd_poll(proc, fd, (size_t)item->events);
            uint32_t current     = epoll_map_poll_result(poll_result, item->events);
            item->last_revents   = current;
            if (item->events & EPOLLET) {
                item->revents = current;
            } else {
                item->revents = current;
            }

            /* Wake any waiters if this fd is immediately ready */
            if (item->revents) { wait_queue_wake_all(&epi->wq); }

            ret = EOK;
            break;
        }

        case EPOLL_CTL_DEL : {
            ret = epoll_item_del(epi, fd);
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
            int      poll_result = process_fd_poll(proc, fd, (size_t)ev.events);
            uint32_t current     = epoll_map_poll_result(poll_result, ev.events);

            epoll_item_t *item = epoll_item_find(epi, fd);
            if (item) {
                item->last_revents = current;
                if (ev.events & EPOLLET) {
                    item->revents = current;
                } else {
                    item->revents = current;
                }

                if (item->revents) { wait_queue_wake_all(&epi->wq); }
            }

            ret = EOK;
            break;
        }

        default :
            ret = -EINVAL;
            break;
    }

    spin_unlock(&epi->lock);
    process_file_put(ep_file);

    return ret;
}

/* ------------------------------------------------------------------ */
/*  Syscall: epoll_wait                                                 */
/* ------------------------------------------------------------------ */

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

    spin_lock(&epi->lock);

    for (;;) {
        /* Poll all registered fds */
        int ready = epoll_poll_all(epi);

        if (ready > 0) {
            /* Collect events into user buffer */
            ret = epoll_collect_events(epi, events, maxevents);
            break;
        }

        if (timeout == 0) {
            ret = 0;
            break;
        }

        /*
         * No events ready and timeout != 0: block.
         * Release the lock, sleep, re-acquire on wakeup.
         */
        spin_unlock(&epi->lock);

        if (timeout > 0) {
            /* Convert milliseconds to scheduler ticks */
            uint64_t ticks = ((uint64_t)timeout * EPOLL_TICKS_PER_SEC + 999) / 1000;
            task_sleep_ticks(ticks);
        } else {
            /* timeout == -1: block indefinitely until woken */
            wait_queue_wait(&epi->wq);
        }

        spin_lock(&epi->lock);
    }

    spin_unlock(&epi->lock);
    process_file_put(ep_file);

    return ret;
}

/* ------------------------------------------------------------------ */
/*  Syscall: epoll_pwait                                                */
/* ------------------------------------------------------------------ */

int64_t sys_epoll_pwait(int epfd, epoll_event_t *events, int maxevents, int timeout, const void *sigmask, size_t sigsetsize)
{
    process_t      *proc = process_current();
    signal_state_t *sig  = &proc->signal;
    sigset_t        old_blocked;

    if (sigmask && sigsetsize >= sizeof(sigset_t)) {
        sigset_t new_blocked;
        if (copy_from_user(&new_blocked, sigmask, sizeof(sigset_t))) return -EFAULT;

        sigdelset(&new_blocked, SIGKILL);
        sigdelset(&new_blocked, SIGSTOP);

        spin_lock(&sig->lock);
        old_blocked = sig->blocked;
        sig->blocked = new_blocked;
        spin_unlock(&sig->lock);
    }

    int64_t ret = sys_epoll_wait(epfd, events, maxevents, timeout);

    if (sigmask && sigsetsize >= sizeof(sigset_t)) {
        spin_lock(&sig->lock);
        sig->blocked = old_blocked;
        spin_unlock(&sig->lock);
    }

    return ret;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                      */
/* ------------------------------------------------------------------ */

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
        plogk("epoll: Failed to register VFS callback (err=%d).\n", epoll_fsid);
        free(cb);
        return;
    }

    plogk("epoll: Subsystem initialized (fsid=%d, max_fds=%d)\n", epoll_fsid, EPOLL_MAX_FDS);
}