/*
 *
 *      signalfd.c
 *      Signalfd file descriptor implementation
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
#include <signal.h>
#include <signalfd.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <task.h>
#include <uaccess.h>
#include <vfs.h>

static int signalfd_fsid = -1;

/* ---------- VFS callback implementations ---------- */

static void signalfd_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

static void signalfd_vfs_close(void *current)
{
    (void)current;
}

static size_t signalfd_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)file;
    if (!ctx) return (size_t)-1;
    if (size < sizeof(signalfd_siginfo_t)) return (size_t)-1;

    spin_lock(&ctx->lock);

    if (ctx->pending_count == 0) {
        if (ctx->flags & SFD_NONBLOCK) {
            spin_unlock(&ctx->lock);
            return (size_t)-1;
        }
        spin_unlock(&ctx->lock);
        wait_queue_wait(&ctx->wq);
        spin_lock(&ctx->lock);

        if (ctx->pending_count == 0) {
            spin_unlock(&ctx->lock);
            return (size_t)-1;
        }
    }

    signalfd_siginfo_t info = ctx->pending[ctx->pending_head];
    ctx->pending_head       = (ctx->pending_head + 1) % SIG_PENDING_MAX;
    ctx->pending_count--;

    spin_unlock(&ctx->lock);
    wait_queue_wake_all(&ctx->wq);

    if (copy_to_user(addr, &info, sizeof(info))) return (size_t)-1;
    return sizeof(signalfd_siginfo_t);
}

static int signalfd_vfs_poll(void *file, size_t events)
{
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)file;
    if (!ctx) return 0;

    int revents = 0;
    spin_lock(&ctx->lock);
    if (ctx->pending_count > 0) revents |= 0x001;
    revents |= 0x004;
    spin_unlock(&ctx->lock);
    return revents & (int)events;
}

static int signalfd_vfs_free(void *handle)
{
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)handle;
    if (!ctx) return -EINVAL;
    free(ctx);
    return EOK;
}

/* Generic stubs */
static void signalfd_stub_unmount(void *root)
{
    (void)root;
}
static int signalfd_stub_stat(void *f, vfs_node_t n)
{
    (void)f;
    (void)n;
    return EOK;
}
static int signalfd_stub_mk(void *p, const char *nm, vfs_node_t n)
{
    (void)p;
    (void)nm;
    (void)n;
    return -ENOSYS;
}
static size_t signalfd_stub_write(void *f, const void *a, size_t o, size_t s)
{
    (void)f;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}
static size_t signalfd_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}
static int signalfd_stub_ioctl(void *f, size_t o, void *a)
{
    (void)f;
    (void)o;
    (void)a;
    return -ENOSYS;
}
static vfs_node_t signalfd_stub_dup(vfs_node_t n)
{
    (void)n;
    return NULL;
}
static int signalfd_stub_del(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -ENOSYS;
}
static int signalfd_stub_rename(void *c, const char *nm)
{
    (void)c;
    (void)nm;
    return -ENOSYS;
}
static int signalfd_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

/* ---------- Public API ---------- */

static vfs_node_t signalfd_node_create(sigset_t sigmask, int flags)
{
    if (signalfd_fsid < 0) return NULL;

    signalfd_ctx_t *ctx = calloc(1, sizeof(signalfd_ctx_t));
    if (!ctx) return NULL;

    ctx->sigmask       = sigmask;
    ctx->flags         = (uint64_t)(flags & (SFD_NONBLOCK | SFD_CLOEXEC));
    ctx->pending_head  = 0;
    ctx->pending_tail  = 0;
    ctx->pending_count = 0;
    wait_queue_init(&ctx->wq);

    vfs_node_t node = vfs_node_alloc(NULL, "[signalfd]");
    if (!node) {
        free(ctx);
        return NULL;
    }

    node->type   = file_stream;
    node->handle = ctx;
    node->fsid   = signalfd_fsid;
    node->size   = 0;
    node->mode   = O_RDONLY;

    return node;
}

int sys_signalfd(int fd, const void *mask, int flags)
{
    return sys_signalfd4(fd, mask, 8, flags);
}

int sys_signalfd4(int fd, const void *mask, size_t sizemask, int flags)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    sigset_t sigmask;
    sigemptyset(&sigmask);

    if (mask) {
        if (sizemask >= sizeof(sigset_t)) {
            if (copy_from_user(&sigmask, mask, sizeof(sigset_t))) return -EFAULT;
        } else {
            uint32_t mask32 = 0;
            if (copy_from_user(&mask32, mask, sizeof(uint32_t))) return -EFAULT;
            sigmask = (sigset_t)mask32;
        }
    }

    if (fd == -1) {
        vfs_node_t node = signalfd_node_create(sigmask, flags);
        if (!node) return -ENOMEM;

        int newfd = process_fd_install(proc, node, O_RDONLY);
        if (newfd < 0) {
            signalfd_vfs_free(node->handle);
            vfs_free(node);
            return newfd;
        }
        return newfd;
    }

    spin_lock(&proc->fd_lock);
    process_file_t *file = NULL;
    if (fd >= 0 && fd < PROCESS_MAX_FD) {
        file = proc->fds[fd];
        if (file) {
            spin_lock(&file->lock);
            file->refcount++;
            spin_unlock(&file->lock);
        }
    }
    spin_unlock(&proc->fd_lock);

    if (!file || !file->node || !file->node->handle) {
        if (file) process_file_put(file);
        return -EBADF;
    }

    signalfd_ctx_t *ctx = (signalfd_ctx_t *)file->node->handle;
    spin_lock(&ctx->lock);
    ctx->sigmask = sigmask;
    ctx->flags   = (uint64_t)(flags & (SFD_NONBLOCK | SFD_CLOEXEC));
    spin_unlock(&ctx->lock);

    process_file_put(file);
    return fd;
}

void signalfd_deliver(process_t *proc, int sig)
{
    if (!proc || !sig_valid(sig)) return;

    spin_lock(&proc->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        process_file_t *file = proc->fds[i];
        if (!file || !file->node || !file->node->handle) continue;
        if (file->node->type != file_stream) continue;

        signalfd_ctx_t *ctx = (signalfd_ctx_t *)file->node->handle;

        spin_lock(&ctx->lock);
        if (!sigismember(&ctx->sigmask, sig)) {
            spin_unlock(&ctx->lock);
            continue;
        }

        if (ctx->pending_count >= SIG_PENDING_MAX) {
            spin_unlock(&ctx->lock);
            continue;
        }

        signalfd_siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.ssi_signo = (uint32_t)sig;
        info.ssi_pid   = (uint32_t)(proc->task ? proc->task->pid : 0);
        info.ssi_uid   = proc->uid;

        ctx->pending[ctx->pending_tail] = info;
        ctx->pending_tail               = (ctx->pending_tail + 1) % SIG_PENDING_MAX;
        ctx->pending_count++;

        spin_unlock(&ctx->lock);
        wait_queue_wake_all(&ctx->wq);
    }
    spin_unlock(&proc->fd_lock);
}

void signalfd_init(void)
{
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("signalfd: Failed to allocate callback.\n");
        return;
    }
    cb->mount    = signalfd_stub_mount;
    cb->unmount  = signalfd_stub_unmount;
    cb->open     = signalfd_vfs_open;
    cb->close    = signalfd_vfs_close;
    cb->read     = signalfd_vfs_read;
    cb->write    = signalfd_stub_write;
    cb->readlink = signalfd_stub_readlink;
    cb->mkdir    = signalfd_stub_mk;
    cb->mkfile   = signalfd_stub_mk;
    cb->link     = signalfd_stub_mk;
    cb->symlink  = signalfd_stub_mk;
    cb->stat     = signalfd_stub_stat;
    cb->ioctl    = signalfd_stub_ioctl;
    cb->dup      = signalfd_stub_dup;
    cb->poll     = signalfd_vfs_poll;
    cb->free     = signalfd_vfs_free;
    cb->delete   = signalfd_stub_del;
    cb->rename   = signalfd_stub_rename;

    signalfd_fsid = vfs_regist(cb);
    if (signalfd_fsid < 0) {
        plogk("signalfd: Failed to register VFS callback.\n");
        free(cb);
        return;
    }
    plogk("signalfd: Subsystem initialized (fsid=%d).\n", signalfd_fsid);
}