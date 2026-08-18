/*
 *
 *      signalfd.c
 *      Signalfd file descriptor implementation
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
#include <syscall/signalfd.h>
#include <syscall/syscall.h>

static int signalfd_fsid = -1;

/* Copy a signal into the signalfd info layout */
static void signalfd_format_info(signalfd_siginfo_t *dest, int sig, const siginfo_t *source)
{
    memset(dest, 0, sizeof(*dest));
    dest->ssi_signo = (uint32_t)sig;
    if (!source) return;
    dest->ssi_errno     = source->si_errno;
    dest->ssi_code      = source->si_code;
    dest->ssi_pid       = (uint32_t)source->si_pid;
    dest->ssi_uid       = source->si_uid;
    dest->ssi_status    = source->si_status;
    dest->ssi_int       = source->si_int;
    dest->ssi_ptr       = (uint64_t)(uintptr_t)source->si_ptr;
    dest->ssi_utime     = (uint64_t)source->si_utime;
    dest->ssi_stime     = (uint64_t)source->si_stime;
    dest->ssi_addr      = (uint64_t)(uintptr_t)source->si_addr;
    dest->ssi_addr_lsb  = (uint16_t)source->si_addr_lsb;
    dest->ssi_fd        = source->si_fd;
    dest->ssi_band      = (uint32_t)source->si_band;
    dest->ssi_tid       = (uint32_t)source->si_tid;
    dest->ssi_overrun   = (uint32_t)source->si_overrun;
    dest->ssi_syscall   = source->si_syscall;
    dest->ssi_call_addr = (uint64_t)(uintptr_t)source->si_call_addr;
    dest->ssi_arch      = source->si_arch;
}

/* VFS open callback (no-op) */
static void signalfd_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

/* VFS close callback (no-op) */
static void signalfd_vfs_close(void *current)
{
    (void)current;
}

/* VFS read callback: dequeue a pending signal, blocking if needed */
static size_t signalfd_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)file;
    if (!ctx) return (size_t)-1;
    if (size < sizeof(signalfd_siginfo_t)) return (size_t)-1;

    process_t *proc = process_current();
    if (!proc) return (size_t)-1;

    for (;;) {
        spin_lock(&ctx->lock);
        sigset_t mask  = ctx->sigmask;
        bool     block = !(ctx->flags & SFD_NONBLOCK);
        spin_unlock(&ctx->lock);

        siginfo_t source;
        memset(&source, 0, sizeof(source));
        int sig = signal_dequeue_masked(proc, &mask, &source);
        if (sig) {
            signalfd_siginfo_t info;
            signalfd_format_info(&info, sig, &source);
            memcpy(addr, &info, sizeof(info));
            return sizeof(info);
        }
        if (!block) return (size_t)-1;

        /*
         * Close the check-to-sleep race: install the waiter first, then
         * recheck the process pending bitmap.
         */
        wait_queue_prepare(&ctx->wq);
        if (signal_has_pending_masked(proc, &mask)) wait_queue_wake_all(&ctx->wq);
        wait_queue_sleep();
    }
}

/* VFS poll callback: report readability when a signal is pending */
static int signalfd_vfs_poll(void *file, size_t events)
{
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)file;
    if (!ctx) return 0;

    spin_lock(&ctx->lock);
    sigset_t mask = ctx->sigmask;
    spin_unlock(&ctx->lock);
    return signal_has_pending_masked(process_current(), &mask) ? (0x001 & (int)events) : 0;
}

/* File read entry: validate size then delegate to the read callback */
static int64_t signalfd_vfs_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)flags;
    if (size < sizeof(signalfd_siginfo_t)) return -EINVAL;
    size_t ret = signalfd_vfs_read(node->handle, addr, offset, size);
    return ret == (size_t)-1 ? -EAGAIN : (int64_t)ret;
}

/* VFS free callback: release the signalfd context */
static int signalfd_vfs_free(void *handle)
{
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)handle;
    if (!ctx) return -EINVAL;
    free(ctx);
    return EOK;
}

/* Unsupported unmount callback */
static void signalfd_stub_unmount(void *root)
{
    (void)root;
}

/* Unsupported stat callback */
static int signalfd_stub_stat(void *f, vfs_node_t n)
{
    (void)f;
    (void)n;
    return EOK;
}

/* Unsupported mkdir/mkfile/link/symlink callback */
static int signalfd_stub_mk(void *p, const char *nm, vfs_node_t n)
{
    (void)p;
    (void)nm;
    (void)n;
    return -ENOSYS;
}

/* Unsupported write callback */
static size_t signalfd_stub_write(void *f, const void *a, size_t o, size_t s)
{
    (void)f;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}

/* Unsupported readlink callback */
static size_t signalfd_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}

/* Unsupported ioctl callback */
static int signalfd_stub_ioctl(void *f, size_t o, void *a)
{
    (void)f;
    (void)o;
    (void)a;
    return -ENOSYS;
}

/* Unsupported dup callback */
static vfs_node_t signalfd_stub_dup(vfs_node_t n)
{
    (void)n;
    return NULL;
}

/* Unsupported delete callback */
static int signalfd_stub_del(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -ENOSYS;
}

/* Unsupported rename callback */
static int signalfd_stub_rename(const vfs_rename_context_t *context)
{
    (void)context;
    return -ENOSYS;
}

/* Unsupported mount callback */
static int signalfd_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

/* Allocate and initialize a signalfd VFS node */
static vfs_node_t signalfd_node_create(sigset_t sigmask, int flags)
{
    if (signalfd_fsid < 0) return NULL;

    signalfd_ctx_t *ctx = calloc(1, sizeof(signalfd_ctx_t));
    if (!ctx) return NULL;

    ctx->sigmask = sigmask;
    ctx->flags   = (uint64_t)(flags & (SFD_NONBLOCK | SFD_CLOEXEC));
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

/* signalfd syscall: create a signalfd descriptor */
int sys_signalfd(int fd, const void *mask, int flags)
{
    return sys_signalfd4(fd, mask, 8, flags);
}

/* signalfd4 syscall: create or update a signalfd descriptor */
int sys_signalfd4(int fd, const void *mask, size_t sizemask, int flags)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!mask || sizemask != sizeof(uint64_t) || (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC))) return -EINVAL;

    sigset_t sigmask;
    sigemptyset(&sigmask);

    if (copy_from_user(&sigmask, mask, sizeof(sigmask))) return -EFAULT;
    sigdelset(&sigmask, SIGKILL);
    sigdelset(&sigmask, SIGSTOP);

    if (fd == -1) {
        vfs_node_t node = signalfd_node_create(sigmask, flags);
        if (!node) return -ENOMEM;

        uint64_t fd_flags = O_RDONLY;
        if (flags & SFD_CLOEXEC) fd_flags |= O_CLOEXEC;
        int newfd = process_fd_install(proc, node, fd_flags);
        if (newfd < 0) {
            vfs_close(node);
            return newfd;
        }
        return newfd;
    }

    spin_lock(&proc->fd_lock);
    process_file_t *file = NULL;
    if (fd >= 0 && fd < PROCESS_MAX_FD) {
        file = proc->fds[fd];
        if (file) process_file_get(file);
    }
    spin_unlock(&proc->fd_lock);

    if (!file || !file->node || file->node->fsid != signalfd_fsid || !file->node->handle) {
        if (file) process_file_put(file);
        return -EBADF;
    }

    signalfd_ctx_t *ctx = (signalfd_ctx_t *)file->node->handle;
    spin_lock(&ctx->lock);
    ctx->sigmask = sigmask;
    ctx->flags   = (uint64_t)(flags & (SFD_NONBLOCK | SFD_CLOEXEC));
    spin_unlock(&ctx->lock);

    if (signal_has_pending_masked(proc, &sigmask)) {
        wait_queue_wake_all(&ctx->wq);
        vfs_poll_notify(file->node, 0x001U);
    }

    process_file_put(file);
    return fd;
}

/* Wake any signalfd matching a delivered signal */
void signalfd_deliver(process_t *proc, int sig, const siginfo_t *source)
{
    if (!proc || !sig_valid(sig)) return;
    (void)source;

    spin_lock(&proc->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        process_file_t *file = proc->fds[i];
        if (!file || !file->node || !file->node->handle) continue;
        if (file->node->fsid != signalfd_fsid) continue;

        signalfd_ctx_t *ctx = (signalfd_ctx_t *)file->node->handle;

        spin_lock(&ctx->lock);
        if (!sigismember(&ctx->sigmask, sig)) {
            spin_unlock(&ctx->lock);
            continue;
        }

        spin_unlock(&ctx->lock);
        wait_queue_wake_all(&ctx->wq);
        vfs_poll_notify(file->node, 0x001U);
    }
    spin_unlock(&proc->fd_lock);
}

/* Register the signalfd filesystem callback set */
void signalfd_init(void)
{
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("signalfd: Failed to allocate callback.\n");
        return;
    }
    cb->mount     = signalfd_stub_mount;
    cb->unmount   = signalfd_stub_unmount;
    cb->open      = signalfd_vfs_open;
    cb->close     = signalfd_vfs_close;
    cb->read      = signalfd_vfs_read;
    cb->write     = signalfd_stub_write;
    cb->readlink  = signalfd_stub_readlink;
    cb->mkdir     = signalfd_stub_mk;
    cb->mkfile    = signalfd_stub_mk;
    cb->link      = signalfd_stub_mk;
    cb->symlink   = signalfd_stub_mk;
    cb->stat      = signalfd_stub_stat;
    cb->ioctl     = signalfd_stub_ioctl;
    cb->dup       = signalfd_stub_dup;
    cb->poll      = signalfd_vfs_poll;
    cb->file_read = signalfd_vfs_file_read;
    cb->free      = signalfd_vfs_free;
    cb->delete    = signalfd_stub_del;
    cb->rename    = signalfd_stub_rename;

    signalfd_fsid = vfs_regist(cb);
    if (signalfd_fsid < 0) {
        plogk("signalfd: Failed to register VFS callback.\n");
        free(cb);
        return;
    }
    plogk("signalfd: Signalfd subsystem registered (fsid=%d)\n", signalfd_fsid);
}
