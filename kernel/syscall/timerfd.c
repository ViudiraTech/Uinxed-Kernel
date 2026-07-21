/*
 *
 *      timerfd.c
 *      Timerfd file descriptor implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <heap.h>
#include <printk.h>
#include <process.h>
#include <sched.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <task.h>
#include <timerfd.h>
#include <uaccess.h>
#include <vfs.h>

typedef struct {
        uint64_t it_interval_ns;
        uint64_t it_value_ns;
} timerfd_itimerspec_t;

static int timerfd_fsid = -1;

/* ---------- VFS callback implementations ---------- */

static void timerfd_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;(void)name;(void)node;
}

static void timerfd_vfs_close(void *current)
{
    (void)current;
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

    uint64_t val       = ctx->expire_count;
    ctx->expire_count  = 0;
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
    revents |= 0x004;
    spin_unlock(&ctx->lock);
    return revents & (int)events;
}

static int timerfd_vfs_free(void *handle)
{
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)handle;
    if (!ctx) return -EINVAL;
    free(ctx);
    return EOK;
}

/* Generic stubs */
static void timerfd_stub_unmount(void *root) { (void)root; }
static int timerfd_stub_stat(void *f, vfs_node_t n) { (void)f;(void)n;return EOK; }
static int timerfd_stub_mk(void *p, const char *nm, vfs_node_t n) { (void)p;(void)nm;(void)n;return -ENOSYS; }
static size_t timerfd_stub_write(void *f, const void *a, size_t o, size_t s) { (void)f;(void)a;(void)o;(void)s;return (size_t)-1; }
static size_t timerfd_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s) { (void)n;(void)a;(void)o;(void)s;return (size_t)-1; }
static int timerfd_stub_ioctl(void *f, size_t o, void *a) { (void)f;(void)o;(void)a;return -ENOSYS; }
static vfs_node_t timerfd_stub_dup(vfs_node_t n) { (void)n;return NULL; }
static int timerfd_stub_del(void *p, vfs_node_t n) { (void)p;(void)n;return -ENOSYS; }
static int timerfd_stub_rename(void *c, const char *nm) { (void)c;(void)nm;return -ENOSYS; }
static int timerfd_stub_mount(const char *s, vfs_node_t n) { (void)s;(void)n;return -ENOSYS; }

/* ---------- Public API ---------- */

static vfs_node_t timerfd_node_create(int clockid, int flags)
{
    if (timerfd_fsid < 0) return NULL;

    timerfd_ctx_t *ctx = calloc(1, sizeof(timerfd_ctx_t));
    if (!ctx) return NULL;

    ctx->clockid      = (uint64_t)clockid;
    ctx->flags        = (uint64_t)(flags & (TFD_NONBLOCK | TFD_CLOEXEC));
    ctx->expire_count = 0;
    ctx->interval_ns  = 0;
    ctx->value_ns     = 0;
    ctx->start_tick   = 0;
    ctx->armed        = 0;
    ctx->one_shot     = 0;
    wait_queue_init(&ctx->wq);

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
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC && clockid != CLOCK_BOOTTIME) {
        return -EINVAL;
    }

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = timerfd_node_create(clockid, flags);
    if (!node) return -ENOMEM;

    int fd = process_fd_install(proc, node, O_RDONLY);
    if (fd < 0) {
        timerfd_vfs_free(node->handle);
        vfs_free(node);
        return fd;
    }
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
    if (!file->node || !file->node->handle) {
        process_file_put(file);
        return -EINVAL;
    }

    timerfd_ctx_t *ctx = (timerfd_ctx_t *)file->node->handle;

    timerfd_itimerspec_t new_its;
    if (new_value) {
        if (copy_from_user(&new_its, new_value, sizeof(new_its))) {
            process_file_put(file);
            return -EFAULT;
        }
    }

    spin_lock(&ctx->lock);

    if (old_value) {
        timerfd_itimerspec_t old_its = {
            .it_interval_ns = ctx->one_shot ? 0 : ctx->interval_ns,
            .it_value_ns    = ctx->armed ? ctx->value_ns : 0,
        };
        spin_unlock(&ctx->lock);
        if (copy_to_user(old_value, &old_its, sizeof(old_its))) {
            process_file_put(file);
            return -EFAULT;
        }
        spin_lock(&ctx->lock);
    }

    if (!new_value) {
        spin_unlock(&ctx->lock);
        process_file_put(file);
        return EOK;
    }

    if (new_its.it_value_ns == 0) {
        ctx->armed = 0;
    } else {
        ctx->interval_ns = new_its.it_interval_ns;
        ctx->value_ns    = new_its.it_value_ns;
        ctx->start_tick  = sched_ticks();
        ctx->armed       = 1;
        ctx->one_shot    = (new_its.it_interval_ns == 0);

        if ((flags & TFD_TIMER_ABSTIME) && ctx->clockid == CLOCK_REALTIME) {
            uint64_t now_ns = sched_ticks() * 10000000ULL;
            if (ctx->value_ns <= now_ns) {
                ctx->value_ns = 1;
            } else {
                ctx->value_ns = ctx->value_ns - now_ns;
            }
        }
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
    if (!file->node || !file->node->handle) {
        process_file_put(file);
        return -EINVAL;
    }

    timerfd_ctx_t *ctx = (timerfd_ctx_t *)file->node->handle;

    spin_lock(&ctx->lock);
    timerfd_itimerspec_t its = {
        .it_interval_ns = ctx->one_shot ? 0 : ctx->interval_ns,
        .it_value_ns    = ctx->armed ? ctx->value_ns : 0,
    };
    spin_unlock(&ctx->lock);

    process_file_put(file);

    if (copy_to_user(curr_value, &its, sizeof(its))) return -EFAULT;
    return EOK;
}

void timerfd_init(void)
{
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("timerfd: Failed to allocate callback.\n");
        return;
    }
    cb->mount    = timerfd_stub_mount;
    cb->unmount  = timerfd_stub_unmount;
    cb->open     = timerfd_vfs_open;
    cb->close    = timerfd_vfs_close;
    cb->read     = timerfd_vfs_read;
    cb->write    = timerfd_stub_write;
    cb->readlink = timerfd_stub_readlink;
    cb->mkdir    = timerfd_stub_mk;
    cb->mkfile   = timerfd_stub_mk;
    cb->link     = timerfd_stub_mk;
    cb->symlink  = timerfd_stub_mk;
    cb->stat     = timerfd_stub_stat;
    cb->ioctl    = timerfd_stub_ioctl;
    cb->dup      = timerfd_stub_dup;
    cb->poll     = timerfd_vfs_poll;
    cb->free     = timerfd_vfs_free;
    cb->delete   = timerfd_stub_del;
    cb->rename   = timerfd_stub_rename;

    timerfd_fsid = vfs_regist(cb);
    if (timerfd_fsid < 0) {
        plogk("timerfd: Failed to register VFS callback.\n");
        free(cb);
        return;
    }
    plogk("timerfd: Subsystem initialized (fsid=%d).\n", timerfd_fsid);
}