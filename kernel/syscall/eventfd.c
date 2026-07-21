/*
 *
 *      eventfd.c
 *      Eventfd file descriptor implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <eventfd.h>
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
#include <uaccess.h>
#include <vfs.h>

#define EVENTFD_MAX_VAL    (0xfffffffffffffffeULL)
#define EVENTFD_UINT64_MAX (0xffffffffffffffffULL)

static int eventfd_fsid = -1;

/* ---------- VFS callback implementations ---------- */

static void eventfd_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

static void eventfd_vfs_close(void *current)
{
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)current;
    if (!ctx) return;
    spin_lock(&ctx->lock);
    ctx->count = 0;
    spin_unlock(&ctx->lock);
    wait_queue_wake_all(&ctx->wq);
}

static size_t eventfd_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)file;
    if (!ctx) return (size_t)-1;
    if (size < sizeof(uint64_t)) return (size_t)-1;

    spin_lock(&ctx->lock);

    if (ctx->count == 0) {
        if (ctx->flags & EFD_NONBLOCK) {
            spin_unlock(&ctx->lock);
            return (size_t)-1;
        }
        spin_unlock(&ctx->lock);
        wait_queue_wait(&ctx->wq);
        spin_lock(&ctx->lock);

        if (ctx->count == 0) {
            spin_unlock(&ctx->lock);
            return (size_t)-1;
        }
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
    return sizeof(uint64_t);
}

static size_t eventfd_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)file;
    if (!ctx) return (size_t)-1;
    if (size < sizeof(uint64_t)) return (size_t)-1;

    uint64_t val;
    memcpy(&val, addr, sizeof(val));
    if (val == EVENTFD_UINT64_MAX) return (size_t)-1;

    spin_lock(&ctx->lock);

    for (;;) {
        if (ctx->count > EVENTFD_MAX_VAL - val) {
            if (ctx->flags & EFD_NONBLOCK) {
                spin_unlock(&ctx->lock);
                return (size_t)-1;
            }
            spin_unlock(&ctx->lock);
            wait_queue_wait(&ctx->wq);
            spin_lock(&ctx->lock);
            continue;
        }
        break;
    }

    ctx->count += val;
    spin_unlock(&ctx->lock);

    wait_queue_wake_all(&ctx->wq);
    return sizeof(uint64_t);
}

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

static int eventfd_vfs_free(void *handle)
{
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)handle;
    if (!ctx) return -EINVAL;
    free(ctx);
    return EOK;
}

/* ---------- Generic stubs for unused VFS callbacks ---------- */
static void eventfd_stub_unmount(void *root)
{
    (void)root;
}
static int eventfd_stub_stat(void *f, vfs_node_t n)
{
    (void)f;
    (void)n;
    return EOK;
}
static int eventfd_stub_mk(void *p, const char *nm, vfs_node_t n)
{
    (void)p;
    (void)nm;
    (void)n;
    return -ENOSYS;
}
static size_t eventfd_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}
static int eventfd_stub_ioctl(void *f, size_t o, void *a)
{
    (void)f;
    (void)o;
    (void)a;
    return -ENOSYS;
}
static vfs_node_t eventfd_stub_dup(vfs_node_t n)
{
    (void)n;
    return NULL;
}
static int eventfd_stub_del(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -ENOSYS;
}
static int eventfd_stub_rename(void *c, const char *nm)
{
    (void)c;
    (void)nm;
    return -ENOSYS;
}
static int eventfd_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

/* ---------- Public API ---------- */

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

int sys_eventfd(unsigned int initval, int flags)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    vfs_node_t node = eventfd_node_create(initval, flags);
    if (!node) return -ENOMEM;

    int fd = process_fd_install(proc, node, (uint64_t)((flags & EFD_NONBLOCK) ? (O_RDWR | O_NONBLOCK) : O_RDWR));
    if (fd < 0) {
        eventfd_vfs_free(node->handle);
        vfs_free(node);
        return fd;
    }
    return fd;
}

int sys_eventfd2(unsigned int initval, int flags)
{
    return sys_eventfd(initval, flags);
}

void eventfd_init(void)
{
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("eventfd: Failed to allocate callback.\n");
        return;
    }
    cb->mount    = eventfd_stub_mount;
    cb->unmount  = eventfd_stub_unmount;
    cb->open     = eventfd_vfs_open;
    cb->close    = eventfd_vfs_close;
    cb->read     = eventfd_vfs_read;
    cb->write    = eventfd_vfs_write;
    cb->readlink = eventfd_stub_readlink;
    cb->mkdir    = eventfd_stub_mk;
    cb->mkfile   = eventfd_stub_mk;
    cb->link     = eventfd_stub_mk;
    cb->symlink  = eventfd_stub_mk;
    cb->stat     = eventfd_stub_stat;
    cb->ioctl    = eventfd_stub_ioctl;
    cb->dup      = eventfd_stub_dup;
    cb->poll     = eventfd_vfs_poll;
    cb->free     = eventfd_vfs_free;
    cb->delete   = eventfd_stub_del;
    cb->rename   = eventfd_stub_rename;

    eventfd_fsid = vfs_regist(cb);
    if (eventfd_fsid < 0) {
        plogk("eventfd: Failed to register VFS callback.\n");
        free(cb);
        return;
    }
    plogk("eventfd: Subsystem initialized (fsid=%d).\n", eventfd_fsid);
}