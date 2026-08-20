/*
 *
 *      eventfd.c
 *      Eventfd file descriptor implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/eventfd.h>
#include <syscall/poll.h>
#include <syscall/syscall.h>

#define EVENTFD_MAX_VAL    (0xfffffffffffffffeULL)
#define EVENTFD_UINT64_MAX (0xffffffffffffffffULL)

static int eventfd_fsid = -1;

static bool eventfd_signal_pending(void)
{
    process_t *proc = process_current();
    if (!proc) return false;
    spin_lock(&proc->signal.lock);
    bool pending = signal_has_interrupting_pending(&proc->signal);
    spin_unlock(&proc->signal.lock);
    return pending;
}

/* VFS open callback (no-op) */
static void eventfd_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

/* VFS close callback: reset the counter and wake all waiters */
static void eventfd_vfs_close(void *current)
{
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)current;
    if (!ctx) return;
    spin_lock(&ctx->lock);
    ctx->count = 0;
    spin_unlock(&ctx->lock);
    wait_queue_wake_all(&ctx->wq);
}

/* Consume the counter value, blocking interruptibly while it is empty. */
static int64_t eventfd_read_common(eventfd_ctx_t *ctx, void *addr, size_t size, uint64_t flags)
{
    if (!ctx) return -EIO;
    if (size != sizeof(uint64_t)) return -EINVAL;

    spin_lock(&ctx->lock);
    while (ctx->count == 0) {
        if ((ctx->flags | flags) & EFD_NONBLOCK) {
            spin_unlock(&ctx->lock);
            return -EAGAIN;
        }
        if (eventfd_signal_pending()) {
            spin_unlock(&ctx->lock);
            return -ERESTARTSYS;
        }
        wait_queue_prepare(&ctx->wq);
        spin_unlock(&ctx->lock);
        if (eventfd_signal_pending()) {
            wait_queue_cancel(&ctx->wq);
            return -ERESTARTSYS;
        }
        wait_queue_sleep();
        spin_lock(&ctx->lock);
    }

    uint64_t val;
    if (ctx->flags & EFD_SEMAPHORE) {
        val = 1;
        ctx->count--;
    } else {
        val        = ctx->count;
        ctx->count = 0;
    }

    memcpy(addr, &val, sizeof(val));
    spin_unlock(&ctx->lock);

    wait_queue_wake_all(&ctx->wq);
    return (int64_t)sizeof(uint64_t);
}

/* Add to the counter, blocking interruptibly while it would overflow. */
static int64_t eventfd_write_common(eventfd_ctx_t *ctx, const void *addr, size_t size, uint64_t flags)
{
    if (!ctx) return -EIO;
    if (size != sizeof(uint64_t)) return -EINVAL;

    uint64_t val;
    memcpy(&val, addr, sizeof(val));
    if (val == EVENTFD_UINT64_MAX) return -EINVAL;

    spin_lock(&ctx->lock);

    for (;;) {
        if (ctx->count > EVENTFD_MAX_VAL - val) {
            if ((ctx->flags | flags) & EFD_NONBLOCK) {
                spin_unlock(&ctx->lock);
                return -EAGAIN;
            }
            if (eventfd_signal_pending()) {
                spin_unlock(&ctx->lock);
                return -ERESTARTSYS;
            }
            wait_queue_prepare(&ctx->wq);
            spin_unlock(&ctx->lock);
            if (eventfd_signal_pending()) {
                wait_queue_cancel(&ctx->wq);
                return -ERESTARTSYS;
            }
            wait_queue_sleep();
            spin_lock(&ctx->lock);
            continue;
        }
        break;
    }

    ctx->count += val;
    spin_unlock(&ctx->lock);

    wait_queue_wake_all(&ctx->wq);
    return (int64_t)sizeof(uint64_t);
}

/* Legacy node callbacks used outside the per-open descriptor path. */
static size_t eventfd_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    int64_t ret = eventfd_read_common(file, addr, size, 0);
    return ret < 0 ? (size_t)-1 : (size_t)ret;
}

static size_t eventfd_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    int64_t ret = eventfd_write_common(file, addr, size, 0);
    return ret < 0 ? (size_t)-1 : (size_t)ret;
}

/* VFS poll callback: report readability and writability */
static int eventfd_vfs_poll(void *file, size_t events)
{
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)file;
    if (!ctx) return 0;

    int revents = 0;
    spin_lock(&ctx->lock);

    if (ctx->count > 0) revents |= 0x001;
    if (ctx->count < EVENTFD_MAX_VAL) revents |= 0x004;

    spin_unlock(&ctx->lock);
    return revents & (int)events;
}

/* File read entry: validate size then delegate to the read callback */
static int64_t eventfd_vfs_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)offset;
    int64_t ret = eventfd_read_common(node->handle, addr, size, flags);
    if (ret > 0) vfs_poll_notify(node, POLLOUT);
    return ret;
}

/* File write entry: validate size and value then delegate to the write callback */
static int64_t eventfd_vfs_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)offset;
    int64_t ret = eventfd_write_common(node->handle, addr, size, flags);
    if (ret > 0) vfs_poll_notify(node, POLLIN);
    return ret;
}

/* VFS free callback: release the eventfd context */
static int eventfd_vfs_free(void *handle)
{
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)handle;
    if (!ctx) return -EINVAL;
    free(ctx);
    return EOK;
}

/* Generic stubs for unused VFS callbacks */

/* Unsupported unmount callback */
static void eventfd_stub_unmount(void *root)
{
    (void)root;
}

/* Unsupported stat callback */
static int eventfd_stub_stat(void *f, vfs_node_t n)
{
    (void)f;
    (void)n;
    return EOK;
}

/* Unsupported mkdir/mkfile/link/symlink callback */
static int eventfd_stub_mk(void *p, const char *nm, vfs_node_t n)
{
    (void)p;
    (void)nm;
    (void)n;
    return -ENOSYS;
}

/* Unsupported readlink callback */
static size_t eventfd_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}

/* Unsupported ioctl callback */
static int eventfd_stub_ioctl(void *f, size_t o, void *a)
{
    (void)f;
    (void)o;
    (void)a;
    return -ENOSYS;
}

/* Unsupported dup callback */
static vfs_node_t eventfd_stub_dup(vfs_node_t n)
{
    (void)n;
    return NULL;
}

/* Unsupported delete callback */
static int eventfd_stub_del(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -ENOSYS;
}

/* Unsupported rename callback */
static int eventfd_stub_rename(const vfs_rename_context_t *context)
{
    (void)context;
    return -ENOSYS;
}

/* Unsupported mount callback */
static int eventfd_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

/* Allocate and initialize an eventfd VFS node */
static vfs_node_t eventfd_node_create(unsigned int initval, int flags)
{
    if (eventfd_fsid < 0) return NULL;

    eventfd_ctx_t *ctx = calloc(1, sizeof(eventfd_ctx_t));
    if (!ctx) return NULL;

    ctx->count = initval;
    ctx->flags = (uint64_t)(flags & (EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC));
    wait_queue_init(&ctx->wq);

    vfs_node_t node = vfs_node_alloc(NULL, "[eventfd]");
    if (!node) {
        free(ctx);
        return NULL;
    }

    node->type   = file_stream;
    node->handle = ctx;
    node->fsid   = eventfd_fsid;
    node->size   = 0;
    node->mode   = O_RDWR;

    return node;
}

/* eventfd syscall: create an eventfd descriptor */
int sys_eventfd(unsigned int initval, int flags)
{
    if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC)) return -EINVAL;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = eventfd_node_create(initval, flags);
    if (!node) return -ENOMEM;

    uint64_t fd_flags = O_RDWR;
    if (flags & EFD_NONBLOCK) fd_flags |= O_NONBLOCK;
    if (flags & EFD_CLOEXEC) fd_flags |= O_CLOEXEC;
    int fd = process_fd_install(proc, node, fd_flags);
    if (fd < 0) {
        vfs_close(node);
        return fd;
    }
    return fd;
}

/* eventfd2 syscall: create an eventfd descriptor with flags */
int sys_eventfd2(unsigned int initval, int flags)
{
    return sys_eventfd(initval, flags);
}

/* Register the eventfd filesystem callback set */
void eventfd_init(void)
{
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("eventfd: Failed to allocate callback.\n");
        return;
    }
    cb->mount      = eventfd_stub_mount;
    cb->unmount    = eventfd_stub_unmount;
    cb->open       = eventfd_vfs_open;
    cb->close      = eventfd_vfs_close;
    cb->read       = eventfd_vfs_read;
    cb->write      = eventfd_vfs_write;
    cb->readlink   = eventfd_stub_readlink;
    cb->mkdir      = eventfd_stub_mk;
    cb->mkfile     = eventfd_stub_mk;
    cb->link       = eventfd_stub_mk;
    cb->symlink    = eventfd_stub_mk;
    cb->stat       = eventfd_stub_stat;
    cb->ioctl      = eventfd_stub_ioctl;
    cb->dup        = eventfd_stub_dup;
    cb->poll       = eventfd_vfs_poll;
    cb->file_read  = eventfd_vfs_file_read;
    cb->file_write = eventfd_vfs_file_write;
    cb->free       = eventfd_vfs_free;
    cb->delete     = eventfd_stub_del;
    cb->rename     = eventfd_stub_rename;

    eventfd_fsid = vfs_regist(cb);
    if (eventfd_fsid < 0) {
        plogk("eventfd: Failed to register VFS callback.\n");
        free(cb);
        return;
    }
    plogk("eventfd: Eventfd subsystem registered (fsid=%d)\n", eventfd_fsid);
}
