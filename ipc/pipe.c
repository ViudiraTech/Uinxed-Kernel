/*
 *
 *      pipe.c
 *      Pipe and FIFO (named pipe) implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/smp.h>
#include <fs/core/vfs_stub.h>
#include <ipc/pipe.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/termios.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/fcntl.h>
#include <syscall/poll.h>
#include <syscall/syscall.h>

/* Constants */
#define PIPE_DEFAULT_MODE 0644

/* Pipe ring buffer.  Process context only, so ring->lock is a raw spinlock. */
typedef struct pipe_ring {
        uint8_t       *buf;
        uint32_t       head;
        uint32_t       tail;
        uint32_t       size;
        uint32_t       capacity;
        uint32_t       index_mask;
        uint32_t       readers;
        uint32_t       writers;
        uint32_t       read_waiters;
        uint32_t       write_waiters;
        uint32_t       write_wake_threshold;
        uint32_t       last_reader_cpu;
        uint32_t       last_writer_cpu;
        int            closed;
        raw_spinlock_t lock;
        wait_queue_t   read_wq;
        wait_queue_t   write_wq;
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

/* Static VFS filesystem ID */
static int pipe_fsid = -1;

/* Notify a pipe node's poll source of readiness changes. */
static void pipe_poll_notify(vfs_node_t node, uint32_t events)
{
    if (!node) return;

    /*
     * Call the source primitive directly: it performs the single subscriber
     * check itself, avoiding the wrapper and a duplicate atomic load.
     */
    vfs_poll_source_notify(&node->poll_source, events);
}

/* Return the number of unread bytes in the ring. */
static uint32_t pipe_ring_readable(const pipe_ring_t *ring)
{
    return ring->size;
}

/* Return the number of free bytes in the ring. */
static uint32_t pipe_ring_writable(const pipe_ring_t *ring)
{
    return ring->capacity - ring->size;
}

/*
 * A short adaptive spin avoids paying a full block/wakeup/context-switch when
 * the peer is actively running on another CPU and is expected to satisfy the
 * condition within a few hundred pause instructions.  Never spin for a peer
 * on the same CPU: that would prevent the producer/consumer from running.
 */
static bool pipe_peer_is_remote(uint32_t peer_cpu)
{
    uint32_t this_cpu = get_current_cpu_id();
    uint32_t nr_cpus  = sched_cpu_count();
    return peer_cpu < nr_cpus && peer_cpu != this_cpu;
}

/* Wait briefly for a remote writer to publish data. Called without ring->lock. */
static void pipe_spin_for_reader(pipe_ring_t *ring)
{
    uint32_t peer = __atomic_load_n(&ring->last_writer_cpu, __ATOMIC_RELAXED);
    if (!pipe_peer_is_remote(peer)) return;

    for (uint32_t i = 0; i < CONFIG_PIPE_ADAPTIVE_SPIN_ITERS; i++) {
        if (__atomic_load_n(&ring->size, __ATOMIC_ACQUIRE) != 0 || __atomic_load_n(&ring->writers, __ATOMIC_RELAXED) == 0 || __atomic_load_n(&ring->closed, __ATOMIC_RELAXED)) break;
        cpu_relax();
    }
}

/* Wait briefly for a remote reader to free at least @needed bytes. */
static void pipe_spin_for_writer(pipe_ring_t *ring, uint32_t needed)
{
    uint32_t peer = __atomic_load_n(&ring->last_reader_cpu, __ATOMIC_RELAXED);
    if (!pipe_peer_is_remote(peer)) return;

    for (uint32_t i = 0; i < CONFIG_PIPE_ADAPTIVE_SPIN_ITERS; i++) {
        uint32_t used = __atomic_load_n(&ring->size, __ATOMIC_ACQUIRE);
        if (ring->capacity - used >= needed || __atomic_load_n(&ring->readers, __ATOMIC_RELAXED) == 0 || __atomic_load_n(&ring->closed, __ATOMIC_RELAXED)) break;
        cpu_relax();
    }
}

/* Advance a ring index by count, wrapping at the ring capacity. */
static uint32_t pipe_ring_advance(const pipe_ring_t *ring, uint32_t index, uint32_t count)
{
    if (ring->index_mask) return (index + count) & ring->index_mask;

    uint32_t next = index + count;
    return next >= ring->capacity ? next - ring->capacity : next;
}

/* Advance the tail past count consumed bytes. */
static void pipe_ring_consume(pipe_ring_t *ring, uint32_t count)
{
    ring->tail = pipe_ring_advance(ring, ring->tail, count);
    ring->size -= count;
}

/* Advance the head past count produced bytes. */
static void pipe_ring_produce(pipe_ring_t *ring, uint32_t count)
{
    ring->head = pipe_ring_advance(ring, ring->head, count);
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

/* Copy ring data to user memory, handling tail wraparound. */
static int pipe_ring_copy_out_user(pipe_ring_t *ring, process_t *proc, uint8_t *dst, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->tail;
    if (first_chunk > count) first_chunk = count;

    if (copy_to_user_process_nofault_current(proc, dst, ring->buf + ring->tail, first_chunk)) return -EFAULT;
    if (count > first_chunk && copy_to_user_process_nofault_current(proc, dst + first_chunk, ring->buf, count - first_chunk)) return -EFAULT;
    return EOK;
}

/* Copy user memory into the ring, handling head wraparound. */
static int pipe_ring_copy_in_user(pipe_ring_t *ring, process_t *proc, const uint8_t *src, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->head;
    if (first_chunk > count) first_chunk = count;

    if (copy_from_user_process_nofault_current(proc, ring->buf + ring->head, src, first_chunk)) return -EFAULT;
    if (count > first_chunk && copy_from_user_process_nofault_current(proc, ring->buf, src + first_chunk, count - first_chunk)) return -EFAULT;
    return EOK;
}

/* Allocate a ring buffer with the default capacity. */
static pipe_ring_t *pipe_ring_alloc(void)
{
    pipe_ring_t *ring = calloc(1, sizeof(pipe_ring_t));
    if (!ring) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("pipe: Ring allocation failed.\n");
        return NULL;
    }

    ring->buf = malloc(CONFIG_PIPE_BUF_SIZE);
    if (!ring->buf) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("pipe: Ring buffer allocation failed (%d bytes)\n", CONFIG_PIPE_BUF_SIZE);
        free(ring);
        return NULL;
    }
    ring->capacity = CONFIG_PIPE_BUF_SIZE;

    /*
     * The configured ring size is power-of-two by default. Keep a safe
     * modulo-free path for custom non-power-of-two configurations too.
     */
    ring->index_mask      = (ring->capacity && !(ring->capacity & (ring->capacity - 1))) ? ring->capacity - 1 : 0;
    ring->head            = 0;
    ring->tail            = 0;
    ring->size            = 0;
    ring->readers         = 0;
    ring->writers         = 0;
    ring->last_reader_cpu = UINT32_MAX;
    ring->last_writer_cpu = UINT32_MAX;
    ring->closed          = 0;
    wait_queue_init(&ring->read_wq);
    wait_queue_init(&ring->write_wq);

    return ring;
}

/* Free a ring buffer and its backing store. */
static void pipe_ring_free(pipe_ring_t *ring)
{
    if (!ring) return;
    if (ring->buf) free(ring->buf);
    free(ring);
}

/* Deliver SIGPIPE to the current process (writing to a closed pipe). */
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

/* VFS callback: open */
static void pipe_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;

    if (!node) return;

    /*
     * For FIFO (named pipe) nodes, the handle is NULL on first open.
     * Create the pipe ring here.  For anonymous pipes created by
     * sys_pipe, the handle is already set before the node enters the
     * VFS, so this path is a no-op.
     */
    if (!node->handle) {
        pipe_ring_t *ring = pipe_ring_alloc();
        if (!ring) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("pipe: FIFO %s open failed (ring allocation)\n", name ? name : "?");
            return;
        }
        node->handle = ring;
    }
}

/*
 * VFS callback: close
 *
 * NOTE: The VFS layer calls this callback only when node->refcount reaches
 * zero, i.e. when the *last* file descriptor referencing this pipe node is
 * closed.  For anonymous pipes this means both the read and write ends have
 * been closed.
 *
 * All blocked readers and writers are woken here so that no task remains
 * stuck on a pipe that will never be serviced again.
 */
static void pipe_vfs_close(void *current)
{
    pipe_ring_t *ring = (pipe_ring_t *)current;
    if (!ring) return;

    raw_spin_lock(&ring->lock);
    ring->closed  = 1;
    ring->readers = 0;
    ring->writers = 0;
    raw_spin_unlock(&ring->lock);

    wait_queue_wake_all(&ring->read_wq);
    wait_queue_wake_all(&ring->write_wq);
}

/* Open a pipe endpoint, applying FIFO rendezvous rules. */
static int pipe_file_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    if (!node || !private_data) return -EINVAL;
    pipe_ring_t *ring = node->handle;
    if (!ring) return -EINVAL;

    uint64_t access = flags & O_ACCMODE;
    if (access != O_RDONLY && access != O_WRONLY && access != O_RDWR) return -EINVAL;

    pipe_endpoint_t *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("pipe: Endpoint allocation failed.\n");
        return -ENOMEM;
    }
    endpoint->ring     = ring;
    endpoint->readable = access != O_WRONLY;
    endpoint->writable = access != O_RDONLY;

    raw_spin_lock(&ring->lock);
    if (ring->closed) {
        raw_spin_unlock(&ring->lock);
        free(endpoint);
        return -EIO;
    }
    if (endpoint->readable) ring->readers++;
    if (endpoint->writable) ring->writers++;
    raw_spin_unlock(&ring->lock);
    wait_queue_wake_all(&ring->read_wq);
    wait_queue_wake_all(&ring->write_wq);

    /*
     * Anonymous pipes are born with both endpoints and must not block while
     * pipe2(2) installs them sequentially.  Named FIFOs follow open(2)'s
     * rendezvous rules.
     */
    if (node->parent && !endpoint->readable && endpoint->writable && (flags & O_NONBLOCK)) {
        raw_spin_lock(&ring->lock);
        if (ring->readers == 0) {
            ring->writers--;
            raw_spin_unlock(&ring->lock);
            free(endpoint);
            return -ENXIO;
        }
        raw_spin_unlock(&ring->lock);
    } else if (node->parent && !(flags & O_NONBLOCK) && access != O_RDWR) {
        raw_spin_lock(&ring->lock);
        while (!ring->closed && (endpoint->readable ? ring->writers == 0 : ring->readers == 0)) {
            if (signal_has_pending_current()) {
                if (endpoint->readable) ring->readers--;
                if (endpoint->writable) ring->writers--;
                raw_spin_unlock(&ring->lock);
                free(endpoint);
                return -EINTR;
            }
            wait_queue_prepare(endpoint->readable ? &ring->read_wq : &ring->write_wq);
            raw_spin_unlock(&ring->lock);
            wait_queue_sleep();
            raw_spin_lock(&ring->lock);
        }
        if (ring->closed) {
            if (endpoint->readable) ring->readers--;
            if (endpoint->writable) ring->writers--;
            raw_spin_unlock(&ring->lock);
            free(endpoint);
            return -EIO;
        }
        raw_spin_unlock(&ring->lock);
    }

    *private_data = endpoint;
    return EOK;
}

/* Release a pipe endpoint, waking peers when the last reader or writer goes away. */
static void pipe_file_release(vfs_node_t node, void *private_data)
{
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint) return;
    pipe_ring_t *ring        = endpoint->ring;
    bool         last_reader = false;
    bool         last_writer = false;

    raw_spin_lock(&ring->lock);
    if (endpoint->readable && ring->readers) last_reader = --ring->readers == 0;
    if (endpoint->writable && ring->writers) last_writer = --ring->writers == 0;
    raw_spin_unlock(&ring->lock);

    if (last_reader) {
        wait_queue_wake_all(&ring->write_wq);
        pipe_poll_notify(node, POLLERR);
    }
    if (last_writer) {
        wait_queue_wake_all(&ring->read_wq);
        pipe_poll_notify(node, POLLHUP);
    }
    free(endpoint);
}

/* VFS callback: read */
static int64_t pipe_read_common(vfs_node_t node, pipe_ring_t *ring, uint64_t flags, void *addr, size_t size)
{
    if (!ring || (!addr && size)) return -EINVAL;
    if (!size) return 0;

    bool spun = false;
    raw_spin_lock(&ring->lock);

    /*
     * Wait until data is available, the pipe is closed, or all writers have gone
     * away.  The condition is re-checked under the lock on each wakeup.
     */
    while (pipe_ring_readable(ring) == 0) {
        if (ring->closed) {
            raw_spin_unlock(&ring->lock);
            return 0;
        }
        if (ring->writers == 0) {
            raw_spin_unlock(&ring->lock);
            return 0;
        }
        if (flags & O_NONBLOCK) {
            raw_spin_unlock(&ring->lock);
            return -EAGAIN;
        }
        if (signal_has_pending_current()) {
            raw_spin_unlock(&ring->lock);
            return -EINTR;
        }
        if (!spun) {
            spun = true;
            raw_spin_unlock(&ring->lock);
            pipe_spin_for_reader(ring);
            raw_spin_lock(&ring->lock);
            continue;
        }

        /* prepare wait under lock, then block, re-acquire on wakeup */
        ring->read_waiters++;
        wait_queue_prepare(&ring->read_wq);
        raw_spin_unlock(&ring->lock);
        wait_queue_sleep();
        raw_spin_lock(&ring->lock);
        if (ring->read_waiters) ring->read_waiters--;
    }

    uint32_t avail    = pipe_ring_readable(ring);
    uint32_t chunk    = (size < avail) ? (uint32_t)size : avail;
    bool     was_full = pipe_ring_writable(ring) == 0;

    pipe_ring_copy_out(ring, (uint8_t *)addr, chunk);
    pipe_ring_consume(ring, chunk);
    __atomic_store_n(&ring->last_reader_cpu, get_current_cpu_id(), __ATOMIC_RELAXED);
    bool wake_writers = ring->write_waiters != 0 && pipe_ring_writable(ring) >= ring->write_wake_threshold;

    raw_spin_unlock(&ring->lock);

    /*
     * An exclusive writer wait is used: freeing one pipe-buffer slot
     * wakes one writer, which may cascade to the next writer if space
     * remains.  Waking the whole queue creates avoidable scheduler/IPI work.
     */
    if (wake_writers) wait_queue_wake_one_sync(&ring->write_wq);
    if (was_full) pipe_poll_notify(node, POLLOUT);

    return (int64_t)chunk;
}

/* VFS read callback using a kernel-supplied buffer. */
static size_t pipe_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    int64_t result = pipe_read_common(NULL, file, 0, addr, size);
    return result < 0 ? (size_t)-1 : (size_t)result;
}

/* Read from a pipe endpoint into a kernel buffer. */
static int64_t pipe_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->readable) return -EBADF;
    return pipe_read_common(node, endpoint->ring, flags, addr, size);
}

/* Read from a pipe endpoint, copying data to user space. */
static int64_t pipe_file_read_user(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->readable) return -EBADF;
    if (!size) return 0;

    pipe_ring_t *ring = endpoint->ring;
    bool         spun = false;
    for (;;) {
        raw_spin_lock(&ring->lock);
        while (pipe_ring_readable(ring) == 0) {
            if (ring->closed || ring->writers == 0) {
                raw_spin_unlock(&ring->lock);
                return 0;
            }
            if (flags & O_NONBLOCK) {
                raw_spin_unlock(&ring->lock);
                return -EAGAIN;
            }
            if (signal_has_pending_current()) {
                raw_spin_unlock(&ring->lock);
                return -EINTR;
            }
            if (!spun) {
                spun = true;
                raw_spin_unlock(&ring->lock);
                pipe_spin_for_reader(ring);
                raw_spin_lock(&ring->lock);
                continue;
            }
            ring->read_waiters++;
            wait_queue_prepare(&ring->read_wq);
            raw_spin_unlock(&ring->lock);
            wait_queue_sleep();
            raw_spin_lock(&ring->lock);
            if (ring->read_waiters) ring->read_waiters--;
        }

        uint32_t avail    = pipe_ring_readable(ring);
        uint32_t chunk    = size < avail ? (uint32_t)size : avail;
        bool     was_full = pipe_ring_writable(ring) == 0;

        if (pipe_ring_copy_out_user(ring, proc, addr, chunk)) {
            raw_spin_unlock(&ring->lock);

            /*
             * Fault/COW resolution can allocate or sleep, so it is done only
             * after dropping the ring lock.  Nothing has been consumed yet.
             */
            if (!user_access_ok_process(proc, addr, chunk, 1)) return -EFAULT;
            continue;
        }

        pipe_ring_consume(ring, chunk);
        __atomic_store_n(&ring->last_reader_cpu, get_current_cpu_id(), __ATOMIC_RELAXED);
        bool wake_writers = ring->write_waiters != 0 && pipe_ring_writable(ring) >= ring->write_wake_threshold;
        raw_spin_unlock(&ring->lock);

        if (wake_writers) wait_queue_wake_one_sync(&ring->write_wq);
        if (was_full) pipe_poll_notify(node, POLLOUT);
        return (int64_t)chunk;
    }
}

/* VFS callback: write */
static int64_t pipe_write_common(vfs_node_t node, pipe_ring_t *ring, uint64_t flags, const void *addr, size_t size)
{
    if (!ring || (!addr && size)) return -EINVAL;
    if (!size) return 0;

    size_t         total_written = 0;
    const uint8_t *src           = (const uint8_t *)addr;
    bool           spun          = false;

    while (total_written < size) {
        raw_spin_lock(&ring->lock);

        if (ring->closed || ring->readers == 0) {
            raw_spin_unlock(&ring->lock);
            if (!total_written) pipe_raise_sigpipe();
            return total_written ? (int64_t)total_written : -EPIPE;
        }

        size_t   remaining = size - total_written;
        bool     atomic    = size <= CONFIG_PIPE_ATOMIC_SIZE;
        uint32_t wake_threshold;
        if (atomic) {
            wake_threshold = (uint32_t)remaining;
        } else {
            wake_threshold = (uint32_t)(remaining < CONFIG_PIPE_ATOMIC_SIZE ? remaining : CONFIG_PIPE_ATOMIC_SIZE);
        }

        /*
         * PIPE_BUF-sized writes remain atomic.  Larger writes may consume whatever
         * space is already available (including O_NONBLOCK partial writes), but once
         * the pipe is full they wait for a useful batch of space instead of bouncing
         * producer/consumer every 512 bytes.
         */
        while (atomic ? pipe_ring_writable(ring) < (uint32_t)remaining : pipe_ring_writable(ring) == 0) {
            if (ring->closed || ring->readers == 0) {
                raw_spin_unlock(&ring->lock);
                if (!total_written) pipe_raise_sigpipe();
                return total_written ? (int64_t)total_written : -EPIPE;
            }
            if (flags & O_NONBLOCK) {
                raw_spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EAGAIN;
            }
            if (signal_has_pending_current()) {
                raw_spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EINTR;
            }
            if (!spun) {
                spun               = true;
                uint32_t spin_need = atomic ? (uint32_t)remaining : 1U;
                raw_spin_unlock(&ring->lock);
                pipe_spin_for_writer(ring, spin_need);
                raw_spin_lock(&ring->lock);
                continue;
            }
            if (!ring->write_waiters || wake_threshold < ring->write_wake_threshold) ring->write_wake_threshold = wake_threshold;
            ring->write_waiters++;
            wait_queue_prepare(&ring->write_wq);
            raw_spin_unlock(&ring->lock);
            wait_queue_sleep();
            raw_spin_lock(&ring->lock);
            if (ring->write_waiters) ring->write_waiters--;
            if (!ring->write_waiters) ring->write_wake_threshold = 0;
        }

        uint32_t writable = pipe_ring_writable(ring);
        uint32_t chunk    = (uint32_t)(remaining < writable ? remaining : writable);

        bool was_empty = pipe_ring_readable(ring) == 0;
        pipe_ring_copy_in(ring, src + total_written, chunk);
        pipe_ring_produce(ring, chunk);
        __atomic_store_n(&ring->last_writer_cpu, get_current_cpu_id(), __ATOMIC_RELAXED);
        total_written += chunk;
        spun              = false;
        bool wake_readers = was_empty && ring->read_waiters != 0;

        raw_spin_unlock(&ring->lock);

        /* Wake readers that may be waiting for data */
        if (wake_readers) wait_queue_wake_one_sync(&ring->read_wq);
        if (was_empty) pipe_poll_notify(node, POLLIN);
    }

    return (int64_t)total_written;
}

/* VFS write callback using a kernel-supplied buffer. */
static size_t pipe_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    int64_t result = pipe_write_common(NULL, file, 0, addr, size);
    return result < 0 ? (size_t)-1 : (size_t)result;
}

/* Copy up to len bytes between two pipe endpoints without consuming the input, for tee(2). */
int64_t pipe_tee(void *in_private, void *out_private, size_t len, uint64_t flags)
{
    pipe_endpoint_t *in  = in_private;
    pipe_endpoint_t *out = out_private;
    if (!in || !in->ring || !in->readable || !out || !out->ring || !out->writable) return -EINVAL;
    if (!len) return 0;

    pipe_ring_t *ring = in->ring;
    size_t       cap  = ring->capacity < len ? ring->capacity : len;
    uint8_t     *buf  = malloc(cap);
    if (!buf) return -ENOMEM;

    bool spun = false;
    raw_spin_lock(&ring->lock);
    while (pipe_ring_readable(ring) == 0) {
        if (ring->closed || ring->writers == 0) {
            raw_spin_unlock(&ring->lock);
            free(buf);
            return 0;
        }
        if (flags & O_NONBLOCK) {
            raw_spin_unlock(&ring->lock);
            free(buf);
            return -EAGAIN;
        }
        if (signal_has_pending_current()) {
            raw_spin_unlock(&ring->lock);
            free(buf);
            return -EINTR;
        }
        if (!spun) {
            spun = true;
            raw_spin_unlock(&ring->lock);
            pipe_spin_for_reader(ring);
            raw_spin_lock(&ring->lock);
            continue;
        }
        ring->read_waiters++;
        wait_queue_prepare(&ring->read_wq);
        raw_spin_unlock(&ring->lock);
        wait_queue_sleep();
        raw_spin_lock(&ring->lock);
        if (ring->read_waiters) ring->read_waiters--;
    }

    uint32_t avail = pipe_ring_readable(ring);
    uint32_t chunk = (cap < avail) ? (uint32_t)cap : avail;
    pipe_ring_copy_out(ring, buf, chunk);
    raw_spin_unlock(&ring->lock);

    int64_t written = pipe_write_common(NULL, out->ring, flags, buf, chunk);
    free(buf);
    return written;
}

/* Write to a pipe endpoint from a kernel buffer. */
static int64_t pipe_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->writable) return -EBADF;
    return pipe_write_common(node, endpoint->ring, flags, addr, size);
}

/* Write to a pipe endpoint, copying data from user space. */
static int64_t pipe_file_write_user(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc)
{
    (void)offset;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->writable) return -EBADF;
    if (!size) return 0;

    pipe_ring_t *ring          = endpoint->ring;
    size_t       total_written = 0;
    bool         spun          = false;

    while (total_written < size) {
        raw_spin_lock(&ring->lock);
        if (ring->closed || ring->readers == 0) {
            raw_spin_unlock(&ring->lock);
            if (!total_written) pipe_raise_sigpipe();
            return total_written ? (int64_t)total_written : -EPIPE;
        }

        size_t   remaining = size - total_written;
        bool     atomic    = size <= CONFIG_PIPE_ATOMIC_SIZE;
        uint32_t wake_threshold;
        if (atomic) {
            wake_threshold = (uint32_t)remaining;
        } else {
            wake_threshold = (uint32_t)(remaining < CONFIG_PIPE_ATOMIC_SIZE ? remaining : CONFIG_PIPE_ATOMIC_SIZE);
        }

        while (atomic ? pipe_ring_writable(ring) < (uint32_t)remaining : pipe_ring_writable(ring) == 0) {
            if (ring->closed || ring->readers == 0) {
                raw_spin_unlock(&ring->lock);
                if (!total_written) pipe_raise_sigpipe();
                return total_written ? (int64_t)total_written : -EPIPE;
            }
            if (flags & O_NONBLOCK) {
                raw_spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EAGAIN;
            }
            if (signal_has_pending_current()) {
                raw_spin_unlock(&ring->lock);
                return total_written ? (int64_t)total_written : -EINTR;
            }
            if (!spun) {
                spun               = true;
                uint32_t spin_need = atomic ? (uint32_t)remaining : 1U;
                raw_spin_unlock(&ring->lock);
                pipe_spin_for_writer(ring, spin_need);
                raw_spin_lock(&ring->lock);
                continue;
            }
            if (!ring->write_waiters || wake_threshold < ring->write_wake_threshold) ring->write_wake_threshold = wake_threshold;
            ring->write_waiters++;
            wait_queue_prepare(&ring->write_wq);
            raw_spin_unlock(&ring->lock);
            wait_queue_sleep();
            raw_spin_lock(&ring->lock);
            if (ring->write_waiters) ring->write_waiters--;
            if (!ring->write_waiters) ring->write_wake_threshold = 0;
        }

        uint32_t       writable = pipe_ring_writable(ring);
        uint32_t       chunk    = (uint32_t)(remaining < writable ? remaining : writable);
        const uint8_t *src      = (const uint8_t *)addr + total_written;
        if (pipe_ring_copy_in_user(ring, proc, src, chunk)) {
            raw_spin_unlock(&ring->lock);
            if (!user_access_ok_process(proc, src, chunk, 0)) return total_written ? (int64_t)total_written : -EFAULT;
            continue;
        }

        bool was_empty = pipe_ring_readable(ring) == 0;
        pipe_ring_produce(ring, chunk);
        __atomic_store_n(&ring->last_writer_cpu, get_current_cpu_id(), __ATOMIC_RELAXED);
        total_written += chunk;
        spun              = false;
        bool wake_readers = was_empty && ring->read_waiters != 0;
        raw_spin_unlock(&ring->lock);

        if (wake_readers) wait_queue_wake_one_sync(&ring->read_wq);
        if (was_empty) pipe_poll_notify(node, POLLIN);
    }

    return (int64_t)total_written;
}

/* VFS callback: poll */
static int pipe_vfs_poll(void *file, size_t events)
{
    pipe_ring_t *ring = (pipe_ring_t *)file;
    if (!ring) return 0;
    int revents = 0;

    raw_spin_lock(&ring->lock);

    if (pipe_ring_readable(ring) > 0) revents |= POLLIN;
    if (ring->writers == 0 || ring->closed) revents |= POLLHUP;
    if (pipe_ring_writable(ring) > 0 && ring->readers > 0 && !ring->closed) revents |= POLLOUT;

    raw_spin_unlock(&ring->lock);

    return (revents & (int)events) | (revents & (POLLERR | POLLHUP));
}

/* Poll a pipe endpoint for readability and writability. */
static int pipe_file_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events)
{
    (void)node;
    (void)flags;
    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint) return POLLERR;
    pipe_ring_t *ring    = endpoint->ring;
    int          revents = 0;

    raw_spin_lock(&ring->lock);
    if (endpoint->readable) {
        if (pipe_ring_readable(ring)) revents |= POLLIN;
        if (!ring->writers || ring->closed) revents |= POLLHUP;
    }
    if (endpoint->writable) {
        if (!ring->readers || ring->closed) {
            revents |= POLLERR;
        } else if (pipe_ring_writable(ring)) {
            revents |= POLLOUT;
        }
    }
    raw_spin_unlock(&ring->lock);
    return (revents & (int)events) | (revents & (POLLERR | POLLHUP));
}

/* VFS callback: report the number of bytes immediately readable from a pipe. */
static int pipe_file_ioctl(vfs_node_t node, void *private_data, uint64_t flags, size_t request, void *argument)
{
    (void)node;
    (void)flags;
    if (request != FIONREAD) return -ENOTTY;
    if (!argument) return -EFAULT;

    pipe_endpoint_t *endpoint = private_data;
    if (!endpoint || !endpoint->ring) return -EBADF;

    pipe_ring_t *ring = endpoint->ring;
    raw_spin_lock(&ring->lock);
    int bytes = (int)pipe_ring_readable(ring);
    raw_spin_unlock(&ring->lock);

    return copy_to_user(argument, &bytes, sizeof(bytes)) ? -EFAULT : EOK;
}

/* VFS callback: free (release handle resources) */
static int pipe_vfs_free(void *handle)
{
    pipe_ring_t *ring = (pipe_ring_t *)handle;
    if (!ring) return -EINVAL;

    pipe_ring_free(ring);
    return EOK;
}

/* VFS callback: stat */
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

/* Pipe node creation (shared by sys_pipe and sys_pipe2) */
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

/* Syscall: pipe - create an anonymous pipe. */
int64_t sys_pipe(int pipefd[2])
{
    return sys_pipe2(pipefd, 0);
}

/* Syscall: pipe2 - create an anonymous pipe with flags. */
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
        pipe_ring_free(ring);
        return -ENOMEM;
    }

    /*
     * Hold a construction reference.  Each successfully installed open-file
     * description consumes one additional node reference.
     */
    (void)vfs_node_retain(node);

    /* Build fd flags: O_RDONLY for read, O_WRONLY for write, plus O_CLOEXEC and O_NONBLOCK from the flags argument. */
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

/* Create a FIFO (named pipe) node at the given resolved path */
int pipe_mknod(char *path, uint16_t mode)
{
    char        parent_path[CONFIG_VFS_PATH_MAX];
    const char *filename;
    vfs_node_t  parent;
    int         status = vfs_split_parent(path, parent_path, sizeof(parent_path), &filename);

    if (status != EOK) return status;

    parent = vfs_open(parent_path);
    if (!parent || !(parent->type & file_dir)) {
        if (parent) vfs_close(parent);
        return -ENOENT;
    }

    /* Reject a node that already exists */
    if (vfs_do_search(parent, filename)) {
        vfs_close(parent);
        return -EEXIST;
    }

    /*
     * Construct the FIFO node directly: the parent filesystem's mkfile
     * callback would otherwise treat it as a regular file.
     */
    vfs_node_t node = vfs_node_alloc(parent, filename);
    if (!node) {
        vfs_close(parent);
        return -ENOMEM;
    }

    node->type        = file_pipe;
    node->fsid        = pipe_fsid;
    node->mode        = mode & 07777;
    node->handle      = NULL; // ring created in the VFS open callback
    node->permissions = mode & 07777;

    process_t *proc = process_current();
    if (proc) {
        node->owner = proc->fsuid;
        node->group = proc->fsgid;
    }

    vfs_close(parent);
    return EOK;
}

/* Initialization */
void pipe_init(void)
{
    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("pipe: Failed to allocate VFS callback structure.\n");
        return;
    }

    cb->unmount         = vfs_stub_unmount;
    cb->open            = pipe_vfs_open;
    cb->close           = pipe_vfs_close;
    cb->read            = pipe_vfs_read;
    cb->write           = pipe_vfs_write;
    cb->readlink        = vfs_stub_readlink;
    cb->mkdir           = vfs_stub_mk;
    cb->mkfile          = vfs_stub_mk;
    cb->link            = vfs_stub_mk;
    cb->symlink         = vfs_stub_mk;
    cb->stat            = pipe_vfs_stat;
    cb->ioctl           = vfs_stub_ioctl_notty;
    cb->dup             = vfs_stub_dup;
    cb->poll            = pipe_vfs_poll;
    cb->free            = pipe_vfs_free;
    cb->delete          = vfs_stub_del;
    cb->rename          = vfs_stub_rename;
    cb->file_open       = pipe_file_open;
    cb->file_release    = pipe_file_release;
    cb->file_read       = pipe_file_read;
    cb->file_write      = pipe_file_write;
    cb->file_read_user  = pipe_file_read_user;
    cb->file_write_user = pipe_file_write_user;
    cb->file_ioctl      = pipe_file_ioctl;
    cb->file_poll       = pipe_file_poll;

    pipe_fsid = vfs_regist_fs("pipefs", cb);
    if (pipe_fsid < 0) {
        plogk("pipe: Failed to register VFS callback (err=%d)\n", pipe_fsid);
        free(cb);
        return;
    }

    plogk("pipe: Pipe subsystem registered (fsid=%d, buffer=%d bytes)\n", pipe_fsid, CONFIG_PIPE_BUF_SIZE);
}
