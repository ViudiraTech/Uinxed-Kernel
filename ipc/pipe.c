/*
 *
 *      pipe.c
 *      Pipe and FIFO (named pipe) implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <ipc/pipe.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>
#include <syscall/syscall.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#ifndef PIPE_BUF_SIZE
#    define PIPE_BUF_SIZE 65536
#endif
#define PIPE_ATOMIC_SIZE  4096
#define PIPE_DEFAULT_MODE 0644

/* poll event bits */
#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010

/* ------------------------------------------------------------------ */
/*  Pipe ring buffer structure                                          */
/* ------------------------------------------------------------------ */

typedef struct pipe_ring {
        uint8_t     *buf;
        uint32_t     head;
        uint32_t     tail;
        uint32_t     size;
        uint32_t     capacity;
        uint32_t     readers;
        uint32_t     writers;
        uint32_t     read_waiters;
        uint32_t     write_waiters;
        uint32_t     write_wake_threshold;
        int          closed;
        spinlock_t   lock;
        wait_queue_t read_wq;
        wait_queue_t write_wq;
} pipe_ring_t;

/*
 * One endpoint per open-file description.  fork(2) and dup(2) share the
 * process_file object, so the endpoint is released only after the last
 * descriptor referring to that description is closed.
 */
typedef struct pipe_endpoint {
        pipe_ring_t *ring;
        bool         readable;
        bool         writable;
} pipe_endpoint_t;

/* ------------------------------------------------------------------ */
/*  Static VFS filesystem ID                                            */
/* ------------------------------------------------------------------ */

static int pipe_fsid = -1;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t pipe_ring_readable(const pipe_ring_t *ring)
{
    return ring->size;
}

static uint32_t pipe_ring_writable(const pipe_ring_t *ring)
{
    return ring->capacity - ring->size;
}

static void pipe_ring_consume(pipe_ring_t *ring, uint32_t count)
{
    ring->tail = (ring->tail + count) % ring->capacity;
    ring->size -= count;
}

static void pipe_ring_produce(pipe_ring_t *ring, uint32_t count)
{
    ring->head = (ring->head + count) % ring->capacity;
    ring->size += count;
}

/* Copy data from ring buffer to linear buffer, handling wraparound */
static uint32_t pipe_ring_copy_out(pipe_ring_t *ring, uint8_t *dst, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->tail;

    if (first_chunk > count) first_chunk = count;
    memcpy(dst, ring->buf + ring->tail, first_chunk);

    if (count > first_chunk) memcpy(dst + first_chunk, ring->buf, count - first_chunk);
    return count;
}

/* Copy data from linear buffer to ring buffer, handling wraparound */
static uint32_t pipe_ring_copy_in(pipe_ring_t *ring, const uint8_t *src, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->head;

    if (first_chunk > count) first_chunk = count;
    memcpy(ring->buf + ring->head, src, first_chunk);

    if (count > first_chunk) memcpy(ring->buf, src + first_chunk, count - first_chunk);
    return count;
}

static int pipe_ring_copy_out_user(pipe_ring_t *ring, process_t *proc, uint8_t *dst, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->tail;
    if (first_chunk > count) first_chunk = count;

    if (copy_to_user_process_nofault(proc, dst, ring->buf + ring->tail, first_chunk)) return -EFAULT;
    if (count > first_chunk && copy_to_user_process_nofault(proc, dst + first_chunk, ring->buf, count - first_chunk)) return -EFAULT;
    return EOK;
}

static int pipe_ring_copy_in_user(pipe_ring_t *ring, process_t *proc, const uint8_t *src, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->head;
    if (first_chunk > count) first_chunk = count;

    if (copy_from_user_process_nofault(proc, ring->buf + ring->head, src, first_chunk)) return -EFAULT;
    if (count > first_chunk && copy_from_user_process_nofault(proc, ring->buf, src + first_chunk, count - first_chunk)) return -EFAULT;
    return EOK;
}

static pipe_ring_t *pipe_ring_alloc(void)
{
    pipe_ring_t *ring = calloc(1, sizeof(pipe_ring_t));
    if (!ring) {
        plogk("pipe: Ring allocation failed.\n");
        return NULL;
    }

    ring->buf = malloc(PIPE_BUF_SIZE);
    if (!ring->buf) {
        plogk("pipe: Ring buffer allocation failed (%d bytes)\n", PIPE_BUF_SIZE);
        free(ring);
        return NULL;
    }
    ring->capacity = PIPE_BUF_SIZE;
    ring->head     = 0;
    ring->tail     = 0;
    ring->size     = 0;
    ring->readers  = 0;
    ring->writers  = 0;
    ring->closed   = 0;
    wait_queue_init(&ring->read_wq);
    wait_queue_init(&ring->write_wq);

    return ring;
}

static void pipe_ring_free(pipe_ring_t *ring)
{
    if (!ring) return;
    if (ring->buf) free(ring->buf);
    free(ring);
}

static bool pipe_signal_pending(void)
{
    process_t *proc = process_current();
    return proc && signal_has_pending(&proc->signal);
}

static void pipe_raise_sigpipe(void)
{
    process_t *proc = process_current();
    if (!proc) return;
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = SIGPIPE;
    info.si_code  = SI_KERNEL;
    (void)signal_send(proc, SIGPIPE, &info);
}

/* ------------------------------------------------------------------ */
/*  VFS callback: open                                                  */
/* ------------------------------------------------------------------ */

static void pipe_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;

    if (!node) return;

    /*
     * For FIFO (named pipe) nodes created by sys_mknod / sys_mkfifo,
     * the handle is NULL on first open.  Create the pipe ring here.
     * For anonymous pipes created by sys_pipe, the handle is already
     * set before the node enters the VFS, so this path is a no-op.
     */
    if (!node->handle) {
        pipe_ring_t *ring = pipe_ring_alloc();
        if (!ring) {
            plogk("pipe: FIFO %s open failed (ring allocation)\n", name ? name : "?");
            return;
        }
        node->handle = ring;
    }
}

/* ------------------------------------------------------------------ */
/*  VFS callback: close                                                 */
/*                                                                      */
/*  NOTE: The VFS layer calls this callback only when node->refcount    */
/*  reaches zero, i.e. when the *last* file descriptor referencing      */
/*  this pipe node is closed.  For anonymous pipes this means both      */
/*  the read and write ends have been closed.                           */
/*                                                                      */
/*  We wake all blocked readers and writers here so that no task        */
/*  remains stuck on a pipe that will never be serviced again.          */
/* ------------------------------------------------------------------ */

static void pipe_vfs_close(void *current)
{
    pipe_ring_t *ring = (pipe_ring_t *)current;
    if (!ring) return;

    spin_lock(&ring->lock);
    ring->closed  = 1;
    ring->readers = 0;
    ring->writers = 0;
    spin_unlock(&ring->lock);

    wait_queue_wake_all(&ring->read_wq);
    wait_queue_wake_all(&ring->write_wq);
}

static int pipe_file_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    if (!node || !private_data) return -EINVAL;
    pipe_ring_t *ring = node->handle;
    if (!ring) return -EIO;

    uint64_t access = flags & O_ACCMODE;
    if (access != O_RDONLY && access != O_WRONLY && access != O_RDWR) return -EINVAL;

    pipe_endpoint_t *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) {
        plogk("pipe: Endpoint allocation failed.\n");
        return -ENOMEM;
    }
    endpoint->ring     = ring;
    endpoint->readable = access != O_WRONLY;
    endpoint->writable = access != O_RDONLY;

    spin_lock(&ring->lock);
    if (ring->closed) {
        spin_unlock(&ring->lock);
        free(endpoint);
        return -EIO;
    }
    if (endpoint->readable) ring->readers++;
    if (endpoint->writable) ring->writers++;
    spin_unlock(&ring->lock);
    wait_queue_wake_all(&ring->read_wq);
    wait_queue_wake_all(&ring->write_wq);

    /*
     * Anonymous pipes are born with both endpoints and must not block while
     * pipe2(2) installs them sequentially.  Named FIFOs follow open(2)'s
     * rendezvous rules.
     */
    if (node->parent && !endpoint->readable && endpoint->writable && (flags & O_NONBLOCK)) {
        spin_lock(&ring->lock);
        if (ring->readers == 0) {
            ring->writers--;
            spin_unlock(&ring->lock);
            free(endpoint);
            return -ENXIO;
        }
        spin_unlock(&ring->lock);
    } else if (node->parent && !(flags & O_NONBLOCK) && access != O_RDWR) {
        spin_lock(&ring->lock);
        while (!ring->closed && (endpoint->readable ? ring->writers == 0 : ring->readers == 0)) {
            if (pipe_signal_pending()) {
                if (endpoint->readable) ring->readers--;
                if (endpoint->writable) ring->writers--;
                spin_unlock(&ring->lock);
                free(endpoint);
                return -EINTR;
            }
            wait_queue_prepare(endpoint->readable ? &ring->read_wq : &ring->write_wq);
            spin_unlock(&ring->lock);
            wait_queue_sleep();
            spin_lock(&ring->lock);
        }
        if (ring->closed) {
            if (endpoint->readable) ring->readers--;
            if (endpoint->writable) ring->writers--;
            spin_unlock(&ring->lock);
            free(endpoint);
            return -EIO;
        }
        spin_unlock(&ring->lock);
    }

    *private_data = endpoint;
    return EOK;
}

static void pipe_file_release(vfs_node_t node, void *private_data)
{
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint) return;
    pipe_ring_t *ring        = endpoint->ring;
    bool         last_reader = false;
    bool         last_writer = false;

    spin_lock(&ring->lock);
    if (endpoint->readable && ring->readers) last_reader = --ring->readers == 0;
    if (endpoint->writable && ring->writers) last_writer = --ring->writers == 0;
    spin_unlock(&ring->lock);

    if (last_reader) {
        wait_queue_wake_all(&ring->write_wq);
        vfs_poll_notify(node, POLLERR);
    }
    if (last_writer) {
        wait_queue_wake_all(&ring->read_wq);
        vfs_poll_notify(node, POLLHUP);
    }
    free(endpoint);
}

/* ------------------------------------------------------------------ */
/*  VFS callback: read                                                  */
/* ------------------------------------------------------------------ */

static int64_t pipe_read_common(vfs_node_t node, pipe_ring_t *ring, uint64_t flags, void *addr, size_t size)
{
    if (!ring || (!addr && size)) return -EINVAL;
    if (!size) return 0;

    spin_lock(&ring->lock);

    /*
     * Spin until data is available, the pipe is closed, or all
     * writers have gone away.  On each wakeup we re-check the
     * condition under the lock.
     */
    while (pipe_ring_readable(ring) == 0) {
        if (ring->closed) {
            spin_unlock(&ring->lock);
            return 0;
        }
        if (ring->writers == 0) {
            spin_unlock(&ring->lock);
            return 0;
        }
        if (flags & O_NONBLOCK) {
            spin_unlock(&ring->lock);
            return -EAGAIN;
        }
        if (pipe_signal_pending()) {
            spin_unlock(&ring->lock);
            return -EINTR;
        }
        /* prepare wait under lock, then block, re-acquire on wakeup */
        ring->read_waiters++;
        wait_queue_prepare(&ring->read_wq);
        spin_unlock(&ring->lock);
        wait_queue_sleep();
        spin_lock(&ring->lock);
        if (ring->read_waiters) ring->read_waiters--;
    }

    uint32_t avail = pipe_ring_readable(ring);
    uint32_t chunk = (size < avail) ? (uint32_t)size : avail;

    pipe_ring_copy_out(ring, (uint8_t *)addr, chunk);
    pipe_ring_consume(ring, chunk);
    bool wake_writers = ring->write_waiters != 0 && pipe_ring_writable(ring) >= ring->write_wake_threshold;

    spin_unlock(&ring->lock);

    /* Wake writers that may be waiting for buffer space */
    /*
     * Linux uses an exclusive writer wait: freeing one pipe-buffer slot
     * wakes one writer, which may cascade to the next writer if space
     * remains.  Waking the whole queue creates avoidable scheduler/IPI work.
     */
    if (wake_writers) wait_queue_wake_one_sync(&ring->write_wq);
    if (node) vfs_poll_notify(node, POLLOUT);

    return (int64_t)chunk;
}

static size_t pipe_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    int64_t result = pipe_read_common(NULL, file, 0, addr, size);
    return result < 0 ? (size_t)-1 : (size_t)result;
}

static int64_t pipe_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->readable) return -EBADF;
    return pipe_read_common(node, endpoint->ring, flags, addr, size);
}

static int64_t pipe_file_read_user(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->readable) return -EBADF;
    if (!user_range_ok(addr, size)) return -EFAULT;
    if (!size) return 0;

    pipe_ring_t *ring = endpoint->ring;
    for (;;) {
        spin_lock(&ring->lock);
        while (pipe_ring_readable(ring) == 0) {
            if (ring->closed || ring->writers == 0) {
                spin_unlock(&ring->lock);
                return 0;
            }
            if (flags & O_NONBLOCK) {
                spin_unlock(&ring->lock);
                return -EAGAIN;
            }
            if (pipe_signal_pending()) {
                spin_unlock(&ring->lock);
                return -EINTR;
            }
            ring->read_waiters++;
            wait_queue_prepare(&ring->read_wq);
            spin_unlock(&ring->lock);
            wait_queue_sleep();
            spin_lock(&ring->lock);
            if (ring->read_waiters) ring->read_waiters--;
        }

        uint32_t avail = pipe_ring_readable(ring);
        uint32_t chunk = size < avail ? (uint32_t)size : avail;
        if (pipe_ring_copy_out_user(ring, proc, addr, chunk)) {
            spin_unlock(&ring->lock);
            /*
             * Fault/COW resolution can allocate or sleep, so it is done only
             * after dropping the ring lock.  Nothing has been consumed yet.
             */
            if (!user_access_ok_process(proc, addr, chunk, 1)) return -EFAULT;
            continue;
        }

        pipe_ring_consume(ring, chunk);
        bool wake_writers = ring->write_waiters != 0 && pipe_ring_writable(ring) >= ring->write_wake_threshold;
        spin_unlock(&ring->lock);

        if (wake_writers) wait_queue_wake_one_sync(&ring->write_wq);
        if (node) vfs_poll_notify(node, POLLOUT);
        return (int64_t)chunk;
    }
}

/* ------------------------------------------------------------------ */
/*  VFS callback: write                                                 */
/* ------------------------------------------------------------------ */

static int64_t pipe_write_common(vfs_node_t node, pipe_ring_t *ring, uint64_t flags, const void *addr, size_t size)
{
    if (!ring || (!addr && size)) return -EINVAL;
    if (!size) return 0;

    size_t         total_written = 0;
    const uint8_t *src           = (const uint8_t *)addr;

    while (total_written < size) {
        spin_lock(&ring->lock);

        if (ring->closed || ring->readers == 0) {
            spin_unlock(&ring->lock);
            if (!total_written) pipe_raise_sigpipe();
            return total_written ? (int64_t)total_written : -EPIPE;
        }

        size_t   remaining      = size - total_written;
        bool     atomic         = size <= PIPE_ATOMIC_SIZE;
        uint32_t wake_threshold = atomic ? (uint32_t)remaining : (uint32_t)(remaining < PIPE_ATOMIC_SIZE ? remaining : PIPE_ATOMIC_SIZE);

        /*
         * PIPE_BUF-sized writes remain atomic.  Larger writes may consume
         * whatever space is already available (including O_NONBLOCK partial
         * writes), but once the pipe is full we wait for a useful batch of
         * space instead of bouncing producer/consumer every 512 bytes.
         */
        while (atomic ? pipe_ring_writable(ring) < (uint32_t)remaining : pipe_ring_writable(ring) == 0) {
            if (ring->closed || ring->readers == 0) {
                spin_unlock(&ring->lock);
                if (!total_written) pipe_raise_sigpipe();
                return total_written ? (int64_t)total_written : -EPIPE;
            }
            if (flags & O_NONBLOCK) {
                spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EAGAIN;
            }
            if (pipe_signal_pending()) {
                spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EINTR;
            }
            if (!ring->write_waiters || wake_threshold < ring->write_wake_threshold) ring->write_wake_threshold = wake_threshold;
            ring->write_waiters++;
            wait_queue_prepare(&ring->write_wq);
            spin_unlock(&ring->lock);
            wait_queue_sleep();
            spin_lock(&ring->lock);
            if (ring->write_waiters) ring->write_waiters--;
            if (!ring->write_waiters) ring->write_wake_threshold = 0;
        }

        uint32_t writable = pipe_ring_writable(ring);
        uint32_t chunk    = (uint32_t)(remaining < writable ? remaining : writable);

        pipe_ring_copy_in(ring, src + total_written, chunk);
        pipe_ring_produce(ring, chunk);
        total_written += chunk;
        bool wake_readers = ring->read_waiters != 0;

        spin_unlock(&ring->lock);

        /* Wake readers that may be waiting for data */
        if (wake_readers) wait_queue_wake_one_sync(&ring->read_wq);
        if (node) vfs_poll_notify(node, POLLIN);
    }

    return (int64_t)total_written;
}

static size_t pipe_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    int64_t result = pipe_write_common(NULL, file, 0, addr, size);
    return result < 0 ? (size_t)-1 : (size_t)result;
}

static int64_t pipe_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->writable) return -EBADF;
    return pipe_write_common(node, endpoint->ring, flags, addr, size);
}

static int64_t pipe_file_write_user(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size,
                                    process_t *proc)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->writable) return -EBADF;
    if (!user_range_ok(addr, size)) return -EFAULT;
    if (!size) return 0;

    pipe_ring_t *ring          = endpoint->ring;
    size_t       total_written = 0;

    while (total_written < size) {
        spin_lock(&ring->lock);
        if (ring->closed || ring->readers == 0) {
            spin_unlock(&ring->lock);
            if (!total_written) pipe_raise_sigpipe();
            return total_written ? (int64_t)total_written : -EPIPE;
        }

        size_t   remaining      = size - total_written;
        bool     atomic         = size <= PIPE_ATOMIC_SIZE;
        uint32_t wake_threshold = atomic ? (uint32_t)remaining : (uint32_t)(remaining < PIPE_ATOMIC_SIZE ? remaining : PIPE_ATOMIC_SIZE);

        while (atomic ? pipe_ring_writable(ring) < (uint32_t)remaining : pipe_ring_writable(ring) == 0) {
            if (ring->closed || ring->readers == 0) {
                spin_unlock(&ring->lock);
                if (!total_written) pipe_raise_sigpipe();
                return total_written ? (int64_t)total_written : -EPIPE;
            }
            if (flags & O_NONBLOCK) {
                spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EAGAIN;
            }
            if (pipe_signal_pending()) {
                spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EINTR;
            }
            if (!ring->write_waiters || wake_threshold < ring->write_wake_threshold) ring->write_wake_threshold = wake_threshold;
            ring->write_waiters++;
            wait_queue_prepare(&ring->write_wq);
            spin_unlock(&ring->lock);
            wait_queue_sleep();
            spin_lock(&ring->lock);
            if (ring->write_waiters) ring->write_waiters--;
            if (!ring->write_waiters) ring->write_wake_threshold = 0;
        }

        uint32_t       writable = pipe_ring_writable(ring);
        uint32_t       chunk    = (uint32_t)(remaining < writable ? remaining : writable);
        const uint8_t *src      = (const uint8_t *)addr + total_written;
        if (pipe_ring_copy_in_user(ring, proc, src, chunk)) {
            spin_unlock(&ring->lock);
            if (!user_access_ok_process(proc, src, chunk, 0)) return total_written ? (int64_t)total_written : -EFAULT;
            continue;
        }

        pipe_ring_produce(ring, chunk);
        total_written += chunk;
        bool wake_readers = ring->read_waiters != 0;
        spin_unlock(&ring->lock);

        if (wake_readers) wait_queue_wake_one_sync(&ring->read_wq);
        if (node) vfs_poll_notify(node, POLLIN);
    }

    return (int64_t)total_written;
}

/* ------------------------------------------------------------------ */
/*  VFS callback: poll                                                  */
/* ------------------------------------------------------------------ */

static int pipe_vfs_poll(void *file, size_t events)
{
    pipe_ring_t *ring = (pipe_ring_t *)file;
    if (!ring) return 0;

    int revents = 0;

    spin_lock(&ring->lock);

    if (pipe_ring_readable(ring) > 0) revents |= POLLIN;
    if (ring->writers == 0 || ring->closed) revents |= POLLHUP;
    if (pipe_ring_writable(ring) > 0 && ring->readers > 0 && !ring->closed) revents |= POLLOUT;

    spin_unlock(&ring->lock);

    return (revents & (int)events) | (revents & (POLLERR | POLLHUP));
}

static int pipe_file_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events)
{
    (void)node;
    (void)flags;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint) return POLLERR;
    pipe_ring_t *ring    = endpoint->ring;
    int          revents = 0;

    spin_lock(&ring->lock);
    if (endpoint->readable) {
        if (pipe_ring_readable(ring)) revents |= POLLIN;
        if (!ring->writers || ring->closed) revents |= POLLHUP;
    }
    if (endpoint->writable) {
        if (!ring->readers || ring->closed)
            revents |= POLLERR;
        else if (pipe_ring_writable(ring))
            revents |= POLLOUT;
    }
    spin_unlock(&ring->lock);
    return (revents & (int)events) | (revents & (POLLERR | POLLHUP));
}

/* ------------------------------------------------------------------ */
/*  VFS callback: free (release handle resources)                       */
/* ------------------------------------------------------------------ */

static int pipe_vfs_free(void *handle)
{
    pipe_ring_t *ring = (pipe_ring_t *)handle;
    if (!ring) return -EINVAL;

    pipe_ring_free(ring);
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  VFS callback: stat                                                  */
/* ------------------------------------------------------------------ */

static int pipe_vfs_stat(void *file, vfs_node_t node)
{
    (void)file;
    if (!node) return -EINVAL;

    pipe_ring_t *ring = (pipe_ring_t *)node->handle;
    if (ring) node->size = ring->size;
    node->type |= file_pipe;
    node->mode = PIPE_DEFAULT_MODE;
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  VFS callback stubs (unused operations for pipe)                     */
/* ------------------------------------------------------------------ */

static int pipe_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

static void pipe_stub_unmount(void *root)
{
    (void)root;
}

static size_t pipe_stub_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)addr;
    (void)offset;
    (void)size;
    return (size_t)-1;
}

static int pipe_stub_mk(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -ENOSYS;
}

static int pipe_stub_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOSYS;
}

static vfs_node_t pipe_stub_dup(vfs_node_t node)
{
    (void)node;
    return NULL;
}

static int pipe_stub_del(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -ENOSYS;
}

static int pipe_stub_rename(void *current, const char *new_name)
{
    (void)current;
    (void)new_name;
    return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/*  Pipe node creation (shared by sys_pipe and sys_pipe2)               */
/* ------------------------------------------------------------------ */

static vfs_node_t pipe_node_create(pipe_ring_t *ring)
{
    if (pipe_fsid < 0) return NULL;

    vfs_node_t node = vfs_node_alloc(NULL, "[pipe]");
    if (!node) return NULL;

    node->type   = file_pipe;
    node->handle = ring;
    node->fsid   = pipe_fsid;
    node->size   = 0;
    node->mode   = PIPE_DEFAULT_MODE;

    return node;
}

/* ------------------------------------------------------------------ */
/*  Syscall: pipe / pipe2                                               */
/* ------------------------------------------------------------------ */

/* Forward declaration */
int64_t sys_pipe2(int pipefd[2], int flags);

int64_t sys_pipe(int pipefd[2])
{
    return sys_pipe2(pipefd, 0);
}

int64_t sys_pipe2(int pipefd[2], int flags)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    if (!pipefd) return -EFAULT;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;

    /* Validate user pointer readability */
    if (!user_access_ok(pipefd, 2 * sizeof(int), 1)) return -EFAULT;

    /* Allocate the pipe ring buffer */
    pipe_ring_t *ring = pipe_ring_alloc();
    if (!ring) return -ENOMEM;

    /* Create the VFS node */
    vfs_node_t node = pipe_node_create(ring);
    if (!node) {
        plogk("pipe: Sys_pipe2 node creation failed.\n");
        pipe_ring_free(ring);
        return -ENOMEM;
    }

    /*
     * Hold a construction reference.  Each successfully installed open-file
     * description consumes one additional node reference.
     */
    (void)vfs_node_retain(node);

    /*
     * Build fd flags: O_RDONLY for read, O_WRONLY for write,
     * plus O_CLOEXEC and O_NONBLOCK from the flags argument.
     */
    uint64_t read_flags  = O_RDONLY | (flags & (O_CLOEXEC | O_NONBLOCK));
    uint64_t write_flags = O_WRONLY | (flags & (O_CLOEXEC | O_NONBLOCK));

    /* Install read-end fd */
    (void)vfs_node_retain(node);
    int fd_read = process_fd_install(proc, node, read_flags);
    if (fd_read < 0) {
        vfs_close(node);
        vfs_close(node);
        return fd_read;
    }

    /* Install write-end fd */
    (void)vfs_node_retain(node);
    int fd_write = process_fd_install(proc, node, write_flags);
    if (fd_write < 0) {
        vfs_close(node);
        process_fd_close(proc, fd_read);
        vfs_close(node);
        return fd_write;
    }

    /* Only the two endpoint descriptions own the node from here on. */
    vfs_close(node);

    /* Copy the two fds out to user space */
    int fds[2] = {fd_read, fd_write};
    if (copy_to_user(pipefd, fds, sizeof(fds))) {
        process_fd_close(proc, fd_read);
        process_fd_close(proc, fd_write);
        return -EFAULT;
    }

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mknod / mkfifo (FIFO / named pipe)                         */
/* ------------------------------------------------------------------ */

static int64_t sys_mknod(const char *path, uint32_t mode, uint64_t dev)
{
    (void)dev;

    if (!path) return -EFAULT;

    /* Only support FIFO creation through mknod */
    if ((mode & 0170000) != 0010000) return -EINVAL;

    /* Allocate a pipe ring for the FIFO (will be filled on open) */
    /* Actually, for FIFO we defer ring creation to the VFS open callback. */
    /* Here we just create the VFS node with type file_pipe. */

    /*
     * Use vfs_mkfile to create the node, then override its type and fsid.
     * We need to manually construct the FIFO node because vfs_mkfile
     * delegates to the parent filesystem's mkfile callback, which would
     * treat it as a regular file.
     */
    char path_copy[256];
    int  copied = strncpy_from_user(path_copy, path, sizeof(path_copy));
    if (copied < 0) return copied;

    if (path_copy[0] != '/') return -EINVAL;

    /* Find the last '/' to separate parent path from filename */
    char      *fullpath  = path_copy;
    char      *lastslash = strrchr(fullpath, '/');
    char      *filename;
    vfs_node_t parent;

    if (lastslash == fullpath) {
        /* path is "/filename" */
        filename = fullpath + 1;
        parent   = rootdir;
    } else if (lastslash) {
        *lastslash = '\0';
        filename   = lastslash + 1;
        parent     = vfs_open(fullpath);
    } else {
        return -EINVAL;
    }

    if (!parent || !(parent->type & file_dir)) {
        if (parent && parent != rootdir) vfs_close(parent);
        return -ENOENT;
    }

    /* Check for existing node with same name */
    if (vfs_do_search(parent, filename)) {
        if (parent != rootdir) vfs_close(parent);
        return -EEXIST;
    }

    /* Create the child node */
    vfs_node_t node = vfs_node_alloc(parent, filename);
    if (!node) {
        if (parent != rootdir) vfs_close(parent);
        return -ENOMEM;
    }

    node->type        = file_pipe;
    node->fsid        = pipe_fsid;
    node->mode        = mode & 07777;
    node->dev         = dev;
    node->rdev        = dev;
    node->handle      = NULL; // ring created in VFS open callback
    node->permissions = mode & 07777;

    if (parent != rootdir) vfs_close(parent);

    return EOK;
}

static int64_t sys_mkfifo(const char *path, uint32_t mode)
{
    return sys_mknod(path, 0010000 | (mode & 07777), 0);
}

/* ------------------------------------------------------------------ */
/*  FIFO open helper                                                    */
/*                                                                      */
/*  Called by the syscall layer after vfs_open() has completed for a    */
/*  FIFO node.  This function blocks the caller until the other end     */
/*  of the FIFO is also opened, unless O_NONBLOCK was specified.        */
/*                                                                      */
/*  Parameters:                                                         */
/*    node  - the FIFO vfs node (must have type file_pipe)              */
/*    flags - open flags (O_RDONLY / O_WRONLY / O_NONBLOCK)            */
/*                                                                      */
/*  Returns:                                                            */
/*    EOK    - both ends are now open                                   */
/*    -EAGAIN - O_NONBLOCK was set and the other end is not open yet    */
/*    -ENXIO  - O_NONBLOCK | O_WRONLY and no reader exists              */
/* ------------------------------------------------------------------ */

static int pipe_open(vfs_node_t node, uint64_t flags)
{
    if (!node || !(node->type & file_pipe)) return -EINVAL;

    pipe_ring_t *ring = (pipe_ring_t *)node->handle;
    if (!ring) return -EINVAL;

    int is_read  = ((flags & O_ACCMODE) == O_RDONLY);
    int is_write = ((flags & O_ACCMODE) == O_WRONLY);

    spin_lock(&ring->lock);

    if (is_read) {
        ring->readers++;
    } else if (is_write) {
        ring->writers++;
    }

    /*
     * If both ends are now open, we are done.
     */
    if (ring->readers > 0 && ring->writers > 0) {
        spin_unlock(&ring->lock);
        return EOK;
    }

    /*
     * O_NONBLOCK: return immediately.
     */
    if (flags & O_NONBLOCK) {
        if (is_write && ring->readers == 0) {
            /* Opening write-only with no readers and O_NONBLOCK ?ENXIO */
            ring->writers--;
            spin_unlock(&ring->lock);
            return -ENXIO;
        }
        spin_unlock(&ring->lock);
        return -EAGAIN;
    }

    /*
     * Block until the other end opens or the pipe is closed.
     */
    while (ring->readers == 0 || ring->writers == 0) {
        if (ring->closed) {
            if (is_read) ring->readers--;
            if (is_write) ring->writers--;
            spin_unlock(&ring->lock);
            return -EIO;
        }
        /*
         * Prepare wait under lock, then block. The other end's open
         * will wake us via wait_queue_wake_all.
         */
        if (is_read) {
            wait_queue_prepare(&ring->read_wq);
        } else {
            wait_queue_prepare(&ring->write_wq);
        }
        spin_unlock(&ring->lock);
        wait_queue_sleep();
        spin_lock(&ring->lock);
    }

    spin_unlock(&ring->lock);
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                      */
/* ------------------------------------------------------------------ */

void pipe_init(void)
{
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("pipe: Failed to allocate VFS callback structure.\n");
        return;
    }

    cb->mount           = pipe_stub_mount;
    cb->unmount         = pipe_stub_unmount;
    cb->open            = pipe_vfs_open;
    cb->close           = pipe_vfs_close;
    cb->read            = pipe_vfs_read;
    cb->write           = pipe_vfs_write;
    cb->readlink        = pipe_stub_readlink;
    cb->mkdir           = pipe_stub_mk;
    cb->mkfile          = pipe_stub_mk;
    cb->link            = pipe_stub_mk;
    cb->symlink         = pipe_stub_mk;
    cb->stat            = pipe_vfs_stat;
    cb->ioctl           = pipe_stub_ioctl;
    cb->dup             = pipe_stub_dup;
    cb->poll            = pipe_vfs_poll;
    cb->free            = pipe_vfs_free;
    cb->delete          = pipe_stub_del;
    cb->rename          = pipe_stub_rename;
    cb->file_open       = pipe_file_open;
    cb->file_release    = pipe_file_release;
    cb->file_read       = pipe_file_read;
    cb->file_write      = pipe_file_write;
    cb->file_read_user  = pipe_file_read_user;
    cb->file_write_user = pipe_file_write_user;
    cb->file_poll       = pipe_file_poll;

    pipe_fsid = vfs_regist(cb);
    if (pipe_fsid < 0) {
        plogk("pipe: Failed to register VFS callback (err=%d)\n", pipe_fsid);
        free(cb);
        return;
    }

    plogk("pipe: Pipe subsystem registered (fsid=%d, buffer=%d bytes)\n", pipe_fsid, PIPE_BUF_SIZE);
}
