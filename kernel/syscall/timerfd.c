/*
 *
 *      timerfd.c
 *      Timerfd file descriptor implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smp.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/syscall.h>
#include <syscall/timerfd.h>

typedef struct {
        int64_t tv_sec;
        int64_t tv_nsec;
} timerfd_timespec_t;

typedef struct {
        timerfd_timespec_t it_interval;
        timerfd_timespec_t it_value;
} timerfd_itimerspec_t;

static int          timerfd_fsid = -1;
static ilist_node_t timerfd_list;
static spinlock_t   timerfd_list_lock;

#define TIMERFD_TICK_NS 10000000ULL

static int timerfd_timespec_to_ticks(const timerfd_timespec_t *ts, uint64_t *ticks)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL) return -EINVAL;
    if ((uint64_t)ts->tv_sec > UINT64_MAX / 100ULL) return -EINVAL;

    *ticks = (uint64_t)ts->tv_sec * 100ULL + ((uint64_t)ts->tv_nsec + TIMERFD_TICK_NS - 1) / TIMERFD_TICK_NS;
    if (!*ticks && (ts->tv_sec || ts->tv_nsec)) *ticks = 1;
    return EOK;
}

static timerfd_timespec_t timerfd_ticks_to_timespec(uint64_t ticks)
{
    timerfd_timespec_t ts = {
        .tv_sec  = (int64_t)(ticks / 100ULL),
        .tv_nsec = (int64_t)((ticks % 100ULL) * TIMERFD_TICK_NS),
    };
    return ts;
}

/* ---------- VFS callback implementations ---------- */

static void timerfd_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

static void timerfd_vfs_close(void *current)
{
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)current;
    if (!ctx) return;

    spin_lock(&timerfd_list_lock);
    if (ctx->timers.next != &ctx->timers) ilist_remove(&ctx->timers);
    spin_unlock(&timerfd_list_lock);
}

static size_t timerfd_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)file;
    if (!ctx) return (size_t)-1;
    if (size < sizeof(uint64_t)) return (size_t)-1;

    spin_lock(&ctx->lock);

    if (ctx->expire_count == 0) {
        if (ctx->flags & TFD_NONBLOCK) {
            spin_unlock(&ctx->lock);
            return (size_t)-1;
        }
        spin_unlock(&ctx->lock);
        wait_queue_wait(&ctx->wq);
        spin_lock(&ctx->lock);

        if (ctx->expire_count == 0) {
            spin_unlock(&ctx->lock);
            return (size_t)-1;
        }
    }

    uint64_t val      = ctx->expire_count;
    ctx->expire_count = 0;
    spin_unlock(&ctx->lock);

    memcpy(addr, &val, sizeof(val));
    wait_queue_wake_all(&ctx->wq);
    return sizeof(uint64_t);
}

static int timerfd_vfs_poll(void *file, size_t events)
{
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)file;
    if (!ctx) return 0;

    int revents = 0;
    spin_lock(&ctx->lock);
    if (ctx->expire_count > 0) revents |= 0x001;
    spin_unlock(&ctx->lock);
    return revents & (int)events;
}

static int64_t timerfd_vfs_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)flags;
    if (size < sizeof(uint64_t)) return -EINVAL;
    size_t ret = timerfd_vfs_read(node->handle, addr, offset, size);
    return ret == (size_t)-1 ? -EAGAIN : (int64_t)ret;
}

static int timerfd_vfs_free(void *handle)
{
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)handle;
    if (!ctx) return -EINVAL;
    free(ctx);
    return EOK;
}

/* Generic stubs */
static void timerfd_stub_unmount(void *root)
{
    (void)root;
}
static int timerfd_stub_stat(void *f, vfs_node_t n)
{
    (void)f;
    (void)n;
    return EOK;
}
static int timerfd_stub_mk(void *p, const char *nm, vfs_node_t n)
{
    (void)p;
    (void)nm;
    (void)n;
    return -ENOSYS;
}
static size_t timerfd_stub_write(void *f, const void *a, size_t o, size_t s)
{
    (void)f;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}
static size_t timerfd_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}
static int timerfd_stub_ioctl(void *f, size_t o, void *a)
{
    (void)f;
    (void)o;
    (void)a;
    return -ENOSYS;
}
static vfs_node_t timerfd_stub_dup(vfs_node_t n)
{
    (void)n;
    return NULL;
}
static int timerfd_stub_del(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -ENOSYS;
}
static int timerfd_stub_rename(void *c, const char *nm)
{
    (void)c;
    (void)nm;
    return -ENOSYS;
}
static int timerfd_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

/* ---------- Public API ---------- */

static vfs_node_t timerfd_node_create(int clockid, int flags)
{
    if (timerfd_fsid < 0) return NULL;

    timerfd_ctx_t *ctx = calloc(1, sizeof(timerfd_ctx_t));
    if (!ctx) return NULL;

    ctx->clockid       = (uint64_t)clockid;
    ctx->flags         = (uint64_t)(flags & (TFD_NONBLOCK | TFD_CLOEXEC));
    ctx->expire_count  = 0;
    ctx->interval_ns   = 0;
    ctx->deadline_tick = 0;
    ctx->armed         = 0;
    wait_queue_init(&ctx->wq);
    ilist_init(&ctx->timers);

    vfs_node_t node = vfs_node_alloc(NULL, "[timerfd]");
    if (!node) {
        free(ctx);
        return NULL;
    }

    node->type   = file_stream;
    node->handle = ctx;
    node->fsid   = timerfd_fsid;
    node->size   = 0;
    node->mode   = O_RDONLY;

    return node;
}

int sys_timerfd_create(int clockid, int flags)
{
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC && clockid != CLOCK_BOOTTIME) { return -EINVAL; }
    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC)) return -EINVAL;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = timerfd_node_create(clockid, flags);
    if (!node) return -ENOMEM;

    uint64_t fd_flags = O_RDONLY;
    if (flags & TFD_CLOEXEC) fd_flags |= O_CLOEXEC;
    int fd = process_fd_install(proc, node, fd_flags);
    if (fd < 0) {
        vfs_close(node);
        return fd;
    }
    spin_lock(&timerfd_list_lock);
    ilist_insert_before(&timerfd_list, &((timerfd_ctx_t *)node->handle)->timers);
    spin_unlock(&timerfd_list_lock);
    return fd;
}

int sys_timerfd_settime(int fd, int flags, const void *new_value, void *old_value)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *file = NULL;
    spin_lock(&proc->fd_lock);
    if (fd >= 0 && fd < PROCESS_MAX_FD) {
        file = proc->fds[fd];
        if (file) {
            spin_lock(&file->lock);
            file->refcount++;
            spin_unlock(&file->lock);
        }
    }
    spin_unlock(&proc->fd_lock);

    if (!file) return -EBADF;
    if (!file->node || file->node->fsid != timerfd_fsid || !file->node->handle) {
        process_file_put(file);
        return -EINVAL;
    }

    timerfd_ctx_t *ctx = (timerfd_ctx_t *)file->node->handle;

    if (!new_value) {
        process_file_put(file);
        return -EFAULT;
    }
    /* CLOCK_REALTIME changes are not tracked yet, so cancellation cannot be
     * reported correctly. Reject the flag instead of silently losing it. */
    if (flags & TFD_TIMER_CANCEL_ON_SET) {
        process_file_put(file);
        return -EINVAL;
    }
    if ((flags & ~TFD_TIMER_ABSTIME) || ((flags & TFD_TIMER_ABSTIME) && ctx->clockid == CLOCK_REALTIME)) {
        process_file_put(file);
        return -EINVAL;
    }

    timerfd_itimerspec_t new_its;
    if (copy_from_user(&new_its, new_value, sizeof(new_its))) {
        process_file_put(file);
        return -EFAULT;
    }

    uint64_t interval_ticks;
    uint64_t value_ticks;
    if (timerfd_timespec_to_ticks(&new_its.it_interval, &interval_ticks) || timerfd_timespec_to_ticks(&new_its.it_value, &value_ticks)) {
        process_file_put(file);
        return -EINVAL;
    }

    spin_lock(&ctx->lock);

    if (old_value) {
        timerfd_itimerspec_t old_its = {
            .it_interval = timerfd_ticks_to_timespec(ctx->interval_ns),
            .it_value    = timerfd_ticks_to_timespec(ctx->armed && ctx->deadline_tick > sched_ticks() ? ctx->deadline_tick - sched_ticks() : 0),
        };
        spin_unlock(&ctx->lock);
        if (copy_to_user(old_value, &old_its, sizeof(old_its))) {
            process_file_put(file);
            return -EFAULT;
        }
        spin_lock(&ctx->lock);
    }

    ctx->interval_ns = interval_ticks;
    if (!value_ticks) {
        ctx->armed = 0;
    } else {
        uint64_t now       = sched_ticks();
        ctx->deadline_tick = (flags & TFD_TIMER_ABSTIME) ? value_ticks : now + value_ticks;
        if (ctx->deadline_tick <= now) ctx->deadline_tick = now + 1;
        ctx->armed = 1;
    }

    spin_unlock(&ctx->lock);
    process_file_put(file);
    return EOK;
}

int sys_timerfd_gettime(int fd, void *curr_value)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *file = NULL;
    spin_lock(&proc->fd_lock);
    if (fd >= 0 && fd < PROCESS_MAX_FD) {
        file = proc->fds[fd];
        if (file) {
            spin_lock(&file->lock);
            file->refcount++;
            spin_unlock(&file->lock);
        }
    }
    spin_unlock(&proc->fd_lock);

    if (!file) return -EBADF;
    if (!curr_value) {
        process_file_put(file);
        return -EFAULT;
    }
    if (!file->node || file->node->fsid != timerfd_fsid || !file->node->handle) {
        process_file_put(file);
        return -EINVAL;
    }

    timerfd_ctx_t *ctx = (timerfd_ctx_t *)file->node->handle;

    spin_lock(&ctx->lock);
    timerfd_itimerspec_t its = {
        .it_interval = timerfd_ticks_to_timespec(ctx->interval_ns),
        .it_value    = timerfd_ticks_to_timespec(ctx->armed && ctx->deadline_tick > sched_ticks() ? ctx->deadline_tick - sched_ticks() : 0),
    };
    spin_unlock(&ctx->lock);

    process_file_put(file);

    if (copy_to_user(curr_value, &its, sizeof(its))) return -EFAULT;
    return EOK;
}

void timerfd_tick(void)
{
    if (get_current_cpu_id() != 0) return;

    uint64_t now = sched_ticks();
    spin_lock(&timerfd_list_lock);
    for (ilist_node_t *node = timerfd_list.next; node != &timerfd_list; node = node->next) {
        timerfd_ctx_t *ctx = (timerfd_ctx_t *)((char *)node - offsetof(timerfd_ctx_t, timers));
        spin_lock(&ctx->lock);
        if (ctx->armed && now >= ctx->deadline_tick) {
            uint64_t expirations = 1;
            if (ctx->interval_ns) {
                expirations += (now - ctx->deadline_tick) / ctx->interval_ns;
                ctx->deadline_tick += expirations * ctx->interval_ns;
            } else {
                ctx->armed = 0;
            }
            if (UINT64_MAX - ctx->expire_count < expirations)
                ctx->expire_count = UINT64_MAX;
            else
                ctx->expire_count += expirations;
            spin_unlock(&ctx->lock);
            wait_queue_wake_all(&ctx->wq);
            continue;
        }
        spin_unlock(&ctx->lock);
    }
    spin_unlock(&timerfd_list_lock);
}

void timerfd_init(void)
{
    ilist_init(&timerfd_list);
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("timerfd: Failed to allocate callback.\n");
        return;
    }
    cb->mount     = timerfd_stub_mount;
    cb->unmount   = timerfd_stub_unmount;
    cb->open      = timerfd_vfs_open;
    cb->close     = timerfd_vfs_close;
    cb->read      = timerfd_vfs_read;
    cb->write     = timerfd_stub_write;
    cb->readlink  = timerfd_stub_readlink;
    cb->mkdir     = timerfd_stub_mk;
    cb->mkfile    = timerfd_stub_mk;
    cb->link      = timerfd_stub_mk;
    cb->symlink   = timerfd_stub_mk;
    cb->stat      = timerfd_stub_stat;
    cb->ioctl     = timerfd_stub_ioctl;
    cb->dup       = timerfd_stub_dup;
    cb->poll      = timerfd_vfs_poll;
    cb->file_read = timerfd_vfs_file_read;
    cb->free      = timerfd_vfs_free;
    cb->delete    = timerfd_stub_del;
    cb->rename    = timerfd_stub_rename;

    timerfd_fsid = vfs_regist(cb);
    if (timerfd_fsid < 0) {
        plogk("timerfd: Failed to register VFS callback.\n");
        free(cb);
        return;
    }
    plogk("timerfd: Timerfd subsystem registered (fsid=%d)\n", timerfd_fsid);
}
