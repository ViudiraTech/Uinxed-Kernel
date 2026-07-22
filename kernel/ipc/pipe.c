/*
 *
 *      pipe.c
 *      Pipe and FIFO (named pipe) implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <alloc.h>
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
#include <task.h>
#include <uaccess.h>
#include <syscall.h>
#include <vfs.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define PIPE_BUF_SIZE    65536   /* 64KB pipe buffer, atomic write guarantee */
#define PIPE_DEFAULT_MODE 0644

/* poll event bits */
#define POLLIN  0x001
#define POLLOUT 0x004
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
    int          closed;
    spinlock_t   lock;
    wait_queue_t read_wq;
    wait_queue_t write_wq;
} pipe_ring_t;

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
    ring->tail  = (ring->tail + count) % ring->capacity;
    ring->size -= count;
}

static void pipe_ring_produce(pipe_ring_t *ring, uint32_t count)
{
    ring->head  = (ring->head + count) % ring->capacity;
    ring->size += count;
}

/* Copy data from ring buffer to linear buffer, handling wraparound */
static uint32_t pipe_ring_copy_out(pipe_ring_t *ring, uint8_t *dst, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->tail;

    if (first_chunk > count) first_chunk = count;
    memcpy(dst, ring->buf + ring->tail, first_chunk);

    if (count > first_chunk) {
        memcpy(dst + first_chunk, ring->buf, count - first_chunk);
    }
    return count;
}

/* Copy data from linear buffer to ring buffer, handling wraparound */
static uint32_t pipe_ring_copy_in(pipe_ring_t *ring, const uint8_t *src, uint32_t count)
{
    uint32_t first_chunk = ring->capacity - ring->head;

    if (first_chunk > count) first_chunk = count;
    memcpy(ring->buf + ring->head, src, first_chunk);

    if (count > first_chunk) {
        memcpy(ring->buf, src + first_chunk, count - first_chunk);
    }
    return count;
}

static pipe_ring_t *pipe_ring_alloc(void)
{
    pipe_ring_t *ring = calloc(1, sizeof(pipe_ring_t));
    if (!ring) return NULL;

    ring->buf = malloc(PIPE_BUF_SIZE);
    if (!ring->buf) {
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
        if (!ring) return;
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

/* ------------------------------------------------------------------ */
/*  VFS callback: read                                                  */
/* ------------------------------------------------------------------ */

static size_t pipe_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    pipe_ring_t *ring = (pipe_ring_t *)file;
    if (!ring || !addr || !size) return (size_t)-1;

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
            return 0;               /* EOF — no writers left */
        }
        /* release lock, block, re-acquire on wakeup */
        spin_unlock(&ring->lock);
        wait_queue_wait(&ring->read_wq);
        spin_lock(&ring->lock);
    }

    uint32_t avail = pipe_ring_readable(ring);
    uint32_t chunk = (size < avail) ? (uint32_t)size : avail;

    pipe_ring_copy_out(ring, (uint8_t *)addr, chunk);
    pipe_ring_consume(ring, chunk);

    spin_unlock(&ring->lock);

    /* Wake writers that may be waiting for buffer space */
    wait_queue_wake_all(&ring->write_wq);

    return chunk;
}

/* ------------------------------------------------------------------ */
/*  VFS callback: write                                                 */
/* ------------------------------------------------------------------ */

static size_t pipe_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    pipe_ring_t *ring = (pipe_ring_t *)file;
    if (!ring || !addr || !size) return (size_t)-1;

    spin_lock(&ring->lock);

    if (ring->closed || ring->readers == 0) {
        spin_unlock(&ring->lock);
        return (size_t)-1;           /* -EPIPE — no readers */
    }

    /*
     * Writes up to PIPE_BUF_SIZE are guaranteed atomic.
     * For non-atomic (larger) writes we write as much as fits
     * in one go and return the count; the caller retries if needed.
     */
    uint32_t write_size = (size > PIPE_BUF_SIZE) ? PIPE_BUF_SIZE : (uint32_t)size;

    while (pipe_ring_writable(ring) < write_size) {
        if (ring->closed || ring->readers == 0) {
            spin_unlock(&ring->lock);
            return (size_t)-1;       /* -EPIPE */
        }
        spin_unlock(&ring->lock);
        wait_queue_wait(&ring->write_wq);
        spin_lock(&ring->lock);
    }

    pipe_ring_copy_in(ring, (const uint8_t *)addr, write_size);
    pipe_ring_produce(ring, write_size);

    spin_unlock(&ring->lock);

    /* Wake readers that may be waiting for data */
    wait_queue_wake_all(&ring->read_wq);

    return write_size;
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

    if (pipe_ring_readable(ring) > 0) {
        revents |= POLLIN;
    }
    if (ring->writers == 0 || ring->closed) {
        revents |= POLLHUP;
    }
    if (pipe_ring_writable(ring) > 0 && ring->readers > 0 && !ring->closed) {
        revents |= POLLOUT;
    }

    spin_unlock(&ring->lock);

    return revents & (int)events;
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
    if (ring) {
        node->size = ring->size;
    }
    node->type  |= file_pipe;
    node->mode   = PIPE_DEFAULT_MODE;
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
/*  Pipe fd installation helper                                         */
/* ------------------------------------------------------------------ */

static int pipe_fd_install(pipe_ring_t *ring, vfs_node_t node, int is_read_end)
{
    (void)ring;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    uint64_t fd_flags = is_read_end ? O_RDONLY : O_WRONLY;
    int      fd       = process_fd_install(proc, node, fd_flags);
    if (fd < 0) return fd;

    return fd;
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
    (void)flags;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    if (!pipefd) return -EFAULT;

    /* Validate user pointer readability */
    if (!user_access_ok(pipefd, 2 * sizeof(int), 1)) return -EFAULT;

    /* Allocate the pipe ring buffer */
    pipe_ring_t *ring = pipe_ring_alloc();
    if (!ring) return -ENOMEM;

    ring->readers = 1;
    ring->writers = 1;

    /* Create the VFS node */
    vfs_node_t node = pipe_node_create(ring);
    if (!node) {
        pipe_ring_free(ring);
        return -ENOMEM;
    }

    /* Install read-end fd */
    int fd_read = pipe_fd_install(ring, node, 1);
    if (fd_read < 0) {
        vfs_close(node);
        vfs_delete(node);
        return fd_read;
    }

    /* Install write-end fd */
    int fd_write = pipe_fd_install(ring, node, 0);
    if (fd_write < 0) {
        process_fd_close(proc, fd_read);
        vfs_close(node);
        vfs_delete(node);
        return fd_write;
    }

    /* Copy the two fds out to user space */
    int fds[2] = {fd_read, fd_write};
    if (copy_to_user(pipefd, fds, sizeof(fds))) {
        process_fd_close(proc, fd_read);
        process_fd_close(proc, fd_write);
        vfs_close(node);
        vfs_delete(node);
        return -EFAULT;
    }

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mknod / mkfifo (FIFO / named pipe)                         */
/* ------------------------------------------------------------------ */

int64_t sys_mknod(const char *path, uint32_t mode, uint64_t dev)
{
    (void)dev;

    if (!path) return -EFAULT;

    /* Only support FIFO creation through mknod */
    if ((mode & 0170000) != 0010000) return -EINVAL;

    /* Allocate a pipe ring for the FIFO (will be filled on open) */
    /* Actually, for FIFO we defer ring creation to the VFS open callback. */
    /* Here we just create the VFS node with type file_pipe. */

    /* Use vfs_mkfile to create the node, then override its type and fsid.
     * We need to manually construct the FIFO node because vfs_mkfile
     * delegates to the parent filesystem's mkfile callback, which would
     * treat it as a regular file. */
    char path_copy[256];
    int  copied = strncpy_from_user(path_copy, path, sizeof(path_copy));
    if (copied < 0) return copied;

    if (path_copy[0] != '/') return -EINVAL;

    /* Find the last '/' to separate parent path from filename */
    char *fullpath  = path_copy;
    char *lastslash = strrchr(fullpath, '/');
    char *filename;
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
    node->handle      = NULL;   /* ring created in VFS open callback */
    node->permissions = mode & 07777;

    if (parent != rootdir) vfs_close(parent);

    return EOK;
}

int64_t sys_mkfifo(const char *path, uint32_t mode)
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

int pipe_open(vfs_node_t node, uint64_t flags)
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
            /* Opening write-only with no readers and O_NONBLOCK → ENXIO */
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
        spin_unlock(&ring->lock);
        /*
         * Wait on the appropriate queue.  The other end's open will
         * wake us via wait_queue_wake_all.
         */
        wait_queue_wait(is_read ? &ring->read_wq : &ring->write_wq);
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

    cb->mount    = pipe_stub_mount;
    cb->unmount  = pipe_stub_unmount;
    cb->open     = pipe_vfs_open;
    cb->close    = pipe_vfs_close;
    cb->read     = pipe_vfs_read;
    cb->write    = pipe_vfs_write;
    cb->readlink = pipe_stub_readlink;
    cb->mkdir    = pipe_stub_mk;
    cb->mkfile   = pipe_stub_mk;
    cb->link     = pipe_stub_mk;
    cb->symlink  = pipe_stub_mk;
    cb->stat     = pipe_vfs_stat;
    cb->ioctl    = pipe_stub_ioctl;
    cb->dup      = pipe_stub_dup;
    cb->poll     = pipe_vfs_poll;
    cb->free     = pipe_vfs_free;
    cb->delete   = pipe_stub_del;
    cb->rename   = pipe_stub_rename;

    pipe_fsid = vfs_regist(cb);
    if (pipe_fsid < 0) {
        plogk("pipe: Failed to register VFS callback (err=%d).\n", pipe_fsid);
        free(cb);
        return;
    }

    plogk("pipe: Subsystem initialized (fsid=%d, buffer=%d bytes)\n",
          pipe_fsid, PIPE_BUF_SIZE);
}