/*
 *
 *      socket.c
 *      BSD Socket API implementation - UNIX domain sockets
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <fs/tmpfs/tmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <net/abi/inet.h>
#include <net/netlink/netlink.h>
#include <net/socket.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>

/* Constants */

#define SOCK_ACCEPT_QUEUE_INIT 16
#ifndef SOCK_ACCEPT_QUEUE_MAX
#    define SOCK_ACCEPT_QUEUE_MAX 1024
#endif
#define SOCK_BOUND_MAX      256
#define SOCK_SHUT_MASK(how) (1U << (uint32_t)(how))

/* Blocked-socket tracking - maps a blocked socket to its task */

/* Bound-address registry - UNIX-domain namespace */

typedef struct sock_bound {
        socket_t     *sk;
        sockaddr_un_t addr;
        uint32_t      addrlen;
        int           abstract; // 1 = abstract namespace, 0 = pathname
} sock_bound_t;

static sock_bound_t sock_bound_tab[SOCK_BOUND_MAX];
static spinlock_t   sock_bound_lock;

/* VFS filesystem id for socket nodes */

static int socket_fsid = -1;

/* Forward declarations - internal helpers */

static void sock_blocked_register(socket_t *sk, task_t *task);
static void sock_blocked_unregister(socket_t *sk);
static void sock_blocked_wake(socket_t *sk);
static void sock_blocked_wake_all(socket_t *sk);

static int  sock_bound_lookup(const sockaddr_un_t *addr, uint32_t addrlen, int abstract, socket_t **out);
static int  sock_bound_add(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen, int abstract);
static void sock_bound_remove(socket_t *sk);

static int       unix_addr_parse(const sockaddr_un_t *addr, uint32_t addrlen, int *is_abstract);
static int       unix_autobind(socket_t *sk);
static int       unix_bind(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen);
static int       unix_listen(socket_t *sk, uint32_t backlog);
static int       unix_accept(socket_t *sk, sockaddr_un_t *addr, uint32_t *addrlen, int flags);
static int       unix_stream_connect(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen);
static int       socket_fd_install_flags(socket_t *sk, uint64_t fd_flags);
static int       socket_fd_nonblock(int fd);
static socket_t *inet_socket_alloc(uint16_t family, uint16_t type, uint16_t protocol, uint32_t flags, void *context);
static int       socket_copy_address_to_user(sockaddr_t *addr, uint32_t *addrlen, const sockaddr_t *kaddr, uint32_t kaddrlen);

static void socket_inet_event(void *argument, uint32_t events)
{
    socket_t *sk = argument;
    if (sk && sk->node) vfs_poll_notify(sk->node, events);
}

static int unix_stream_send(socket_t *sk, const void *buf, size_t len, int flags);
static int unix_stream_send_rights(socket_t *sk, const void *buf, size_t len, int flags, process_file_t **rights, size_t rights_count);
static int unix_stream_recv(socket_t *sk, void *buf, size_t len, int flags);
static int unix_dgram_send(socket_t *sk, const void *buf, size_t len, const sockaddr_un_t *addr, uint32_t addrlen, int flags);
static int unix_dgram_recv(socket_t *sk, void *buf, size_t len, sockaddr_un_t *addr, uint32_t *addrlen, int flags, ucred_t *credentials,
                           int *message_flags, size_t *record_size);

static size_t socket_vfs_read(void *file, void *addr, size_t offset, size_t size);
static size_t socket_vfs_write(void *file, const void *addr, size_t offset, size_t size);
static int    socket_vfs_poll(void *file, size_t events);
static void   socket_vfs_close(void *current);
static int    socket_vfs_free(void *handle);

/* Local helper: bounded strlen */
static size_t strnlen_local(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n] != '\0') n++;
    return n;
}

/* Circular buffer helpers */

static int sock_buf_init(sock_buf_t *buf, uint32_t capacity)
{
    if (capacity > SOCK_BUF_MAX) capacity = SOCK_BUF_MAX;
    if (capacity == 0) capacity = SOCK_BUF_SIZE;

    buf->data = calloc(1, capacity);
    if (!buf->data) return -ENOMEM;

    buf->head     = 0;
    buf->tail     = 0;
    buf->size     = 0;
    buf->capacity = capacity;
    return EOK;
}

static void sock_buf_free(sock_buf_t *buf)
{
    if (!buf) return;
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    buf->head     = 0;
    buf->tail     = 0;
    buf->size     = 0;
    buf->capacity = 0;
}

static uint32_t sock_buf_available(sock_buf_t *buf)
{
    if (!buf || !buf->data) return 0;
    return buf->size;
}

static uint32_t sock_buf_space(sock_buf_t *buf)
{
    if (!buf || !buf->data) return 0;
    return buf->capacity - buf->size;
}

static uint32_t sock_buf_write(sock_buf_t *buf, const void *data, uint32_t len)
{
    uint32_t written;

    if (!buf || !buf->data) return 0;

    uint32_t space = buf->capacity - buf->size;
    if (len > space) len = space;
    if (len == 0) return 0;

    written = 0;
    while (written < len) {
        uint32_t chunk;
        uint32_t pos = buf->tail;

        /*
         * head == tail is ambiguous: it represents both an empty and a full
         * ring.  Full buffers were handled by the space check above; for an
         * empty ring the writable extent runs to the end of the allocation.
         * Treating the empty case as tail < head produced chunk == 0 and an
         * infinite loop on the very first socket write.
         */
        if (pos >= buf->head) {
            chunk = buf->capacity - pos;
        } else {
            chunk = buf->head - pos;
        }
        if (chunk > space) chunk = space;
        if (chunk > len - written) chunk = len - written;

        memcpy(buf->data + pos, (const uint8_t *)data + written, chunk);
        written += chunk;
        buf->tail = (pos + chunk) % buf->capacity;
        buf->size += chunk;
        space -= chunk;
    }

    return written;
}

static uint32_t sock_buf_read(sock_buf_t *buf, void *data, uint32_t len)
{
    uint32_t rd;

    if (!buf || !buf->data) return 0;
    if (len > buf->size) len = buf->size;
    if (len == 0) return 0;

    rd = 0;
    while (rd < len) {
        uint32_t chunk;
        uint32_t pos = buf->head;

        if (pos < buf->tail) {
            chunk = buf->tail - pos;
        } else {
            /* head is at or after tail, wrap */
            chunk = buf->capacity - pos;
        }
        if (chunk > len - rd) chunk = len - rd;

        memcpy((uint8_t *)data + rd, buf->data + pos, chunk);
        rd += chunk;
        buf->head = (pos + chunk) % buf->capacity;
        buf->size -= chunk;
    }

    return rd;
}

static uint32_t sock_buf_peek(sock_buf_t *buf, void *data, uint32_t len)
{
    uint32_t pk;

    if (!buf || !buf->data) return 0;
    uint32_t head = buf->head;
    uint32_t size = buf->size;

    if (len > size) len = size;
    if (len == 0) return 0;

    pk = 0;
    while (pk < len) {
        uint32_t chunk;
        uint32_t pos = head;

        if (pos < buf->tail) {
            chunk = buf->tail - pos;
        } else {
            chunk = buf->capacity - pos;
        }
        if (chunk > len - pk) chunk = len - pk;

        memcpy((uint8_t *)data + pk, buf->data + pos, chunk);
        pk += chunk;
        head = (pos + chunk) % buf->capacity;
    }

    return pk;
}

static uint32_t sock_buf_peek_at(sock_buf_t *buf, uint32_t offset, void *data, uint32_t len)
{
    if (!buf || !buf->data || offset > buf->size) return 0;
    uint32_t available = buf->size - offset;
    if (len > available) len = available;
    if (!len) return 0;

    uint32_t position = (buf->head + offset) % buf->capacity;
    uint32_t copied   = 0;
    while (copied < len) {
        uint32_t chunk = buf->capacity - position;
        if (chunk > len - copied) chunk = len - copied;
        memcpy((uint8_t *)data + copied, buf->data + position, chunk);
        copied += chunk;
        position = (position + chunk) % buf->capacity;
    }
    return copied;
}

static void sock_buf_discard(sock_buf_t *buf, uint32_t len)
{
    if (!buf || !buf->data) return;
    if (len > buf->size) len = buf->size;
    buf->head = (buf->head + len) % buf->capacity;
    buf->size -= len;
}

/* Blocked-socket tracking */

static void sock_blocked_register(socket_t *sk, task_t *task)
{
    /*
     * The old side table followed by task_block() had a lost-wakeup window:
     * a peer could wake the task after the socket lock was released but
     * before task_block() changed its state.  Prepare the scheduler wait
     * while the caller still holds the socket lock; wait_queue_sleep() then
     * atomically observes an early wake and does not sleep.
     */
    (void)task;
    if (sk) wait_queue_prepare(&sk->waitq);
}

static void sock_blocked_unregister(socket_t *sk)
{
    /*
     * A successful wake removes the task from waitq.  Callers retain this
     * hook for symmetry with the old implementation.
     */
    (void)sk;
}

static void sock_blocked_wake(socket_t *sk)
{
    if (sk) wait_queue_wake_all(&sk->waitq);
}

static void sock_blocked_wake_all(socket_t *sk)
{
    if (sk) wait_queue_wake_all(&sk->waitq);
}

/* Bound-address registry */

static int sock_bound_lookup(const sockaddr_un_t *addr, uint32_t addrlen, int abstract, socket_t **out)
{
    spin_lock(&sock_bound_lock);
    for (int i = 0; i < SOCK_BOUND_MAX; i++) {
        if (sock_bound_tab[i].sk == NULL) continue;
        if (sock_bound_tab[i].abstract != abstract) continue;

        if (abstract) {
            /*
             * Abstract names are byte strings, so their supplied length is
             * part of the address.
             */
            if (sock_bound_tab[i].addrlen == addrlen
                && memcmp(sock_bound_tab[i].addr.sun_path, addr->sun_path, addrlen - sizeof(uint16_t)) == 0) {
                *out = sock_bound_tab[i].sk;
                spin_unlock(&sock_bound_lock);
                return EOK;
            }
        } else {
            /*
             * Pathname addresses are identified by sun_path.  Applications
             * may use either the minimal sockaddr length or a larger structure
             * containing the same NUL-terminated path, so unlike abstract
             * names addrlen must not participate in this match.
             */
            if (strncmp(sock_bound_tab[i].addr.sun_path, addr->sun_path, UNIX_PATH_MAX) == 0) {
                /* Also verify the saved path length matches */
                size_t a = strlen(sock_bound_tab[i].addr.sun_path);
                size_t b = strnlen_local(addr->sun_path, UNIX_PATH_MAX);
                if (a == b) {
                    *out = sock_bound_tab[i].sk;
                    spin_unlock(&sock_bound_lock);
                    return EOK;
                }
            }
        }
    }
    spin_unlock(&sock_bound_lock);
    *out = NULL;
    return -EADDRNOTAVAIL;
}

static int sock_bound_add(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen, int abstract)
{
    spin_lock(&sock_bound_lock);

    /* Check for duplicates */
    for (int i = 0; i < SOCK_BOUND_MAX; i++) {
        if (sock_bound_tab[i].sk == NULL) continue;
        if (sock_bound_tab[i].abstract != abstract) continue;

        if (abstract) {
            if (sock_bound_tab[i].addrlen == addrlen
                && memcmp(sock_bound_tab[i].addr.sun_path, addr->sun_path, addrlen - sizeof(uint16_t)) == 0) {
                spin_unlock(&sock_bound_lock);
                return -EADDRINUSE;
            }
        } else {
            if (strncmp(sock_bound_tab[i].addr.sun_path, addr->sun_path, UNIX_PATH_MAX) == 0) {
                size_t a = strlen(sock_bound_tab[i].addr.sun_path);
                size_t b = strnlen_local(addr->sun_path, UNIX_PATH_MAX);
                if (a == b) {
                    spin_unlock(&sock_bound_lock);
                    return -EADDRINUSE;
                }
            }
        }
    }

    /* Find free slot */
    for (int i = 0; i < SOCK_BOUND_MAX; i++) {
        if (sock_bound_tab[i].sk == NULL) {
            /*
             * Pathname sockaddr lengths commonly omit the trailing NUL.  A
             * reused slot must therefore be cleared before copying, or bytes
             * from the previous address become a fake suffix and make later
             * connect(2) lookups miss the listener.
             */
            memset(&sock_bound_tab[i], 0, sizeof(sock_bound_tab[i]));
            sock_bound_tab[i].sk       = sk;
            sock_bound_tab[i].addrlen  = addrlen;
            sock_bound_tab[i].abstract = abstract;
            memcpy(&sock_bound_tab[i].addr, addr, addrlen < sizeof(sockaddr_un_t) ? addrlen : sizeof(sockaddr_un_t));
            spin_unlock(&sock_bound_lock);
            return EOK;
        }
    }

    spin_unlock(&sock_bound_lock);
    return -ENOMEM;
}

static void sock_bound_remove(socket_t *sk)
{
    spin_lock(&sock_bound_lock);
    for (int i = 0; i < SOCK_BOUND_MAX; i++) {
        if (sock_bound_tab[i].sk == sk) {
            memset(&sock_bound_tab[i], 0, sizeof(sock_bound_tab[i]));
            break;
        }
    }
    spin_unlock(&sock_bound_lock);
}

size_t socket_format_unix_table(char *buffer, size_t capacity)
{
    if (!buffer || !capacity) return 0;
    size_t used = 0;
    int    n    = snprintf(buffer, capacity, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    if (n < 0) return 0;
    used = (size_t)n < capacity ? (size_t)n : capacity - 1;

    spin_lock(&sock_bound_lock);
    for (int i = 0; i < SOCK_BOUND_MAX && used < capacity - 1; i++) {
        sock_bound_t *bound = &sock_bound_tab[i];
        socket_t     *sk    = bound->sk;
        if (!sk) continue;

        char   path[UNIX_PATH_MAX + 1];
        size_t path_len = bound->addrlen > sizeof(uint16_t) ? bound->addrlen - sizeof(uint16_t) : 0;
        if (path_len > UNIX_PATH_MAX) path_len = UNIX_PATH_MAX;
        if (bound->abstract) {
            path[0]         = '@';
            size_t copy_len = path_len > 0 ? path_len - 1 : 0;
            if (copy_len > UNIX_PATH_MAX - 1) copy_len = UNIX_PATH_MAX - 1;
            memcpy(path + 1, bound->addr.sun_path + 1, copy_len);
            path[copy_len + 1] = '\0';
        } else {
            size_t copy_len = strnlen_local(bound->addr.sun_path, UNIX_PATH_MAX);
            memcpy(path, bound->addr.sun_path, copy_len);
            path[copy_len] = '\0';
        }

        uint32_t flags = sk->state == SOCK_STATE_LISTENING ? 0x00010000U : 0;
        uint32_t state = sk->state == SOCK_STATE_CONNECTED ? 3U : 1U;
        uint64_t inode = sk->bound_node ? sk->bound_node->inode : sk->node ? sk->node->inode : 0;
        n = snprintf(buffer + used, capacity - used, "%016llx: %08x %08x %08x %04x %02x %llu %s\n", (unsigned long long)(uintptr_t)sk,
                     sk->refcount, 0U, flags, sk->type, state, (unsigned long long)inode, path);
        if (n < 0) break;
        size_t appended = (size_t)n;
        if (appended >= capacity - used) {
            used = capacity - 1;
            break;
        }
        used += appended;
    }
    spin_unlock(&sock_bound_lock);
    buffer[used] = '\0';
    return used;
}

/* UNIX address parsing */

static int unix_addr_parse(const sockaddr_un_t *addr, uint32_t addrlen, int *is_abstract)
{
    if (!addr || !is_abstract) return -EINVAL;
    if (addrlen < sizeof(uint16_t)) return -EINVAL;
    if (addr->sun_family != AF_UNIX) return -EAFNOSUPPORT;

    if (addrlen > sizeof(sockaddr_un_t)) return -EINVAL;

    if (addrlen == sizeof(uint16_t)) {
        /* Unnamed / autobind address */
        *is_abstract = 0;
        return EOK;
    }

    if (addr->sun_path[0] == '\0') {
        *is_abstract = 1;
    } else {
        *is_abstract = 0;
    }

    return EOK;
}

/* Socket lifecycle */

static socket_t *socket_alloc(uint16_t family, uint16_t type, uint16_t protocol)
{
    socket_t *sk;

    /* Validate family - Netlink handled by netlink layer */
    if (family == AF_NETLINK) {
        sk = netlink_sock_alloc(protocol);
        if (sk) sk->type = type;
        return sk;
    }
    if (family != AF_UNIX && family != AF_LOCAL) return NULL;

    /* Validate type */
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) return NULL;

    sk = calloc(1, sizeof(socket_t));
    if (!sk) return NULL;

    sk->state    = SOCK_STATE_UNCONNECTED;
    sk->family   = family;
    sk->type     = type;
    sk->protocol = protocol;
    sk->flags    = 0;
    wait_queue_init(&sk->waitq);

    if (sock_buf_init(&sk->recv_buf, SOCK_BUF_SIZE) != EOK) {
        free(sk);
        return NULL;
    }
    sk->sndbuf      = SOCK_BUF_SIZE;
    sk->rcvbuf      = SOCK_BUF_SIZE;
    sk->rcvlowat    = 1;
    sk->sndlowat    = 1;
    sk->linger_on   = 0;
    sk->linger_time = 0;
    sk->passcred    = 0;
    sk->reuseaddr   = 0;
    sk->so_error    = 0;
    sk->refcount    = 1;

    /*
     * Every unbound AF_UNIX socket has an unnamed local address.  Linux
     * reports that address as just sa_family_t (length 2); it is not an
     * abstract address and must never consume a slot in the bound-name
     * table.  Initialize it at socket creation so connect(), socketpair(),
     * getsockname(), and datagram sender metadata all share one consistent
     * representation.
     */
    sk->local_addr.ss_family = AF_UNIX;
    sk->local_addr_len       = sizeof(sa_family_t);

    /* Set credentials from current process */
    {
        process_t *proc = process_current();
        if (proc) {
            sk->pid = (uint32_t)(proc->task ? proc->task->pid : 0);
            sk->uid = proc->uid;
            sk->gid = proc->gid;
        }
    }

    /*
     * Set polymorphic operations: dgram/seqpacket/stream all route reads and
     * writes through their type-specific paths.
     */
    sk->socket_read  = NULL;
    sk->socket_write = NULL;
    sk->socket_poll  = NULL;
    sk->socket_close = NULL;

    return sk;
}

static socket_t *inet_socket_alloc(uint16_t family, uint16_t type, uint16_t protocol, uint32_t flags, void *context)
{
    socket_t *sk = calloc(1, sizeof(socket_t));
    if (!sk) return NULL;
    sk->state    = SOCK_STATE_UNCONNECTED;
    sk->family   = family;
    sk->type     = type;
    sk->protocol = protocol;
    sk->flags    = flags;
    wait_queue_init(&sk->waitq);
    sk->sndbuf   = SOCK_BUF_SIZE;
    sk->rcvbuf   = SOCK_BUF_SIZE;
    sk->rcvlowat = 1;
    sk->sndlowat = 1;
    sk->refcount = 1;
    sk->priv     = context;
    return sk;
}

static int socket_copy_address_to_user(sockaddr_t *addr, uint32_t *addrlen, const sockaddr_t *kaddr, uint32_t kaddrlen)
{
    uint32_t userlen;
    uint32_t copylen;

    if (!addr && !addrlen) return EOK;
    if (!addr || !addrlen) return -EFAULT;
    if (copy_from_user(&userlen, addrlen, sizeof(userlen))) return -EFAULT;
    copylen = userlen < kaddrlen ? userlen : kaddrlen;
    if (copylen && copy_to_user(addr, kaddr, copylen)) return -EFAULT;
    if (copy_to_user(addrlen, &kaddrlen, sizeof(kaddrlen))) return -EFAULT;
    return EOK;
}

static void socket_drop_rights(socket_t *sk)
{
    process_file_t *files[SOCK_RIGHTS_MAX];
    size_t          count = 0;

    if (!sk) return;
    spin_lock(&sk->lock);
    while (sk->rights_count > 0 && count < SOCK_RIGHTS_MAX) {
        files[count++]              = sk->rights[sk->rights_head];
        sk->rights[sk->rights_head] = NULL;
        sk->rights_head             = (uint16_t)((sk->rights_head + 1U) % SOCK_RIGHTS_MAX);
        sk->rights_count--;
    }
    sk->rights_head = sk->rights_tail = 0;
    spin_unlock(&sk->lock);

    /*
     * A queued SCM_RIGHTS descriptor is an in-flight reference.  Release it
     * outside the socket lock because the final file callback may close a
     * socket and take socket/VFS locks itself.
     */
    for (size_t i = 0; i < count; i++) process_file_put_transfer(files[i]);
}

static size_t socket_take_rights(socket_t *sk, process_file_t **files, size_t capacity)
{
    size_t count = 0;
    if (!sk || !files || capacity == 0) return 0;

    spin_lock(&sk->lock);
    while (sk->rights_count > 0 && count < capacity) {
        files[count++]              = sk->rights[sk->rights_head];
        sk->rights[sk->rights_head] = NULL;
        sk->rights_head             = (uint16_t)((sk->rights_head + 1U) % SOCK_RIGHTS_MAX);
        sk->rights_count--;
    }
    if (sk->rights_count == 0) sk->rights_head = sk->rights_tail = 0;
    spin_unlock(&sk->lock);
    return count;
}

static void socket_free(socket_t *sk)
{
    if (!sk) return;

    sk->refcount--;
    if (sk->refcount > 0) return;

    if ((sk->family == AF_INET || sk->family == AF_INET6) && sk->priv) {
        const struct inet_backend_ops *ops = inet_backend_get();
        if (ops && ops->close) ops->close(sk->priv);
        sk->priv = NULL;
    }

    /* Netlink cleanup */
    if (sk->family == AF_NETLINK && sk->priv) netlink_close(sk);

    /* Remove from bound registry */
    sock_bound_remove(sk);

    /*
     * A pathname socket inode persists after close, as on Linux.  Drop only
     * the endpoint's retained VFS reference; unlink(2) owns namespace removal.
     */
    if (sk->bound_node) {
        vfs_close(sk->bound_node);
        sk->bound_node = NULL;
    }

    /* Wake any blocked tasks */
    sock_blocked_wake_all(sk);

    /* Free peer reference */
    if (sk->peer) {
        sk->peer->peer = NULL;
        sock_blocked_wake_all(sk->peer);
        socket_free(sk->peer);
    }

    /* Free accept queue */
    if (sk->accept_queue) {
        for (uint32_t i = 0; i < sk->accept_queue_len; i++) {
            if (sk->accept_queue[i]) socket_free(sk->accept_queue[i]);
        }
        free(sk->accept_queue);
        sk->accept_queue = NULL;
    }

    /* Free buffers */
    socket_drop_rights(sk);
    sock_buf_free(&sk->recv_buf);
    sock_buf_free(&sk->send_buf);

    /* Close VFS node if present */
    if (sk->node) {
        vfs_close(sk->node);
        sk->node = NULL;
    }

    free(sk);
}

static void socket_ref(socket_t *sk)
{
    if (sk) sk->refcount++;
}

static void socket_unref(socket_t *sk)
{
    if (sk) socket_free(sk);
}

/* Socket fd installation */

int socket_fd_install(socket_t *sk)
{
    return socket_fd_install_flags(sk, 0);
}

int socket_fd_install_flags(socket_t *sk, uint64_t fd_flags)
{
    process_t *proc;
    vfs_node_t node;
    int        fd;

    if (!sk) return -EINVAL;

    proc = process_current();
    if (!proc) return -ESRCH;

    node = vfs_node_alloc(NULL, "[socket]");
    if (!node) {
        plogk("socket: Socket VFS node allocation failed.\n");
        return -ENOMEM;
    }

    node->type   = file_socket;
    node->handle = sk;
    node->fsid   = socket_fsid;
    node->size   = 0;
    node->mode   = 0600;

    sk->node = node;
    socket_ref(sk);
    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        if (ops && ops->set_event_callback) ops->set_event_callback(sk->priv, socket_inet_event, sk);
    }

    fd = process_fd_install(proc, node, O_RDWR | fd_flags);
    if (fd < 0) {
        sk->node = NULL;
        vfs_close(node);
        return fd;
    }

    return fd;
}

static int socket_fd_nonblock(int fd)
{
    process_t      *proc = process_current();
    process_file_t *file = proc ? process_fd_get(proc, fd) : NULL;
    if (!file) return 0;
    spin_lock(&file->lock);
    int nonblock = (file->flags & O_NONBLOCK) != 0;
    spin_unlock(&file->lock);
    process_file_put(file);
    return nonblock;
}

/*
 * socket_from_fd - find a socket by fd in the current process
 * NOTE: returns a weak pointer (no refcount bump).
 * The caller must ensure the socket stays alive during use.
 */

socket_t *socket_from_fd(int fd)
{
    process_t      *proc;
    process_file_t *file;
    socket_t       *sk = NULL;

    proc = process_current();
    if (!proc) return NULL;

    spin_lock(&proc->fd_lock);

    if (fd < 0 || fd >= PROCESS_MAX_FD) goto out;

    file = proc->fds[fd];
    if (!file || !file->node) goto out;

    if (file->node->type != file_socket) goto out;

    sk = (socket_t *)file->node->handle;

out:
    spin_unlock(&proc->fd_lock);
    return sk;
}

/* UNIX autobind */

static int unix_autobind(socket_t *sk)
{
    sockaddr_un_t addr;
    int           ret;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /* Generate an abstract address using sched_ticks + pid */
    {
        uint64_t tick = sched_ticks();
        /* Format: \0unix-%08x-%08x */
        addr.sun_path[0] = '\0';

        /* Simple hex encoding of pid and tick */
        uint32_t pid = sk->pid;
        uint32_t tkl = (uint32_t)(tick & 0xFFFFFFFFU);
        char    *p   = addr.sun_path + 1;
        int      rem = UNIX_PATH_MAX - 1;

        /* Write "unix-" prefix */
        const char *pfx = "unix-";
        while (*pfx && rem > 0) {
            *p++ = *pfx++;
            rem--;
        }

        /* Hex encode pid */
        for (int shift = 28; shift >= 0 && rem > 0; shift -= 4) {
            uint8_t nib = (pid >> (uint32_t)shift) & 0xF;
            *p++        = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
            rem--;
        }

        if (rem > 0) {
            *p++ = '-';
            rem--;
        }

        /* Hex encode tick */
        for (int shift = 28; shift >= 0 && rem > 0; shift -= 4) {
            uint8_t nib = (tkl >> (uint32_t)shift) & 0xF;
            *p++        = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
            rem--;
        }

        addr.sun_path[UNIX_PATH_MAX - 1] = '\0';
    }

    uint32_t alen = (uint32_t)(sizeof(uint16_t) + 1 + strnlen_local(addr.sun_path + 1, UNIX_PATH_MAX - 1));

    ret = sock_bound_add(sk, &addr, alen, 1);
    if (ret != EOK) return ret;

    memcpy(&sk->local_addr, &addr, sizeof(addr));
    sk->local_addr_len = alen;

    return EOK;
}

/* UNIX bind */

static int unix_bind(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen)
{
    int abstract;
    int ret;

    if (sk->state != SOCK_STATE_UNCONNECTED) return -EINVAL;

    ret = unix_addr_parse(addr, addrlen, &abstract);
    if (ret != EOK) return ret;

    /* Unnamed address -> autobind */
    if (addrlen == sizeof(uint16_t)) return unix_autobind(sk);

    if (!abstract) {
        /* Pathname socket: create VFS entry */
        const char *path = addr->sun_path;
        vfs_node_t  file_node;

        /* Check path length */
        if (strnlen_local(path, UNIX_PATH_MAX) >= UNIX_PATH_MAX) return -ENAMETOOLONG;

        /*
         * AF_UNIX pathname ownership follows the filesystem namespace.  An
         * existing inode is EADDRINUSE even with SO_REUSEADDR; applications
         * that own a stale entry must unlink it explicitly.
         */
        ret = vfs_mkfile(path);
        if (ret != EOK) {
            if (ret != -EEXIST && ret != -EACCES && ret != -EROFS) plogk("socket: Unix bind failed to create %s (%d)\n", path, ret);
            return ret == -EEXIST ? -EADDRINUSE : ret;
        }

        file_node = vfs_open(path);
        if (!file_node) {
            plogk("socket: Unix bind could not open %s after creation.\n", path);
            return -EIO;
        }
        ret = tmpfs_set_node_type(file_node, file_socket);
        if (ret != EOK) {
            vfs_delete(file_node);
            vfs_close(file_node);
            return ret;
        }
        sk->bound_node = file_node;
    }

    /* Register in bound table */
    ret = sock_bound_add(sk, addr, addrlen, abstract);
    if (ret != EOK) {
        if (!abstract && sk->bound_node) {
            vfs_delete(sk->bound_node);
            vfs_close(sk->bound_node);
            sk->bound_node = NULL;
        }
        return ret;
    }

    memcpy(&sk->local_addr, addr, sizeof(sockaddr_un_t));
    sk->local_addr_len = addrlen;

    return EOK;
}

/* UNIX listen */

static int unix_listen(socket_t *sk, uint32_t backlog)
{
    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    if (sk->state == SOCK_STATE_CONNECTED) return -EISCONN;

    spin_lock(&sk->lock);

    if (sk->state == SOCK_STATE_LISTENING) {
        /* Already listening - just update backlog */
        if (backlog > SOCK_ACCEPT_QUEUE_MAX) backlog = SOCK_ACCEPT_QUEUE_MAX;
        sk->backlog = backlog;
        spin_unlock(&sk->lock);
        return EOK;
    }

    sk->state = SOCK_STATE_LISTENING;

    if (backlog == 0) backlog = SOCK_ACCEPT_QUEUE_INIT;
    if (backlog > SOCK_ACCEPT_QUEUE_MAX) backlog = SOCK_ACCEPT_QUEUE_MAX;

    sk->accept_queue = calloc(backlog, sizeof(socket_t *));
    if (!sk->accept_queue) {
        plogk("socket: Unix listen accept queue allocation failed (backlog %u)\n", (unsigned)backlog);
        sk->state = SOCK_STATE_UNCONNECTED;
        spin_unlock(&sk->lock);
        return -ENOMEM;
    }

    sk->accept_queue_cap = backlog;
    sk->accept_queue_len = 0;
    sk->backlog          = backlog;

    spin_unlock(&sk->lock);
    return EOK;
}

/* UNIX stream connect */

static int unix_stream_connect(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen)
{
    socket_t *listener = NULL;
    int       abstract;
    int       ret;

    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    ret = unix_addr_parse(addr, addrlen, &abstract);
    if (ret != EOK) return ret;

    /* Look up the listening socket */
    ret = sock_bound_lookup(addr, addrlen, abstract, &listener);
    if (ret != EOK || !listener) return -ECONNREFUSED;

    spin_lock(&listener->lock);

    if (listener->state != SOCK_STATE_LISTENING) {
        spin_unlock(&listener->lock);
        return -ECONNREFUSED;
    }
    if (listener->type != sk->type) {
        spin_unlock(&listener->lock);
        return -EPROTOTYPE;
    }

    /* Check backlog */
    if (listener->accept_queue_len >= listener->accept_queue_cap) {
        spin_unlock(&listener->lock);
        return -ECONNREFUSED;
    }

    /* Create a new server-side socket */
    socket_t *server = calloc(1, sizeof(socket_t));
    if (!server) {
        plogk("socket: Unix stream connect server socket allocation failed.\n");
        spin_unlock(&listener->lock);
        return -ENOMEM;
    }

    server->state    = SOCK_STATE_CONNECTED;
    server->family   = sk->family;
    server->type     = sk->type;
    server->protocol = sk->protocol;
    server->flags    = 0;
    server->refcount = 1;
    wait_queue_init(&server->waitq);

    if (sock_buf_init(&server->recv_buf, SOCK_BUF_SIZE) != EOK) {
        plogk("socket: Unix stream connect recv buffer allocation failed.\n");
        free(server);
        spin_unlock(&listener->lock);
        return -ENOMEM;
    }
    if (sock_buf_init(&server->send_buf, SOCK_BUF_SIZE) != EOK) {
        plogk("socket: Unix stream connect send buffer allocation failed.\n");
        sock_buf_free(&server->recv_buf);
        free(server);
        spin_unlock(&listener->lock);
        return -ENOMEM;
    }
    server->sndbuf   = SOCK_BUF_SIZE;
    server->rcvbuf   = SOCK_BUF_SIZE;
    server->rcvlowat = 1;
    server->sndlowat = 1;

    /* Copy credentials from listener */
    server->pid = listener->pid;
    server->uid = listener->uid;
    server->gid = listener->gid;

    /* Link the two sockets */
    server->peer = sk;
    socket_ref(sk);
    sk->peer = server;
    socket_ref(server);

    /* Copy addresses */
    memcpy(&server->local_addr, &listener->local_addr, sizeof(sockaddr_un_t));
    server->local_addr_len = listener->local_addr_len;
    memcpy(&server->peer_addr, &sk->local_addr, sizeof(sockaddr_un_t));
    server->peer_addr_len = sk->local_addr_len;

    memcpy(&sk->peer_addr, &listener->local_addr, sizeof(sockaddr_un_t));
    sk->peer_addr_len = listener->local_addr_len;

    sk->state = SOCK_STATE_CONNECTED;

    /* Add to accept queue */
    listener->accept_queue[listener->accept_queue_len] = server;
    listener->accept_queue_len++;
    socket_ref(server);

    spin_unlock(&listener->lock);

    sock_blocked_wake(listener);
    if (listener->node) vfs_poll_notify(listener->node, 0x001);

    return EOK;
}

/* UNIX accept */

static int unix_accept(socket_t *sk, sockaddr_un_t *addr, uint32_t *addrlen, int flags)
{
    socket_t *client;
    int       is_nonblock;

    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    is_nonblock = (flags & SOCK_NONBLOCK) || (sk->flags & SOCK_NONBLOCK);

    spin_lock(&sk->lock);

    if (sk->state != SOCK_STATE_LISTENING) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    while (sk->accept_queue_len == 0) {
        if (is_nonblock) {
            spin_unlock(&sk->lock);
            return -EAGAIN;
        }
        if (sk->shutdown_mask) {
            spin_unlock(&sk->lock);
            return -ECONNABORTED;
        }
        sock_blocked_register(sk, current_task());
        spin_unlock(&sk->lock);
        wait_queue_sleep();
        spin_lock(&sk->lock);
        sock_blocked_unregister(sk);
    }

    /* Dequeue first client */
    client = sk->accept_queue[0];
    sk->accept_queue_len--;

    /* Shift remaining entries */
    for (uint32_t i = 0; i < sk->accept_queue_len; i++) sk->accept_queue[i] = sk->accept_queue[i + 1];
    sk->accept_queue[sk->accept_queue_len] = NULL;

    spin_unlock(&sk->lock);

    /* Copy peer address to user */
    if (addr && addrlen) {
        if (client->peer_addr_len > 0) {
            int ret = socket_copy_address_to_user((sockaddr_t *)addr, addrlen, (sockaddr_t *)&client->peer_addr, client->peer_addr_len);
            if (ret < 0) {
                socket_unref(client);
                return ret;
            }
        }
    }

    /* Install the client socket into the current process */
    client->flags = (uint32_t)(flags & SOCK_NONBLOCK);
    return socket_fd_install_flags(client, ((flags & SOCK_CLOEXEC) ? O_CLOEXEC : 0) | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
}

/* UNIX stream send */

static int unix_stream_send_rights(socket_t *sk, const void *buf, size_t len, int flags, process_file_t **rights, size_t rights_count)
{
    socket_t *peer;
    int       is_nonblock;
    uint32_t  total_written = 0;
    int       ret;

    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);

    spin_lock(&sk->lock);

    if (sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_WR)) {
        spin_unlock(&sk->lock);
        return -EPIPE;
    }
    if (sk->state != SOCK_STATE_CONNECTED) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    peer = sk->peer;
    if (!peer) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    socket_ref(peer);
    spin_unlock(&sk->lock);

    spin_lock(&peer->lock);

    if (peer->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD)) {
        spin_unlock(&peer->lock);
        socket_unref(peer);
        return -EPIPE;
    }

    if (rights_count > (size_t)(SOCK_RIGHTS_MAX - peer->rights_count)) {
        spin_unlock(&peer->lock);
        socket_unref(peer);
        return -ENOBUFS;
    }

    while (total_written < len) {
        uint32_t chunk = (uint32_t)(len - total_written);
        uint32_t space = sock_buf_space(&peer->recv_buf);

        if (space == 0) {
            if (is_nonblock) {
                if (total_written == 0) {
                    spin_unlock(&peer->lock);
                    socket_unref(peer);
                    return -EAGAIN;
                }
                break;
            }
            /* Block until peer reads some data */
            sock_blocked_register(sk, current_task());
            spin_unlock(&peer->lock);
            wait_queue_sleep();
            spin_lock(&peer->lock);
            sock_blocked_unregister(sk);

            if (peer->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD)) {
                spin_unlock(&peer->lock);
                socket_unref(peer);
                return -EPIPE;
            }
            continue;
        }

        if (chunk > space) chunk = space;

        /* For SEQPACKET, write the entire message or nothing? */
        /* We'll write as much as we can; upper layer handles boundaries */

        uint32_t written = sock_buf_write(&peer->recv_buf, (const uint8_t *)buf + total_written, chunk);
        total_written += written;

        if (written < chunk) break;
    }

    /*
     * Ancillary descriptors are published under the same lock as the first
     * bytes of this sendmsg.  The refs in rights[] become owned by the peer's
     * receive queue only after at least one byte has been accepted.
     */
    if (total_written > 0) {
        for (size_t i = 0; i < rights_count; i++) {
            peer->rights[peer->rights_tail] = rights[i];
            peer->rights_tail               = (uint16_t)((peer->rights_tail + 1U) % SOCK_RIGHTS_MAX);
            peer->rights_count++;
        }
    }

    spin_unlock(&peer->lock);

    /* Wake the peer if it's blocked on recv */
    sock_blocked_wake(peer);
    if (peer->node) vfs_poll_notify(peer->node, 0x001);

    socket_unref(peer);

    ret = (int)total_written;
    if (ret == 0 && !is_nonblock) ret = -EPIPE;
    return ret;
}

static int unix_stream_send(socket_t *sk, const void *buf, size_t len, int flags)
{
    return unix_stream_send_rights(sk, buf, len, flags, NULL, 0);
}

/* UNIX stream recv */

static int unix_stream_recv(socket_t *sk, void *buf, size_t len, int flags)
{
    int       is_nonblock;
    int       peek;
    uint32_t  total_read = 0;
    socket_t *peer;
    int       ret;

    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);
    peek        = (flags & MSG_PEEK) ? 1 : 0;

    spin_lock(&sk->lock);
    peer = sk->peer;

    while (total_read < len) {
        uint32_t avail = sock_buf_available(&sk->recv_buf);

        if (avail == 0) {
            if (total_read > 0) break;
            if (sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD)) {
                spin_unlock(&sk->lock);
                return 0;
            }
            if (sk->state != SOCK_STATE_CONNECTED) {
                if (sk->state == SOCK_STATE_DISCONNECTING) {
                    spin_unlock(&sk->lock);
                    return 0;
                }
                spin_unlock(&sk->lock);
                return -ENOTCONN;
            }
            if (is_nonblock) {
                spin_unlock(&sk->lock);
                return -EAGAIN;
            }
            /* Check if peer is still connected */
            if (!peer || peer->state == SOCK_STATE_DISCONNECTING) {
                spin_unlock(&sk->lock);
                return 0;
            }
            sock_blocked_register(sk, current_task());
            spin_unlock(&sk->lock);
            wait_queue_sleep();
            spin_lock(&sk->lock);
            sock_blocked_unregister(sk);
            peer = sk->peer;
            continue;
        }

        uint32_t chunk = (uint32_t)(len - total_read);
        if (chunk > avail) chunk = avail;

        uint32_t rd;
        if (peek) {
            rd = sock_buf_peek(&sk->recv_buf, (uint8_t *)buf + total_read, chunk);
        } else {
            rd = sock_buf_read(&sk->recv_buf, (uint8_t *)buf + total_read, chunk);
        }
        total_read += rd;

        if (rd < chunk) break;
    }

    spin_unlock(&sk->lock);

    if (!peek && total_read > 0 && peer) {
        sock_blocked_wake(peer);
        if (peer->node) vfs_poll_notify(peer->node, 0x004);
    }

    ret = (int)total_read;
    if (ret == 0 && !(flags & MSG_PEEK) && !(sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD))) return 0;
    return ret;
}

/*
 * SOCK_SEQPACKET preserves record boundaries.  Store each record as a native
 * 32-bit length followed by its bytes in the peer's private ring.  The ring is
 * protected by peer->lock, so publishing the header and payload is atomic to
 * readers.
 */
static int unix_seqpacket_send(socket_t *sk, const void *buf, size_t len, int flags)
{
    const uint32_t header_size = sizeof(uint32_t);
    int            is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);
    if (len > UINT32_MAX || len > SOCK_BUF_MAX - header_size) return -EMSGSIZE;

    spin_lock(&sk->lock);
    if (sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_WR)) {
        spin_unlock(&sk->lock);
        return -EPIPE;
    }
    if (sk->state != SOCK_STATE_CONNECTED || !sk->peer) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }
    socket_t *peer = sk->peer;
    socket_ref(peer);
    spin_unlock(&sk->lock);

    spin_lock(&peer->lock);
    uint32_t needed = header_size + (uint32_t)len;
    if (needed > peer->recv_buf.capacity) {
        spin_unlock(&peer->lock);
        socket_unref(peer);
        return -EMSGSIZE;
    }
    while (sock_buf_space(&peer->recv_buf) < needed) {
        if (peer->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD) || peer->state == SOCK_STATE_DISCONNECTING) {
            spin_unlock(&peer->lock);
            socket_unref(peer);
            return -EPIPE;
        }
        if (is_nonblock) {
            spin_unlock(&peer->lock);
            socket_unref(peer);
            return -EAGAIN;
        }
        sock_blocked_register(sk, current_task());
        spin_unlock(&peer->lock);
        wait_queue_sleep();
        spin_lock(&peer->lock);
        sock_blocked_unregister(sk);
    }

    uint32_t record_len = (uint32_t)len;
    if (sock_buf_write(&peer->recv_buf, &record_len, header_size) != header_size
        || (record_len && sock_buf_write(&peer->recv_buf, buf, record_len) != record_len)) {
        /*
         * Space was reserved while holding the lock; reaching this path means
         * ring corruption rather than a short write.
         */
        spin_unlock(&peer->lock);
        socket_unref(peer);
        return -EIO;
    }
    spin_unlock(&peer->lock);

    sock_blocked_wake(peer);
    if (peer->node) vfs_poll_notify(peer->node, 0x001);
    socket_unref(peer);
    return (int)len;
}

static int unix_seqpacket_recv(socket_t *sk, void *buf, size_t len, int flags, int *message_flags, size_t *record_size)
{
    const uint32_t header_size = sizeof(uint32_t);
    int            is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);
    int            peek        = (flags & MSG_PEEK) != 0;
    socket_t      *peer;
    uint32_t       packet_len;

    if (message_flags) *message_flags = 0;
    if (record_size) *record_size = 0;

    spin_lock(&sk->lock);
    for (;;) {
        uint32_t available = sock_buf_available(&sk->recv_buf);
        if (available >= header_size) {
            if (sock_buf_peek(&sk->recv_buf, &packet_len, header_size) != header_size || packet_len > sk->recv_buf.capacity - header_size) {
                spin_unlock(&sk->lock);
                return -EIO;
            }
            if (available >= header_size + packet_len) break;
        }

        peer = sk->peer;
        if (sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD) || sk->state == SOCK_STATE_DISCONNECTING || !peer) {
            spin_unlock(&sk->lock);
            return 0;
        }
        if (sk->state != SOCK_STATE_CONNECTED) {
            spin_unlock(&sk->lock);
            return -ENOTCONN;
        }
        if (is_nonblock) {
            spin_unlock(&sk->lock);
            return -EAGAIN;
        }
        sock_blocked_register(sk, current_task());
        spin_unlock(&sk->lock);
        wait_queue_sleep();
        spin_lock(&sk->lock);
        sock_blocked_unregister(sk);
    }

    size_t copied = len < packet_len ? len : packet_len;
    if (copied && sock_buf_peek_at(&sk->recv_buf, header_size, buf, (uint32_t)copied) != copied) {
        spin_unlock(&sk->lock);
        return -EIO;
    }
    if (!peek) sock_buf_discard(&sk->recv_buf, header_size + packet_len);
    peer = sk->peer;
    spin_unlock(&sk->lock);

    if (message_flags) {
        *message_flags |= MSG_EOR;
        if (copied < packet_len) *message_flags |= MSG_TRUNC;
    }
    if (record_size) *record_size = packet_len;
    if (!peek && peer) {
        sock_blocked_wake(peer);
        if (peer->node) vfs_poll_notify(peer->node, 0x004);
    }
    return (int)copied;
}

/* UNIX datagram send */

static int unix_dgram_send(socket_t *sk, const void *buf, size_t len, const sockaddr_un_t *addr, uint32_t addrlen, int flags)
{
    socket_t *dest = NULL;
    int       abstract;
    int       ret;
    int       is_nonblock;

    if (sk->type != SOCK_DGRAM) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);

    const uint32_t header_size = sizeof(uint32_t) + sizeof(sockaddr_un_t) + sizeof(ucred_t);
    if (len > SOCK_BUF_MAX - header_size) return -EMSGSIZE;

    /* If no destination address, use peer address (connected dgram) */
    if (addr && addrlen > 0) {
        ret = unix_addr_parse(addr, addrlen, &abstract);
        if (ret != EOK) return ret;

        ret = sock_bound_lookup(addr, addrlen, abstract, &dest);
        if (ret != EOK || !dest) return -ECONNREFUSED;
    } else if (sk->peer) {
        dest = sk->peer;
    } else {
        return -EDESTADDRREQ;
    }

    if (dest == sk) return -EINVAL;

    spin_lock(&dest->lock);
    uint32_t total = header_size + (uint32_t)len;
    if (total > dest->recv_buf.capacity) {
        spin_unlock(&dest->lock);
        return -EMSGSIZE;
    }
    while (sock_buf_space(&dest->recv_buf) < total) {
        if (dest->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD) || dest->state == SOCK_STATE_DISCONNECTING) {
            spin_unlock(&dest->lock);
            return -ECONNREFUSED;
        }
        if (is_nonblock) {
            spin_unlock(&dest->lock);
            return -EAGAIN;
        }
        sock_blocked_register(dest, current_task());
        spin_unlock(&dest->lock);
        wait_queue_sleep();
        spin_lock(&dest->lock);
        sock_blocked_unregister(dest);
    }

    ucred_t    sender  = {0};
    process_t *process = process_current();
    if (process) {
        sender.pid = (uint32_t)(process->task ? process->task->tgid : 0);
        sender.uid = process->uid;
        sender.gid = process->gid;
    }
    uint32_t msg_len = (uint32_t)len;
    uint32_t written = 0;
    if (sock_buf_write(&dest->recv_buf, &msg_len, sizeof(msg_len)) == sizeof(msg_len)
        && sock_buf_write(&dest->recv_buf, &sk->local_addr, sizeof(sockaddr_un_t)) == sizeof(sockaddr_un_t)
        && sock_buf_write(&dest->recv_buf, &sender, sizeof(sender)) == sizeof(sender))
        written = msg_len ? sock_buf_write(&dest->recv_buf, buf, msg_len) : 0;

    spin_unlock(&dest->lock);

    /* Wake destination */
    sock_blocked_wake(dest);
    if (dest->node) vfs_poll_notify(dest->node, 0x001);

    if (written < (uint32_t)len) {
        plogk("socket: Unix datagram send short write (%u of %u bytes)\n", (unsigned)written, (unsigned)len);
        return -EIO;
    }

    return (int)written;
}

/* UNIX datagram recv */

static int unix_dgram_recv(socket_t *sk, void *buf, size_t len, sockaddr_un_t *addr, uint32_t *addrlen, int flags, ucred_t *credentials,
                           int *message_flags, size_t *record_size)
{
    int            is_nonblock;
    int            peek;
    uint32_t       msg_len;
    sockaddr_un_t  sender_addr;
    ucred_t        sender_credentials;
    const uint32_t header_size = sizeof(uint32_t) + sizeof(sockaddr_un_t) + sizeof(ucred_t);

    if (sk->type != SOCK_DGRAM) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);
    peek        = (flags & MSG_PEEK) ? 1 : 0;
    if (message_flags) *message_flags = 0;
    if (record_size) *record_size = 0;

    spin_lock(&sk->lock);

    /* Writers publish a complete framed datagram under this same lock. */
    for (;;) {
        uint32_t available = sock_buf_available(&sk->recv_buf);
        if (available >= header_size) {
            if (sock_buf_peek_at(&sk->recv_buf, 0, &msg_len, sizeof(msg_len)) != sizeof(msg_len)
                || msg_len > sk->recv_buf.capacity - header_size) {
                spin_unlock(&sk->lock);
                return -EIO;
            }
            if (available >= header_size + msg_len) break;
        }
        if (is_nonblock) {
            spin_unlock(&sk->lock);
            return -EAGAIN;
        }
        if (sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD) || sk->state == SOCK_STATE_DISCONNECTING) {
            spin_unlock(&sk->lock);
            return 0;
        }
        sock_blocked_register(sk, current_task());
        spin_unlock(&sk->lock);
        wait_queue_sleep();
        spin_lock(&sk->lock);
        sock_blocked_unregister(sk);
    }

    if (sock_buf_peek_at(&sk->recv_buf, sizeof(uint32_t), &sender_addr, sizeof(sender_addr)) != sizeof(sender_addr)
        || sock_buf_peek_at(&sk->recv_buf, sizeof(uint32_t) + sizeof(sender_addr), &sender_credentials, sizeof(sender_credentials))
               != sizeof(sender_credentials)) {
        spin_unlock(&sk->lock);
        return -EIO;
    }

    /* Now read the payload */
    uint32_t payload = (uint32_t)len;
    if (payload > msg_len) payload = msg_len;

    uint32_t rd = payload ? sock_buf_peek_at(&sk->recv_buf, header_size, buf, payload) : 0;
    if (rd != payload) {
        spin_unlock(&sk->lock);
        return -EIO;
    }
    if (!peek) sock_buf_discard(&sk->recv_buf, header_size + msg_len);

    spin_unlock(&sk->lock);

    /* Return the sender to the syscall layer in a kernel buffer. */
    if (addr && addrlen) {
        uint32_t copy_len = *addrlen < sizeof(sender_addr) ? *addrlen : sizeof(sender_addr);
        memcpy(addr, &sender_addr, copy_len);
        *addrlen = sizeof(sender_addr);
    }

    if (credentials) *credentials = sender_credentials;
    if (message_flags && rd < msg_len) *message_flags |= MSG_TRUNC;
    if (record_size) *record_size = msg_len;

    if (!peek) {
        sock_blocked_wake_all(sk);
        if (sk->node) vfs_poll_notify(sk->node, 0x004);
    }

    return (int)rd;
}

/* Socket poll support */

static int socket_poll(socket_t *sk, size_t events)
{
    int revents = 0;

    if (!sk) return 0;

    spin_lock(&sk->lock);

    switch (sk->state) {
        case SOCK_STATE_LISTENING :
            /* POLLIN = connection waiting */
            if (sk->accept_queue_len > 0) revents |= 0x001; // POLLIN
            revents |= 0x004;                               // POLLOUT (always writable for listen)
            break;

        case SOCK_STATE_CONNECTED : {
            socket_t *p = sk->peer;

            /* POLLIN = data available or peer closed */
            if (sock_buf_available(&sk->recv_buf) > 0) revents |= 0x001;
            if (sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_RD)) revents |= 0x001;

            /* POLLOUT = send buffer not full */
            if (p && sock_buf_space(&p->recv_buf) > 0) revents |= 0x004;
            if (sk->shutdown_mask & SOCK_SHUT_MASK(SHUT_WR)) revents |= 0x004;

            /* POLLHUP = peer disconnected */
            if (!p || p->state == SOCK_STATE_DISCONNECTING) revents |= 0x010;
            break;
        }

        case SOCK_STATE_UNCONNECTED :
            /* DGRAM sockets can always send/recv if bound */
            if (sk->type == SOCK_DGRAM) {
                if (sock_buf_available(&sk->recv_buf) > 0) revents |= 0x001;
                revents |= 0x004; // dgram always writable
            } else {
                revents |= 0x010; // POLLHUP - not connected
            }
            break;

        case SOCK_STATE_DISCONNECTING :
            revents |= 0x010; // POLLHUP
            if (sock_buf_available(&sk->recv_buf) > 0) revents |= 0x001;
            break;

        default :
            break;
    }

    /* Check error */
    if (sk->so_error) revents |= 0x008; // POLLERR

    spin_unlock(&sk->lock);

    return revents & (int)events;
}

/* VFS callbacks */

static size_t socket_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    socket_t *sk = (socket_t *)file;
    int       ret;
    (void)offset;

    if (!sk) return (size_t)-1;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        ret                                = ops && ops->recvfrom ? ops->recvfrom(sk->priv, addr, size, 0, NULL, NULL) : -EOPNOTSUPP;
        return ret < 0 ? (size_t)-1 : (size_t)ret;
    }

    /* Use polymorphic op if set (netlink) */
    if (sk->socket_read) {
        ret = sk->socket_read(sk, addr, size, NULL, NULL);
        if (ret < 0) return (size_t)-1;
        return (size_t)ret;
    }

    if (sk->type == SOCK_DGRAM) {
        ret = unix_dgram_recv(sk, addr, size, NULL, NULL, sk->flags, NULL, NULL, NULL);
    } else if (sk->type == SOCK_SEQPACKET) {
        ret = unix_seqpacket_recv(sk, addr, size, 0, NULL, NULL);
    } else {
        ret = unix_stream_recv(sk, addr, size, 0);
    }

    if (ret < 0) return (size_t)-1;
    return (size_t)ret;
}

static size_t socket_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    socket_t *sk = (socket_t *)file;
    int       ret;
    (void)offset;

    if (!sk) return (size_t)-1;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        ret                                = ops && ops->sendto ? ops->sendto(sk->priv, addr, size, 0, NULL, 0) : -EOPNOTSUPP;
        return ret < 0 ? (size_t)-1 : (size_t)ret;
    }

    /* Use polymorphic op if set (netlink) */
    if (sk->socket_write) {
        ret = sk->socket_write(sk, addr, size, NULL, 0);
        if (ret < 0) return (size_t)-1;
        return (size_t)ret;
    }

    if (sk->type == SOCK_DGRAM) {
        ret = unix_dgram_send(sk, addr, size, NULL, 0, sk->flags);
    } else if (sk->type == SOCK_SEQPACKET) {
        ret = unix_seqpacket_send(sk, addr, size, 0);
    } else {
        ret = unix_stream_send(sk, addr, size, 0);
    }

    if (ret < 0) return (size_t)-1;
    return (size_t)ret;
}

static int64_t socket_vfs_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)offset;
    socket_t *sk = node ? node->handle : NULL;
    if (!sk) return -EBADF;
    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        return ops && ops->recvfrom ? ops->recvfrom(sk->priv, addr, size, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0, NULL, NULL) : -EOPNOTSUPP;
    }
    if (sk->socket_read) return sk->socket_read(sk, addr, size, NULL, NULL);
    if (sk->type == SOCK_DGRAM) return unix_dgram_recv(sk, addr, size, NULL, NULL, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0, NULL, NULL, NULL);
    if (sk->type == SOCK_SEQPACKET) return unix_seqpacket_recv(sk, addr, size, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0, NULL, NULL);
    return unix_stream_recv(sk, addr, size, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
}

static int64_t socket_vfs_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)offset;
    socket_t *sk = node ? node->handle : NULL;
    if (!sk) return -EBADF;
    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        return ops && ops->sendto ? ops->sendto(sk->priv, addr, size, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0, NULL, 0) : -EOPNOTSUPP;
    }
    if (sk->socket_write) return sk->socket_write(sk, addr, size, NULL, 0);
    if (sk->type == SOCK_DGRAM) return unix_dgram_send(sk, addr, size, NULL, 0, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    if (sk->type == SOCK_SEQPACKET) return unix_seqpacket_send(sk, addr, size, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    return unix_stream_send(sk, addr, size, (flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
}

static int socket_vfs_poll(void *file, size_t events)
{
    socket_t *sk = (socket_t *)file;
    if (!sk) return 0;
    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        return ops && ops->poll ? ops->poll(sk->priv, events) : 0;
    }
    /* Use polymorphic op if set (netlink) */
    if (sk->socket_poll) return sk->socket_poll(sk, events);
    return socket_poll(sk, events);
}

static void socket_vfs_close(void *current)
{
    socket_t *sk = (socket_t *)current;
    if (!sk) return;

    sk->shutdown_mask |= SOCK_SHUT_MASK(SHUT_RD) | SOCK_SHUT_MASK(SHUT_WR);
    sock_blocked_wake_all(sk);
    if (sk->node) vfs_poll_notify(sk->node, 0x01d);
}

static int socket_vfs_free(void *handle)
{
    socket_t *sk = (socket_t *)handle;
    if (!sk) return -EINVAL;

    if ((sk->family == AF_INET || sk->family == AF_INET6) && sk->priv) {
        const struct inet_backend_ops *ops = inet_backend_get();
        if (ops && ops->close) ops->close(sk->priv);
        sk->priv = NULL;
    }

    /* Netlink cleanup */
    if (sk->family == AF_NETLINK && sk->priv) netlink_close(sk);

    /* Remove from bound registry */
    sock_bound_remove(sk);

    if (sk->bound_node) {
        vfs_close(sk->bound_node);
        sk->bound_node = NULL;
    }

    /* Wake all blocked tasks */
    sock_blocked_wake_all(sk);

    /* Disconnect from peer */
    if (sk->peer) {
        socket_t *peer = sk->peer;
        sk->peer       = NULL;
        peer->peer     = NULL;
        peer->state    = SOCK_STATE_DISCONNECTING;
        sock_blocked_wake_all(peer);
        if (peer->node) vfs_poll_notify(peer->node, 0x01d);
        socket_unref(peer);
    }

    /* Free accept queue */
    if (sk->accept_queue) {
        for (uint32_t i = 0; i < sk->accept_queue_len; i++) {
            if (sk->accept_queue[i]) socket_unref(sk->accept_queue[i]);
        }
        free(sk->accept_queue);
        sk->accept_queue = NULL;
    }

    /* Free buffers */
    socket_drop_rights(sk);
    sock_buf_free(&sk->recv_buf);
    sock_buf_free(&sk->send_buf);

    /* Clear VFS node reference */
    sk->node = NULL;

    free(sk);
    return EOK;
}

/* VFS stubs */

static void socket_stub_unmount(void *root)
{
    (void)root;
}

static int socket_stub_stat(void *f, vfs_node_t n)
{
    (void)f;
    (void)n;
    return EOK;
}

static int socket_stub_mk(void *p, const char *nm, vfs_node_t n)
{
    (void)p;
    (void)nm;
    (void)n;
    return -ENOSYS;
}

static size_t socket_stub_readlink(vfs_node_t n, void *a, size_t o, size_t s)
{
    (void)n;
    (void)a;
    (void)o;
    (void)s;
    return (size_t)-1;
}

static int socket_stub_ioctl(void *f, size_t o, void *a)
{
    socket_t *sk = f;
    if (!sk || (sk->family != AF_INET && sk->family != AF_INET6)) return -ENOTTY;
    const struct inet_backend_ops *ops = inet_backend_get();
    if (!ops || !ops->ioctl) return -EOPNOTSUPP;
    if (!a) return -EFAULT;

    switch (o) {
        case SIOCGIFFLAGS :
        case SIOCSIFFLAGS :
        case SIOCGIFADDR :
        case SIOCGIFBRDADDR :
        case SIOCGIFNETMASK :
        case SIOCGIFMTU :
        case SIOCSIFMTU :
        case SIOCGIFHWADDR :
        case SIOCGIFINDEX :
            break;
        default :
            return -EOPNOTSUPP;
    }

    ifreq_t ifr;
    if (copy_from_user(&ifr, a, sizeof(ifr))) return -EFAULT;
    int ret = ops->ioctl(sk->priv, o, &ifr);
    if (ret < 0) return ret;
    if (copy_to_user(a, &ifr, sizeof(ifr))) return -EFAULT;
    return ret;
}

static vfs_node_t socket_stub_dup(vfs_node_t n)
{
    (void)n;
    return NULL;
}

static int socket_stub_del(void *p, vfs_node_t n)
{
    (void)p;
    (void)n;
    return -ENOSYS;
}

static int socket_stub_rename(void *c, const char *nm)
{
    (void)c;
    (void)nm;
    return -ENOSYS;
}

static int socket_stub_mount(const char *s, vfs_node_t n)
{
    (void)s;
    (void)n;
    return -ENOSYS;
}

static void socket_stub_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
}

/* System call implementations */

/* sys_socket */

int64_t sys_socket(uint32_t family, uint32_t type, uint32_t protocol)
{
    uint16_t  sock_type;
    uint16_t  sock_family;
    socket_t *sk;
    int       fd;
    uint32_t  extra_flags = 0;

    if (type & SOCK_NONBLOCK) {
        extra_flags |= SOCK_NONBLOCK;
        type &= ~SOCK_NONBLOCK;
    }
    uint64_t fd_flags = 0;
    if (type & SOCK_CLOEXEC) {
        fd_flags |= O_CLOEXEC;
        type &= ~SOCK_CLOEXEC;
    }

    sock_family = (uint16_t)family;
    sock_type   = (uint16_t)type;

    if (sock_family == AF_NETLINK) {
        /* Netlink uses SOCK_RAW or SOCK_DGRAM */
        if (sock_type != SOCK_RAW && sock_type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    } else if (sock_family == AF_INET || sock_family == AF_INET6) {
        const struct inet_backend_ops *ops     = inet_backend_get();
        void                          *context = NULL;
        int                            ret;
        if (sock_type != SOCK_DGRAM && sock_type != SOCK_STREAM && sock_type != SOCK_RAW) return -ESOCKTNOSUPPORT;
        if (sock_type == SOCK_RAW) {
            process_t *proc = process_current();
            if (!proc || proc->uid != 0) return -EPERM;
            if (sock_family != AF_INET || protocol != IPPROTO_ICMP) return -EPROTONOSUPPORT;
        }
        if (protocol == 0) protocol = sock_type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
        if ((sock_type == SOCK_STREAM && protocol != IPPROTO_TCP) || (sock_type == SOCK_DGRAM && protocol != IPPROTO_UDP)
            || (sock_type == SOCK_RAW && protocol != IPPROTO_ICMP))
            return -EPROTONOSUPPORT;
        if (!ops || !ops->create) return -EAFNOSUPPORT;
        ret = ops->create(sock_family, sock_type, (int)protocol, extra_flags, &context);
        if (ret < 0) return ret;
        sk = inet_socket_alloc(sock_family, sock_type, (uint16_t)protocol, extra_flags, context);
        if (!sk) {
            plogk("socket: Sys_socket inet allocation failed (family=%d type=%d)\n", sock_family, sock_type);
            ops->close(context);
            return -ENOMEM;
        }
        fd = socket_fd_install_flags(sk, fd_flags | (extra_flags ? O_NONBLOCK : 0));
        if (fd < 0) {
            socket_free(sk);
            return fd;
        }
        return fd;
    } else if (sock_family != AF_UNIX && sock_family != AF_LOCAL) {
        return -EAFNOSUPPORT;
    } else {
        if (sock_type != SOCK_STREAM && sock_type != SOCK_DGRAM && sock_type != SOCK_SEQPACKET) return -ESOCKTNOSUPPORT;
    }

    sk = socket_alloc(sock_family, sock_type, (uint16_t)protocol);
    if (!sk) {
        plogk("socket: Sys_socket unix allocation failed (family=%d type=%d)\n", sock_family, sock_type);
        return -ENOMEM;
    }

    sk->flags = extra_flags;

    fd = socket_fd_install_flags(sk, fd_flags | (extra_flags ? O_NONBLOCK : 0));
    if (fd < 0) {
        socket_free(sk);
        return fd;
    }

    return (int64_t)fd;
}

/* sys_bind */

int64_t sys_bind(int fd, const sockaddr_t *addr, uint32_t addrlen)
{
    socket_t     *sk;
    sockaddr_un_t kaddr;
    int           ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!addr) return -EINVAL;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        sockaddr_storage_t             kaddr;
        const struct inet_backend_ops *ops = inet_backend_get();
        if (addrlen < sizeof(sa_family_t) || addrlen > sizeof(kaddr)) return -EINVAL;
        memset(&kaddr, 0, sizeof(kaddr));
        if (copy_from_user(&kaddr, addr, addrlen)) return -EFAULT;
        if (kaddr.ss_family != sk->family) return -EAFNOSUPPORT;
        return ops && ops->bind ? ops->bind(sk->priv, (const sockaddr_t *)&kaddr, addrlen) : -EOPNOTSUPP;
    }

    /* Netlink bind */
    if (sk->family == AF_NETLINK) {
        sockaddr_nl_t nladdr;
        if (addrlen != sizeof(sockaddr_nl_t)) return -EINVAL;
        if (copy_from_user(&nladdr, (const void *)addr, addrlen)) return -EFAULT;
        return (int64_t)netlink_bind(sk, &nladdr, addrlen);
    }

    if (addrlen > sizeof(sockaddr_un_t)) return -EINVAL;

    memset(&kaddr, 0, sizeof(kaddr));
    if (copy_from_user(&kaddr, addr, addrlen)) return -EFAULT;

    spin_lock(&sk->lock);
    ret = unix_bind(sk, &kaddr, addrlen);
    spin_unlock(&sk->lock);

    return (int64_t)ret;
}

/* sys_listen */

int64_t sys_listen(int fd, int backlog)
{
    socket_t *sk;
    int       ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (backlog < 0) backlog = 0;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        return ops && ops->listen ? ops->listen(sk->priv, backlog) : -EOPNOTSUPP;
    }

    ret = unix_listen(sk, (uint32_t)backlog);

    return (int64_t)ret;
}

/* sys_accept */

int64_t sys_accept(int fd, sockaddr_t *addr, uint32_t *addrlen, int flags)
{
    socket_t *sk;
    int       ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    if ((addr == NULL) != (addrlen == NULL)) return -EFAULT;
    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        sockaddr_storage_t             kaddr;
        uint32_t                       kaddrlen = sizeof(kaddr);
        void                          *context  = NULL;
        socket_t                      *accepted;
        int                            fd_new;
        int                            ret;
        if (!ops || !ops->accept) return -EOPNOTSUPP;
        uint32_t operation_flags = (uint32_t)flags;
        if (socket_fd_nonblock(fd)) operation_flags |= SOCK_NONBLOCK;
        ret = ops->accept(sk->priv, &context, (sockaddr_t *)&kaddr, &kaddrlen, operation_flags);
        if (ret < 0) return ret;
        accepted = inet_socket_alloc(sk->family, sk->type, sk->protocol, flags & SOCK_NONBLOCK, context);
        if (!accepted) {
            plogk("socket: Sys_accept inet allocation failed (family=%d)\n", sk->family);
            ops->close(context);
            return -ENOMEM;
        }
        ret = socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&kaddr, kaddrlen);
        if (ret < 0) {
            socket_free(accepted);
            return ret;
        }
        fd_new = socket_fd_install_flags(accepted, ((flags & SOCK_CLOEXEC) ? O_CLOEXEC : 0) | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
        if (fd_new < 0) socket_free(accepted);
        return fd_new;
    }

    ret = unix_accept(sk, (sockaddr_un_t *)addr, addrlen, flags);

    return (int64_t)ret;
}

/* sys_connect */

int64_t sys_connect(int fd, const sockaddr_t *addr, uint32_t addrlen)
{
    socket_t     *sk;
    sockaddr_un_t kaddr;
    int           ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    /* Netlink is connectionless */
    if (sk->family == AF_NETLINK) return -EOPNOTSUPP;

    if (!addr) return -EINVAL;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        sockaddr_storage_t             kaddr;
        const struct inet_backend_ops *ops = inet_backend_get();
        if (addrlen < sizeof(sa_family_t) || addrlen > sizeof(kaddr)) return -EINVAL;
        memset(&kaddr, 0, sizeof(kaddr));
        if (copy_from_user(&kaddr, addr, addrlen)) return -EFAULT;
        if (kaddr.ss_family != sk->family && kaddr.ss_family != AF_UNSPEC) return -EAFNOSUPPORT;
        uint32_t operation_flags = socket_fd_nonblock(fd) ? SOCK_NONBLOCK : 0;
        return ops && ops->connect ? ops->connect(sk->priv, (const sockaddr_t *)&kaddr, addrlen, operation_flags) : -EOPNOTSUPP;
    }

    if (addrlen > sizeof(sockaddr_un_t)) return -EINVAL;

    memset(&kaddr, 0, sizeof(kaddr));
    if (copy_from_user(&kaddr, addr, addrlen)) return -EFAULT;

    spin_lock(&sk->lock);

    if (sk->state == SOCK_STATE_CONNECTED) {
        spin_unlock(&sk->lock);
        return -EISCONN;
    }
    if (sk->state == SOCK_STATE_LISTENING) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    spin_unlock(&sk->lock);

    ret = unix_stream_connect(sk, &kaddr, addrlen);

    return (int64_t)ret;
}

/* sys_sendto */

int64_t sys_sendto(int fd, const void *buf, size_t len, int flags, const sockaddr_t *addr, uint32_t addrlen)
{
    socket_t     *sk;
    sockaddr_un_t kaddr;
    void         *kbuf;
    int           ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    /*
     * O_NONBLOCK is a property of the open file description, not merely a
     * flag supplied to sendto(2).  AF_INET and netlink used to fold it into
     * the operation below, while AF_UNIX accidentally ignored it.  Wayland
     * clients set the display socket nonblocking with fcntl(2), so a flush
     * could otherwise sleep in the kernel instead of returning EAGAIN.
     */
    if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;

    if (!buf && len > 0) return -EFAULT;

    if (len > SOCK_BUF_MAX) return -EMSGSIZE;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        sockaddr_storage_t             kaddr;
        const sockaddr_t              *dest     = NULL;
        void                          *inet_buf = NULL;
        int                            inet_ret;
        if (!ops || !ops->sendto) return -EOPNOTSUPP;
        if (addr) {
            if (addrlen < sizeof(sa_family_t) || addrlen > sizeof(kaddr)) return -EINVAL;
            memset(&kaddr, 0, sizeof(kaddr));
            if (copy_from_user(&kaddr, addr, addrlen)) return -EFAULT;
            if (kaddr.ss_family != sk->family) return -EAFNOSUPPORT;
            dest = (const sockaddr_t *)&kaddr;
        } else if (addrlen) {
            return -EINVAL;
        }
        if (len) {
            inet_buf = malloc(len);
            if (!inet_buf) return -ENOMEM;
            if (copy_from_user(inet_buf, buf, len)) {
                free(inet_buf);
                return -EFAULT;
            }
        }
        inet_ret = ops->sendto(sk->priv, inet_buf, len, flags, dest, addrlen);
        free(inet_buf);
        return inet_ret;
    }

    /* Netlink datagrams retain their destination and operation flags. */
    if (sk->family == AF_NETLINK) {
        sockaddr_nl_t nladdr;
        const void   *nladdr_ptr = NULL;
        if (addr) {
            if (addrlen < sizeof(nladdr)) return -EINVAL;
            if (copy_from_user(&nladdr, addr, sizeof(nladdr))) return -EFAULT;
            nladdr_ptr = &nladdr;
        } else if (addrlen) {
            return -EINVAL;
        }
        void *kbuf_nl = len ? malloc(len) : NULL;
        if (len && !kbuf_nl) return -ENOMEM;
        if (len && copy_from_user(kbuf_nl, buf, len)) {
            free(kbuf_nl);
            return -EFAULT;
        }
        int ret_nl = netlink_sendmsg(sk, kbuf_nl, len, nladdr_ptr, addr ? sizeof(nladdr) : 0, flags);
        free(kbuf_nl);
        return (int64_t)ret_nl;
    }

    kbuf = len ? malloc(len) : NULL;
    if (len && !kbuf) return -ENOMEM;

    if (len && copy_from_user(kbuf, buf, len)) {
        free(kbuf);
        return -EFAULT;
    }

    if (sk->type == SOCK_DGRAM) {
        if (addr && addrlen > 0) {
            if (addrlen > sizeof(sockaddr_un_t)) {
                free(kbuf);
                return -EINVAL;
            }
            if (copy_from_user(&kaddr, addr, addrlen)) {
                free(kbuf);
                return -EFAULT;
            }
            ret = unix_dgram_send(sk, kbuf, len, &kaddr, addrlen, flags);
        } else {
            ret = unix_dgram_send(sk, kbuf, len, NULL, 0, flags);
        }
    } else if (sk->type == SOCK_SEQPACKET) {
        ret = unix_seqpacket_send(sk, kbuf, len, flags);
    } else {
        ret = unix_stream_send(sk, kbuf, len, flags);
    }

    free(kbuf);
    return (int64_t)ret;
}

/* sys_recvfrom */

int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags, sockaddr_t *addr, uint32_t *addrlen)
{
    socket_t *sk;
    void     *kbuf;
    int       ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    /*
     * recv(2) is implemented through recvfrom(2) by libc.  Honor the file's
     * O_NONBLOCK state for AF_UNIX as well as the other socket families.
     */
    if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;

    if (!buf && len) return -EFAULT;
    if (!len) return 0;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        sockaddr_storage_t             kaddr;
        uint32_t                       kaddrlen = sizeof(kaddr);
        void                          *inet_buf = NULL;
        int                            inet_ret;
        if (!ops || !ops->recvfrom) return -EOPNOTSUPP;
        if ((addr == NULL) != (addrlen == NULL)) return -EFAULT;
        if (len) {
            inet_buf = malloc(len);
            if (!inet_buf) return -ENOMEM;
        }
        inet_ret = ops->recvfrom(sk->priv, inet_buf, len, flags, addr ? (sockaddr_t *)&kaddr : NULL, addr ? &kaddrlen : NULL);
        if (inet_ret > 0 && copy_to_user(buf, inet_buf, (size_t)inet_ret)) inet_ret = -EFAULT;
        if (inet_ret >= 0 && addr) {
            int copy_ret = socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&kaddr, kaddrlen);
            if (copy_ret < 0) inet_ret = copy_ret;
        }
        free(inet_buf);
        return inet_ret;
    }

    if (len > SOCK_BUF_MAX) len = SOCK_BUF_MAX;

    if (sk->family == AF_NETLINK) {
        sockaddr_nl_t sender;
        uint32_t      sender_uid;
        uint32_t      sender_gid;
        int           message_flags;

        if ((addr == NULL) != (addrlen == NULL)) return -EFAULT;
        kbuf = malloc(len);
        if (!kbuf) return -ENOMEM;
        ret = netlink_recvmsg_kern(sk, kbuf, len, addr ? &sender : NULL, flags, &sender_uid, &sender_gid, &message_flags);
        if (ret > 0) {
            size_t copied = (size_t)ret < len ? (size_t)ret : len;
            if (copy_to_user(buf, kbuf, copied)) {
                free(kbuf);
                return -EFAULT;
            }
        }
        if (ret >= 0 && addr) {
            int copy_ret = socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&sender, sizeof(sender));
            if (copy_ret < 0) ret = copy_ret;
        }
        free(kbuf);
        return (int64_t)ret;
    }

    kbuf = len ? malloc(len) : NULL;
    if (len && !kbuf) return -ENOMEM;

    if (sk->type == SOCK_DGRAM) {
        sockaddr_un_t sender;
        uint32_t      sender_len = sizeof(sender);
        size_t        record_len = 0;
        ret = unix_dgram_recv(sk, kbuf, len, addr ? &sender : NULL, addr ? &sender_len : NULL, flags, NULL, NULL, &record_len);
        if (ret >= 0 && addr) {
            int copy_ret = socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&sender, sender_len);
            if (copy_ret < 0) ret = copy_ret;
        }
        if (ret >= 0 && (flags & MSG_TRUNC)) {
            int copied = ret;
            if (copied > 0 && copy_to_user(buf, kbuf, (size_t)copied)) {
                free(kbuf);
                return -EFAULT;
            }
            free(kbuf);
            return (int64_t)record_len;
        }
    } else if (sk->type == SOCK_SEQPACKET) {
        size_t record_len = 0;
        ret               = unix_seqpacket_recv(sk, kbuf, len, flags, NULL, &record_len);
        if (ret >= 0 && (flags & MSG_TRUNC)) {
            int copied = ret;
            if (copied > 0 && copy_to_user(buf, kbuf, (size_t)copied)) {
                free(kbuf);
                return -EFAULT;
            }
            free(kbuf);
            return (int64_t)record_len;
        }
    } else {
        ret = unix_stream_recv(sk, kbuf, len, flags);
    }

    if (ret > 0) {
        if (copy_to_user(buf, kbuf, (size_t)ret)) {
            free(kbuf);
            return -EFAULT;
        }
    }

    free(kbuf);
    return (int64_t)ret;
}

/* Internal: sendmsg/recvmsg with kernel buffers */

static void socket_release_rights(process_file_t **rights, size_t rights_count)
{
    for (size_t i = 0; i < rights_count; i++) process_file_put_transfer(rights[i]);
}

static int socket_collect_rights(socket_t *sk, const msghdr_t *kmsg, process_file_t **rights, size_t *rights_count)
{
    enum { CONTROL_MAX = 4096 };
    uint8_t   *control;
    size_t     offset = 0;
    process_t *proc;

    *rights_count = 0;
    if (!sk || sk->family != AF_UNIX || kmsg->msg_controllen == 0) return EOK;
    if (!kmsg->msg_control) return -EFAULT;
    if (kmsg->msg_controllen > CONTROL_MAX) return -EMSGSIZE;

    control = malloc(kmsg->msg_controllen);
    if (!control) return -ENOMEM;
    if (copy_from_user(control, kmsg->msg_control, kmsg->msg_controllen)) {
        free(control);
        return -EFAULT;
    }

    proc = process_current();
    if (!proc) {
        free(control);
        return -ESRCH;
    }

    while (offset < kmsg->msg_controllen) {
        size_t remaining = kmsg->msg_controllen - offset;
        if (remaining < sizeof(cmsghdr_t)) goto malformed;

        cmsghdr_t *cmsg = (cmsghdr_t *)(control + offset);
        if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > remaining) goto malformed;

        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
            if (data_len == 0 || data_len % sizeof(int) != 0) goto malformed;
            size_t count = data_len / sizeof(int);
            if (count > SOCK_RIGHTS_MAX - *rights_count) {
                socket_release_rights(rights, *rights_count);
                *rights_count = 0;
                free(control);
                return -EMSGSIZE;
            }
            uint8_t *cmsg_data = CMSG_DATA(cmsg);
            for (size_t i = 0; i < count; i++) {
                int fd;
                memcpy(&fd, cmsg_data + i * sizeof(fd), sizeof(fd));
                process_file_t *file = process_fd_get_for_transfer(proc, fd);
                if (!file) {
                    socket_release_rights(rights, *rights_count);
                    *rights_count = 0;
                    free(control);
                    return -EBADF;
                }
                rights[(*rights_count)++] = file;
            }
        }

        /*
         * Linux accepts msg_controllen == cmsg_len for the final header; the
         * trailing alignment bytes need not be present in the user buffer.
         */
        if (cmsg->cmsg_len == remaining) break;
        size_t step = CMSG_ALIGN(cmsg->cmsg_len);
        if (step > remaining) goto malformed;
        offset += step;
    }

    free(control);
    return EOK;

malformed:
    socket_release_rights(rights, *rights_count);
    *rights_count = 0;
    free(control);
    return -EINVAL;
}

static int64_t do_sendmsg_kern(int fd, socket_t *sk, const msghdr_t *kmsg, const iovec_t *iov, const void *kbuf, size_t total_len, int flags,
                               process_file_t **rights, size_t rights_count)
{
    int ret;
    (void)iov;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        sockaddr_storage_t             kaddr;
        const sockaddr_t              *dest = NULL;
        if (!ops || !ops->sendto) return -EOPNOTSUPP;
        if (kmsg->msg_name) {
            if (kmsg->msg_namelen < sizeof(sa_family_t) || kmsg->msg_namelen > sizeof(kaddr)) return -EINVAL;
            memset(&kaddr, 0, sizeof(kaddr));
            if (copy_from_user(&kaddr, kmsg->msg_name, kmsg->msg_namelen)) return -EFAULT;
            if (kaddr.ss_family != sk->family) return -EAFNOSUPPORT;
            dest = (const sockaddr_t *)&kaddr;
        } else if (kmsg->msg_namelen) {
            return -EINVAL;
        }
        if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;
        return ops->sendto(sk->priv, kbuf, total_len, flags, dest, kmsg->msg_namelen);
    }

    /* Netlink: copy the optional destination before entering the backend. */
    if (sk->family == AF_NETLINK) {
        sockaddr_nl_t nladdr;
        const void   *dest = NULL;
        if (kmsg->msg_name) {
            if (kmsg->msg_namelen < sizeof(nladdr)) return -EINVAL;
            if (copy_from_user(&nladdr, kmsg->msg_name, sizeof(nladdr))) return -EFAULT;
            dest = &nladdr;
        } else if (kmsg->msg_namelen) {
            return -EINVAL;
        }
        if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;
        return (int64_t)netlink_sendmsg(sk, kbuf, total_len, dest, dest ? sizeof(nladdr) : 0, flags);
    }

    /* sendmsg(2) inherits O_NONBLOCK from the open file description. */
    if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;

    if (sk->type == SOCK_DGRAM) {
        if (rights_count) return -EOPNOTSUPP;
        sockaddr_un_t kaddr;
        if (kmsg->msg_name && kmsg->msg_namelen > 0) {
            if (kmsg->msg_namelen > sizeof(sockaddr_un_t)) return -EINVAL;
            if (copy_from_user(&kaddr, kmsg->msg_name, kmsg->msg_namelen)) return -EFAULT;
            ret = unix_dgram_send(sk, kbuf, total_len, &kaddr, kmsg->msg_namelen, flags);
        } else {
            ret = unix_dgram_send(sk, kbuf, total_len, NULL, 0, flags);
        }
    } else if (sk->type == SOCK_SEQPACKET) {
        if (rights_count) return -EOPNOTSUPP;
        ret = unix_seqpacket_send(sk, kbuf, total_len, flags);
    } else {
        ret = unix_stream_send_rights(sk, kbuf, total_len, flags, rights, rights_count);
    }

    return (int64_t)ret;
}

static int64_t do_recvmsg_kern(int fd, socket_t *sk, msghdr_t *kmsg, const iovec_t *iov, void *kbuf, size_t total_len, int flags)
{
    int    ret;
    int    msg_flags = 0;
    int    installed_rights[SOCK_RIGHTS_MAX];
    size_t installed_rights_count = 0;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        sockaddr_storage_t             kaddr;
        uint32_t                       kaddrlen = sizeof(kaddr);
        if (!ops || !ops->recvfrom) return -EOPNOTSUPP;
        if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;
        ret = ops->recvfrom(sk->priv, kbuf, total_len, flags, kmsg->msg_name ? (sockaddr_t *)&kaddr : NULL, kmsg->msg_name ? &kaddrlen : NULL);
        if (ret > 0) {
            uint8_t *src       = kbuf;
            size_t   remaining = (size_t)ret;
            for (size_t i = 0; i < kmsg->msg_iovlen && remaining; i++) {
                size_t chunk = iov[i].iov_len < remaining ? iov[i].iov_len : remaining;
                if (copy_to_user(iov[i].iov_base, src, chunk)) return -EFAULT;
                src += chunk;
                remaining -= chunk;
            }
        }
        if (ret >= 0 && kmsg->msg_name) {
            uint32_t copylen = kmsg->msg_namelen < kaddrlen ? kmsg->msg_namelen : kaddrlen;
            if (copylen && copy_to_user(kmsg->msg_name, &kaddr, copylen)) return -EFAULT;
            kmsg->msg_namelen = kaddrlen;
        }
        kmsg->msg_flags      = 0;
        kmsg->msg_controllen = 0;
        return ret;
    }

    if (sk->family == AF_NETLINK) {
        sockaddr_nl_t sender;
        uint32_t      sender_uid = 0;
        uint32_t      sender_gid = 0;

        if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;
        ret = netlink_recvmsg_kern(sk, kbuf, total_len, &sender, flags, &sender_uid, &sender_gid, &msg_flags);
        if (ret > 0) {
            uint8_t *src       = (uint8_t *)kbuf;
            size_t   remaining = (size_t)ret < total_len ? (size_t)ret : total_len;
            for (size_t i = 0; i < kmsg->msg_iovlen && remaining > 0; i++) {
                size_t chunk = iov[i].iov_len;
                if (chunk > remaining) chunk = remaining;
                if (copy_to_user(iov[i].iov_base, src, chunk)) return -EFAULT;
                src += chunk;
                remaining -= chunk;
            }
        }
        if (ret >= 0 && kmsg->msg_name) {
            uint32_t copylen = kmsg->msg_namelen < sizeof(sender) ? kmsg->msg_namelen : sizeof(sender);
            if (copylen && copy_to_user(kmsg->msg_name, &sender, copylen)) return -EFAULT;
            kmsg->msg_namelen = sizeof(sender);
        } else if (ret >= 0) {
            kmsg->msg_namelen = 0;
        }

        size_t control_capacity = kmsg->msg_controllen;
        kmsg->msg_controllen    = 0;
        if (ret >= 0 && (sk->passcred || netlink_packet_info_enabled(sk))) {
            uint8_t control[CMSG_SPACE(sizeof(ucred_t)) + CMSG_SPACE(sizeof(nl_pktinfo_t))];
            size_t  used = 0;
            memset(control, 0, sizeof(control));

            if (sk->passcred) {
                cmsghdr_t *cmsg     = (cmsghdr_t *)(control + used);
                cmsg->cmsg_len      = CMSG_LEN(sizeof(ucred_t));
                cmsg->cmsg_level    = SOL_SOCKET;
                cmsg->cmsg_type     = SCM_CREDENTIALS;
                ucred_t credentials = {.pid = sender.nl_pid, .uid = sender_uid, .gid = sender_gid};
                memcpy(CMSG_DATA(cmsg), &credentials, sizeof(credentials));
                used += CMSG_SPACE(sizeof(credentials));
            }
            if (netlink_packet_info_enabled(sk)) {
                cmsghdr_t *cmsg          = (cmsghdr_t *)(control + used);
                cmsg->cmsg_len           = CMSG_LEN(sizeof(nl_pktinfo_t));
                cmsg->cmsg_level         = SOL_NETLINK;
                cmsg->cmsg_type          = NETLINK_PKTINFO;
                nl_pktinfo_t packet_info = {.group = sender.nl_groups ? (uint32_t)__builtin_ctz(sender.nl_groups) + 1U : 0U};
                memcpy(CMSG_DATA(cmsg), &packet_info, sizeof(packet_info));
                used += CMSG_SPACE(sizeof(packet_info));
            }
            if (kmsg->msg_control && control_capacity >= used) {
                if (copy_to_user(kmsg->msg_control, control, used)) return -EFAULT;
                kmsg->msg_controllen = used;
            } else if (used) {
                msg_flags |= MSG_CTRUNC;
            }
        }
        kmsg->msg_flags = msg_flags;
        return (int64_t)ret;
    }

    /*
     * Xtrans uses recvmsg(2) on accepted local X11 sockets.  Without this,
     * an O_NONBLOCK X client can sleep forever inside the kernel.
     */
    if (socket_fd_nonblock(fd)) flags |= MSG_DONTWAIT;

    if (sk->type == SOCK_DGRAM) {
        sockaddr_un_t sender;
        uint32_t      sender_len = sizeof(sender);
        ucred_t       sender_credentials;
        size_t        record_len = 0;
        ret = unix_dgram_recv(sk, kbuf, total_len, kmsg->msg_name ? &sender : NULL, kmsg->msg_name ? &sender_len : NULL, flags,
                              &sender_credentials, &msg_flags, &record_len);
        if (ret >= 0 && kmsg->msg_name) {
            uint32_t copylen = kmsg->msg_namelen < sender_len ? kmsg->msg_namelen : sender_len;
            if (copylen && copy_to_user(kmsg->msg_name, &sender, copylen)) return -EFAULT;
            kmsg->msg_namelen = sender_len;
        }
        size_t control_capacity = kmsg->msg_controllen;
        kmsg->msg_controllen    = 0;
        if (ret >= 0 && sk->passcred) {
            uint8_t control[CMSG_SPACE(sizeof(ucred_t))];
            memset(control, 0, sizeof(control));
            cmsghdr_t *cmsg  = (cmsghdr_t *)control;
            cmsg->cmsg_len   = CMSG_LEN(sizeof(ucred_t));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_CREDENTIALS;
            memcpy(CMSG_DATA(cmsg), &sender_credentials, sizeof(sender_credentials));
            if (kmsg->msg_control && control_capacity >= sizeof(control)) {
                if (copy_to_user(kmsg->msg_control, control, sizeof(control))) return -EFAULT;
                kmsg->msg_controllen = sizeof(control);
            } else {
                msg_flags |= MSG_CTRUNC;
            }
        }
        if (ret >= 0 && (flags & MSG_TRUNC)) {
            int copied = ret;
            if (copied > 0) {
                uint8_t *src       = (uint8_t *)kbuf;
                size_t   remaining = (size_t)copied;
                for (size_t i = 0; i < kmsg->msg_iovlen && remaining; i++) {
                    size_t chunk = iov[i].iov_len < remaining ? iov[i].iov_len : remaining;
                    if (copy_to_user(iov[i].iov_base, src, chunk)) return -EFAULT;
                    src += chunk;
                    remaining -= chunk;
                }
            }
            kmsg->msg_flags = msg_flags;
            return (int64_t)record_len;
        }
    } else if (sk->type == SOCK_SEQPACKET) {
        size_t record_len = 0;
        ret               = unix_seqpacket_recv(sk, kbuf, total_len, flags, &msg_flags, &record_len);
        if (ret >= 0 && (flags & MSG_TRUNC)) {
            /*
             * Scatter only the bytes that fit; recvmsg's return value may
             * still report the complete record length with MSG_TRUNC.
             */
            int copied = ret;
            if (copied > 0) {
                uint8_t *src       = (uint8_t *)kbuf;
                size_t   remaining = (size_t)copied;
                for (size_t i = 0; i < kmsg->msg_iovlen && remaining; i++) {
                    size_t chunk = iov[i].iov_len < remaining ? iov[i].iov_len : remaining;
                    if (copy_to_user(iov[i].iov_base, src, chunk)) return -EFAULT;
                    src += chunk;
                    remaining -= chunk;
                }
            }
            kmsg->msg_flags      = msg_flags;
            kmsg->msg_controllen = 0;
            return (int64_t)record_len;
        }
    } else {
        ret = unix_stream_recv(sk, kbuf, total_len, flags);
    }

    /*
     * Build connected AF_UNIX ancillary data.  SCM_RIGHTS entries are actual
     * references to the sender's open-file descriptions, not fresh opens of
     * the vnode; seatd relies on that to hand its DRM fd to Weston.
     */
    if (sk->type != SOCK_DGRAM) {
        size_t  control_capacity = kmsg->msg_controllen;
        uint8_t control[CMSG_SPACE(SOCK_RIGHTS_MAX * sizeof(int)) + CMSG_SPACE(sizeof(ucred_t))];
        size_t  control_used = 0;
        kmsg->msg_controllen = 0;
        memset(control, 0, sizeof(control));

        process_file_t *received_rights[SOCK_RIGHTS_MAX];
        size_t          received_rights_count = 0;
        if (ret > 0 && sk->type == SOCK_STREAM && !(flags & MSG_PEEK))
            received_rights_count = socket_take_rights(sk, received_rights, SOCK_RIGHTS_MAX);

        if (received_rights_count > 0) {
            size_t fit = received_rights_count;
            if (!kmsg->msg_control) {
                fit = 0;
            } else {
                while (fit > 0 && CMSG_SPACE(fit * sizeof(int)) > control_capacity) fit--;
            }

            int        delivered_fds[SOCK_RIGHTS_MAX];
            size_t     delivered_count = 0;
            process_t *proc            = process_current();
            for (size_t i = 0; i < received_rights_count; i++) {
                if (i < fit && proc) {
                    int newfd = process_fd_install_file(proc, received_rights[i], (flags & MSG_CMSG_CLOEXEC) ? O_CLOEXEC : 0);
                    if (newfd >= 0) {
                        delivered_fds[delivered_count++]           = newfd;
                        installed_rights[installed_rights_count++] = newfd;
                    } else {
                        msg_flags |= MSG_CTRUNC;
                    }
                } else {
                    msg_flags |= MSG_CTRUNC;
                }
                /*
                 * Drop the in-flight ref.  A successfully installed fd owns
                 * its own descriptor reference now.
                 */
                process_file_put_transfer(received_rights[i]);
            }

            if (delivered_count > 0) {
                cmsghdr_t *cmsg  = (cmsghdr_t *)(control + control_used);
                cmsg->cmsg_len   = CMSG_LEN(delivered_count * sizeof(int));
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type  = SCM_RIGHTS;
                memcpy(CMSG_DATA(cmsg), delivered_fds, delivered_count * sizeof(int));
                control_used += CMSG_SPACE(delivered_count * sizeof(int));
            }
        }

        int     passcred;
        int     has_peer;
        ucred_t credentials;
        spin_lock(&sk->lock);
        socket_t *peer  = sk->peer;
        passcred        = sk->passcred;
        has_peer        = peer != NULL;
        credentials.pid = peer ? peer->pid : 0;
        credentials.uid = peer ? peer->uid : 0;
        credentials.gid = peer ? peer->gid : 0;
        spin_unlock(&sk->lock);

        /*
         * SO_PASSCRED applies to connected sockets too.  eudevd's
         * SOCK_SEQPACKET control channel requires this record.
         */
        if (ret >= 0 && passcred && has_peer && kmsg->msg_control && control_capacity >= control_used
            && control_capacity - control_used >= CMSG_SPACE(sizeof(credentials))) {
            cmsghdr_t *cmsg  = (cmsghdr_t *)(control + control_used);
            cmsg->cmsg_len   = CMSG_LEN(sizeof(credentials));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_CREDENTIALS;
            memcpy(CMSG_DATA(cmsg), &credentials, sizeof(credentials));
            control_used += CMSG_SPACE(sizeof(credentials));
        } else if (ret >= 0 && passcred && has_peer) {
            msg_flags |= MSG_CTRUNC;
        }

        if (control_used > 0) {
            if (copy_to_user(kmsg->msg_control, control, control_used)) {
                process_t *proc = process_current();
                for (size_t i = 0; proc && i < installed_rights_count; i++) process_fd_close(proc, installed_rights[i]);
                return -EFAULT;
            }
            kmsg->msg_controllen = control_used;
        }
    }

    if (ret > 0) {
        /* Scatter data to iovecs */
        uint8_t *src       = (uint8_t *)kbuf;
        size_t   remaining = (size_t)ret;
        for (size_t i = 0; i < kmsg->msg_iovlen && remaining > 0; i++) {
            size_t chunk = iov[i].iov_len;
            if (chunk > remaining) chunk = remaining;
            if (copy_to_user(iov[i].iov_base, src, chunk)) {
                process_t *proc = process_current();
                for (size_t j = 0; proc && j < installed_rights_count; j++) process_fd_close(proc, installed_rights[j]);
                return -EFAULT;
            }
            src += chunk;
            remaining -= chunk;
        }
    }

    /* Write back msg_flags */
    kmsg->msg_flags = msg_flags;

    return (int64_t)ret;
}

/* sys_sendmsg */

int64_t sys_sendmsg(int fd, const msghdr_t *msg, int flags)
{
    socket_t       *sk;
    msghdr_t        kmsg;
    iovec_t        *iov;
    void           *kbuf;
    size_t          total_len;
    int64_t         ret;
    process_file_t *rights[SOCK_RIGHTS_MAX];
    size_t          rights_count = 0;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!msg) return -EINVAL;

    if (copy_from_user(&kmsg, msg, sizeof(msghdr_t))) return -EFAULT;

    if (kmsg.msg_iovlen == 0 || !kmsg.msg_iov) return -EINVAL;

    if (kmsg.msg_iovlen > 1024) return -EINVAL;

    iov = malloc(kmsg.msg_iovlen * sizeof(iovec_t));
    if (!iov) return -ENOMEM;

    if (copy_from_user(iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(iovec_t))) {
        free(iov);
        return -EFAULT;
    }

    total_len = 0;
    for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (iov[i].iov_len > SOCK_BUF_MAX - total_len) {
            free(iov);
            return -EMSGSIZE;
        }
        total_len += iov[i].iov_len;
    }

    if (total_len > SOCK_BUF_MAX) {
        free(iov);
        return -EMSGSIZE;
    }

    if (total_len == 0 && sk->type != SOCK_DGRAM && sk->type != SOCK_SEQPACKET) {
        free(iov);
        return 0;
    }

    kbuf = total_len ? malloc(total_len) : NULL;
    if (total_len && !kbuf) {
        free(iov);
        return -ENOMEM;
    }

    {
        uint8_t *dst = (uint8_t *)kbuf;
        for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
            if (iov[i].iov_len > 0) {
                if (copy_from_user(dst, iov[i].iov_base, iov[i].iov_len)) {
                    free(kbuf);
                    free(iov);
                    return -EFAULT;
                }
                dst += iov[i].iov_len;
            }
        }
    }

    int rights_ret = socket_collect_rights(sk, &kmsg, rights, &rights_count);
    if (rights_ret < 0) {
        free(kbuf);
        free(iov);
        return rights_ret;
    }

    ret = do_sendmsg_kern(fd, sk, &kmsg, iov, kbuf, total_len, flags, rights, rights_count);
    if (ret <= 0 && rights_count) socket_release_rights(rights, rights_count);

    free(kbuf);
    free(iov);
    return ret;
}

/* sys_recvmsg */

int64_t sys_recvmsg(int fd, msghdr_t *msg, int flags)
{
    socket_t *sk;
    msghdr_t  kmsg;
    iovec_t  *iov;
    void     *kbuf;
    size_t    total_len;
    int64_t   ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!msg) return -EINVAL;

    if (copy_from_user(&kmsg, msg, sizeof(msghdr_t))) return -EFAULT;

    if (kmsg.msg_iovlen == 0 || !kmsg.msg_iov) return -EINVAL;

    if (kmsg.msg_iovlen > 1024) return -EINVAL;

    iov = malloc(kmsg.msg_iovlen * sizeof(iovec_t));
    if (!iov) return -ENOMEM;

    if (copy_from_user(iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(iovec_t))) {
        free(iov);
        return -EFAULT;
    }

    total_len = 0;
    for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (iov[i].iov_len > SOCK_BUF_MAX - total_len) {
            total_len = SOCK_BUF_MAX;
            break;
        }
        total_len += iov[i].iov_len;
    }

    if (total_len == 0 && sk->type != SOCK_DGRAM && sk->type != SOCK_SEQPACKET) {
        free(iov);
        return 0;
    }

    if (total_len > SOCK_BUF_MAX) total_len = SOCK_BUF_MAX;

    kbuf = total_len ? malloc(total_len) : NULL;
    if (total_len && !kbuf) {
        free(iov);
        return -ENOMEM;
    }

    ret = do_recvmsg_kern(fd, sk, &kmsg, iov, kbuf, total_len, flags);

    /* Write back msghdr to user */
    if (copy_to_user(msg, &kmsg, sizeof(msghdr_t))) {
        free(kbuf);
        free(iov);
        return -EFAULT;
    }

    free(kbuf);
    free(iov);
    return ret;
}

/* sys_shutdown */

int64_t sys_shutdown(int fd, int how)
{
    socket_t *sk;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    /* Netlink is connectionless */
    if (sk->family == AF_NETLINK) return -EOPNOTSUPP;

    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -EINVAL;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        return ops && ops->shutdown ? ops->shutdown(sk->priv, how) : -EOPNOTSUPP;
    }

    spin_lock(&sk->lock);

    if (sk->state != SOCK_STATE_CONNECTED && sk->state != SOCK_STATE_LISTENING) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    sk->shutdown_mask |= SOCK_SHUT_MASK(how);

    /* Wake blocked tasks */
    sock_blocked_wake_all(sk);

    if (sk->peer) sock_blocked_wake_all(sk->peer);

    spin_unlock(&sk->lock);

    if (sk->node) vfs_poll_notify(sk->node, 0x01d);
    if (sk->peer && sk->peer->node) vfs_poll_notify(sk->peer->node, 0x01d);

    return EOK;
}

/* sys_socketpair */

int64_t sys_socketpair(int domain, int type, int protocol, int sv[2])
{
    socket_t *sk1, *sk2;
    int       fd1, fd2;
    int       sv_kern[2];
    uint32_t  socket_flags = (uint32_t)type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int       socket_type  = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (domain != AF_UNIX && domain != AF_LOCAL) return -EAFNOSUPPORT;
    if (type & ~(SOCK_STREAM | SOCK_DGRAM | SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    if (socket_type != SOCK_STREAM && socket_type != SOCK_DGRAM && socket_type != SOCK_SEQPACKET) return -ESOCKTNOSUPPORT;
    if (protocol != 0) return -EPROTONOSUPPORT;
    if (!sv) return -EFAULT;

    sk1 = socket_alloc((uint16_t)domain, (uint16_t)socket_type, (uint16_t)protocol);
    if (!sk1) return -ENOMEM;

    sk2 = socket_alloc((uint16_t)domain, (uint16_t)socket_type, (uint16_t)protocol);
    if (!sk2) {
        socket_free(sk1);
        return -ENOMEM;
    }

    sk1->shutdown_mask = 0;
    sk1->so_error      = 0;
    sk1->flags         = socket_flags & SOCK_NONBLOCK;
    sk2->shutdown_mask = 0;
    sk2->so_error      = 0;
    sk2->flags         = socket_flags & SOCK_NONBLOCK;

    sk1->peer = sk2;
    socket_ref(sk2);
    sk2->peer = sk1;
    socket_ref(sk1);

    sk1->state = SOCK_STATE_CONNECTED;
    sk2->state = SOCK_STATE_CONNECTED;

    memset(&sk1->peer_addr, 0, sizeof(sockaddr_un_t));
    sk1->peer_addr_len = 0;
    memset(&sk2->peer_addr, 0, sizeof(sockaddr_un_t));
    sk2->peer_addr_len = 0;

    uint64_t fd_flags = ((socket_flags & SOCK_CLOEXEC) ? O_CLOEXEC : 0) | ((socket_flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
    fd1               = socket_fd_install_flags(sk1, fd_flags);
    if (fd1 < 0) {
        socket_free(sk2);
        return (int64_t)fd1;
    }

    fd2 = socket_fd_install_flags(sk2, fd_flags);
    if (fd2 < 0) {
        process_t *proc = process_current();
        if (proc) process_fd_close(proc, fd1);
        return (int64_t)fd2;
    }

    socket_unref(sk1);
    socket_unref(sk2);

    sv_kern[0] = fd1;
    sv_kern[1] = fd2;

    if (copy_to_user(sv, sv_kern, sizeof(sv_kern))) {
        process_t *proc = process_current();
        if (proc) {
            process_fd_close(proc, fd1);
            process_fd_close(proc, fd2);
        }
        return -EFAULT;
    }

    return EOK;
}

/* sys_getsockname */

int64_t sys_getsockname(int fd, sockaddr_t *addr, uint32_t *addrlen)
{
    socket_t *sk;
    uint32_t  kaddrlen;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!addr || !addrlen) return -EINVAL;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        sockaddr_storage_t             kaddr;
        uint32_t                       len = sizeof(kaddr);
        int                            ret;
        if (!ops || !ops->getsockname) return -EOPNOTSUPP;
        ret = ops->getsockname(sk->priv, (sockaddr_t *)&kaddr, &len);
        if (ret < 0) return ret;
        return socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&kaddr, len);
    }

    if (sk->family == AF_NETLINK) {
        sockaddr_nl_t local;
        int           ret = netlink_getsockname(sk, &local);
        if (ret) return ret;
        return socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&local, sizeof(local));
    }

    spin_lock(&sk->lock);

    kaddrlen = sk->local_addr_len;
    if (kaddrlen == 0) {
        spin_unlock(&sk->lock);
        return -EADDRNOTAVAIL;
    }

    spin_unlock(&sk->lock);

    return socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&sk->local_addr, kaddrlen);
}

/* sys_getpeername */

int64_t sys_getpeername(int fd, sockaddr_t *addr, uint32_t *addrlen)
{
    socket_t *sk;
    uint32_t  kaddrlen;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!addr || !addrlen) return -EINVAL;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        sockaddr_storage_t             kaddr;
        uint32_t                       len = sizeof(kaddr);
        int                            ret;
        if (!ops || !ops->getpeername) return -EOPNOTSUPP;
        ret = ops->getpeername(sk->priv, (sockaddr_t *)&kaddr, &len);
        if (ret < 0) return ret;
        return socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&kaddr, len);
    }

    spin_lock(&sk->lock);

    if (sk->state != SOCK_STATE_CONNECTED) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    kaddrlen = sk->peer_addr_len;
    spin_unlock(&sk->lock);

    return socket_copy_address_to_user(addr, addrlen, (sockaddr_t *)&sk->peer_addr, kaddrlen);
}

/* sys_setsockopt */

int64_t sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen)
{
    socket_t *sk;
    int       ival;
    linger_t  linger;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        void                          *value;
        int                            ret;
        if (!ops || !ops->setsockopt) return -ENOPROTOOPT;
        if (!optval && optlen) return -EFAULT;
        if (optlen > SOCK_BUF_MAX) return -EINVAL;
        value = optlen ? malloc(optlen) : NULL;
        if (optlen && !value) return -ENOMEM;
        if (optlen && copy_from_user(value, optval, optlen)) {
            free(value);
            return -EFAULT;
        }
        ret = ops->setsockopt(sk->priv, level, optname, value, optlen);
        free(value);
        return ret;
    }

    /* SOL_NETLINK options */
    if (level == SOL_NETLINK) {
        if (sk->family != AF_NETLINK) return -EOPNOTSUPP;
        return (int64_t)netlink_setsockopt(sk, optname, optval, optlen);
    }

    if (level != SOL_SOCKET) return -ENOPROTOOPT;

    spin_lock(&sk->lock);

    switch (optname) {
        case SO_REUSEADDR :
            if (optlen < sizeof(int)) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if (copy_from_user(&ival, optval, sizeof(int))) {
                spin_unlock(&sk->lock);
                return -EFAULT;
            }
            sk->reuseaddr = ival ? 1 : 0;
            break;

        case SO_SNDBUF :
            if (optlen < sizeof(int)) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if (copy_from_user(&ival, optval, sizeof(int))) {
                spin_unlock(&sk->lock);
                return -EFAULT;
            }
            if (ival <= 0) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if ((uint32_t)ival > SOCK_BUF_MAX) ival = SOCK_BUF_MAX;
            sk->sndbuf = (uint32_t)ival;
            break;

        case SO_RCVBUF :
            if (optlen < sizeof(int)) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if (copy_from_user(&ival, optval, sizeof(int))) {
                spin_unlock(&sk->lock);
                return -EFAULT;
            }
            if (ival <= 0) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if ((uint32_t)ival > SOCK_BUF_MAX) ival = SOCK_BUF_MAX;
            sk->rcvbuf = (uint32_t)ival;
            break;

        case SO_LINGER :
            if (optlen < sizeof(linger_t)) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if (copy_from_user(&linger, optval, sizeof(linger_t))) {
                spin_unlock(&sk->lock);
                return -EFAULT;
            }
            sk->linger_on   = linger.l_onoff ? 1 : 0;
            sk->linger_time = (uint32_t)linger.l_linger;
            break;

        case SO_PASSCRED :
            if (optlen < sizeof(int)) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if (copy_from_user(&ival, optval, sizeof(int))) {
                spin_unlock(&sk->lock);
                return -EFAULT;
            }
            sk->passcred = ival ? 1 : 0;
            break;

        case SO_RCVLOWAT :
            if (optlen < sizeof(int)) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if (copy_from_user(&ival, optval, sizeof(int))) {
                spin_unlock(&sk->lock);
                return -EFAULT;
            }
            if (ival < 0) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            sk->rcvlowat = (uint32_t)ival;
            break;

        case SO_SNDLOWAT :
            if (optlen < sizeof(int)) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            if (copy_from_user(&ival, optval, sizeof(int))) {
                spin_unlock(&sk->lock);
                return -EFAULT;
            }
            if (ival < 0) {
                spin_unlock(&sk->lock);
                return -EINVAL;
            }
            sk->sndlowat = (uint32_t)ival;
            break;

        case SO_RCVTIMEO :
        case SO_SNDTIMEO :
            spin_unlock(&sk->lock);
            return -ENOPROTOOPT; // socket timeouts are not supported

        case SO_KEEPALIVE :
        case SO_OOBINLINE :
        case SO_BROADCAST :
        case SO_DEBUG :
        case SO_DONTROUTE :
            /* Silently ignore for UNIX sockets */
            break;

        default :
            spin_unlock(&sk->lock);
            return -ENOPROTOOPT;
    }

    spin_unlock(&sk->lock);
    return EOK;
}

/* sys_getsockopt */

int64_t sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen)
{
    socket_t *sk;
    int       ival;
    uint32_t  koptlen;
    linger_t  linger;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (sk->family == AF_INET || sk->family == AF_INET6) {
        const struct inet_backend_ops *ops = inet_backend_get();
        uint32_t                       userlen;
        uint32_t                       length;
        void                          *value;
        int                            ret;
        if (!optval || !optlen) return -EFAULT;
        if (!ops || !ops->getsockopt) return -ENOPROTOOPT;
        if (copy_from_user(&userlen, optlen, sizeof(userlen))) return -EFAULT;
        if (userlen > SOCK_BUF_MAX) return -EINVAL;
        length = userlen;
        value  = length ? malloc(length) : NULL;
        if (length && !value) return -ENOMEM;
        ret              = ops->getsockopt(sk->priv, level, optname, value, &length);
        uint32_t copylen = length < userlen ? length : userlen;
        if (ret >= 0 && copylen && copy_to_user(optval, value, copylen)) ret = -EFAULT;
        if (ret >= 0 && copy_to_user(optlen, &length, sizeof(length))) ret = -EFAULT;
        free(value);
        return ret;
    }

    /* SOL_NETLINK options */
    if (level == SOL_NETLINK) {
        if (sk->family != AF_NETLINK) return -EOPNOTSUPP;
        return (int64_t)netlink_getsockopt(sk, optname, optval, optlen);
    }

    if (level != SOL_SOCKET) return -ENOPROTOOPT;

    if (!optval || !optlen) return -EINVAL;

    spin_lock(&sk->lock);

    switch (optname) {
        case SO_TYPE :
            ival    = (int)sk->type;
            koptlen = sizeof(int);
            break;

        case SO_DOMAIN :
            ival    = (int)sk->family;
            koptlen = sizeof(int);
            break;

        case SO_PROTOCOL :
            ival    = (int)sk->protocol;
            koptlen = sizeof(int);
            break;

        case SO_ERROR :
            ival         = sk->so_error;
            sk->so_error = 0; // Clear on read
            koptlen      = sizeof(int);
            break;

        case SO_ACCEPTCONN :
            ival    = (sk->state == SOCK_STATE_LISTENING) ? 1 : 0;
            koptlen = sizeof(int);
            break;

        case SO_SNDBUF :
            ival    = (int)sk->sndbuf;
            koptlen = sizeof(int);
            break;

        case SO_RCVBUF :
            ival    = (int)sk->rcvbuf;
            koptlen = sizeof(int);
            break;

        case SO_REUSEADDR :
            ival    = sk->reuseaddr;
            koptlen = sizeof(int);
            break;

        case SO_LINGER :
            linger.l_onoff  = sk->linger_on ? 1 : 0;
            linger.l_linger = (int)sk->linger_time;
            koptlen         = sizeof(linger_t);
            spin_unlock(&sk->lock);
            if (copy_to_user(optval, &linger, sizeof(linger_t))) return -EFAULT;
            if (copy_to_user(optlen, &koptlen, sizeof(uint32_t))) return -EFAULT;
            return EOK;

        case SO_PASSCRED :
            ival    = sk->passcred;
            koptlen = sizeof(int);
            break;

        case SO_PEERCRED : {
            ucred_t cred;
            if (sk->peer) {
                cred.pid = sk->peer->pid;
                cred.uid = sk->peer->uid;
                cred.gid = sk->peer->gid;
            } else {
                cred.pid = 0;
                cred.uid = 0;
                cred.gid = 0;
            }
            koptlen = sizeof(ucred_t);
            spin_unlock(&sk->lock);
            if (copy_to_user(optval, &cred, sizeof(ucred_t))) return -EFAULT;
            if (copy_to_user(optlen, &koptlen, sizeof(uint32_t))) return -EFAULT;
            return EOK;
        }

        case SO_RCVLOWAT :
            ival    = (int)sk->rcvlowat;
            koptlen = sizeof(int);
            break;

        case SO_SNDLOWAT :
            ival    = (int)sk->sndlowat;
            koptlen = sizeof(int);
            break;

        case SO_RCVTIMEO :
        case SO_SNDTIMEO :
        default :
            spin_unlock(&sk->lock);
            return -ENOPROTOOPT;
    }

    spin_unlock(&sk->lock);

    if (koptlen == sizeof(int))
        if (copy_to_user(optval, &ival, sizeof(int))) return -EFAULT;

    if (copy_to_user(optlen, &koptlen, sizeof(uint32_t))) return -EFAULT;

    return EOK;
}

/* sys_sendmmsg */

int64_t sys_sendmmsg(int fd, void *msgvec, uint32_t vlen, int flags)
{
    socket_t *sk;
    int64_t   total = 0;

    if (!msgvec || vlen == 0) return -EINVAL;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    for (uint32_t i = 0; i < vlen; i++) {
        msghdr_t kmsg;
        iovec_t *iov;
        void    *kbuf;
        size_t   total_len;
        int64_t  ret;

        if (copy_from_user(&kmsg, (uint8_t *)msgvec + i * sizeof(msghdr_t), sizeof(msghdr_t))) {
            if (total == 0) return -EFAULT;
            break;
        }

        if (kmsg.msg_iovlen == 0 || !kmsg.msg_iov || kmsg.msg_iovlen > 1024) {
            if (total == 0) return -EINVAL;
            break;
        }

        iov = malloc(kmsg.msg_iovlen * sizeof(iovec_t));
        if (!iov) {
            if (total == 0) return -ENOMEM;
            break;
        }

        if (copy_from_user(iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(iovec_t))) {
            free(iov);
            if (total == 0) return -EFAULT;
            break;
        }

        total_len = 0;
        for (size_t j = 0; j < kmsg.msg_iovlen; j++) {
            if (iov[j].iov_len > SOCK_BUF_MAX - total_len) {
                total_len = SOCK_BUF_MAX + 1U;
                break;
            }
            total_len += iov[j].iov_len;
        }

        if (total_len > SOCK_BUF_MAX) {
            free(iov);
            if (total == 0) return -EMSGSIZE;
            break;
        }

        if (total_len == 0) {
            free(iov);
            total++;
            continue;
        }

        kbuf = malloc(total_len);
        if (!kbuf) {
            free(iov);
            if (total == 0) return -ENOMEM;
            break;
        }

        {
            uint8_t *dst  = (uint8_t *)kbuf;
            int      fail = 0;
            for (size_t j = 0; j < kmsg.msg_iovlen; j++) {
                if (iov[j].iov_len > 0) {
                    if (copy_from_user(dst, iov[j].iov_base, iov[j].iov_len)) {
                        fail = 1;
                        break;
                    }
                    dst += iov[j].iov_len;
                }
            }
            if (fail) {
                free(kbuf);
                free(iov);
                if (total == 0) return -EFAULT;
                break;
            }
        }

        ret = do_sendmsg_kern(fd, sk, &kmsg, iov, kbuf, total_len, flags, NULL, 0);

        free(kbuf);
        free(iov);

        if (ret < 0) {
            if (total == 0) return ret;
            break;
        }
        total++;
    }

    return total;
}

/* sys_recvmmsg */

int64_t sys_recvmmsg(int fd, void *msgvec, uint32_t vlen, int flags, void *timeout)
{
    socket_t *sk;
    int64_t   total = 0;

    (void)timeout;

    if (!msgvec || vlen == 0) return -EINVAL;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    for (uint32_t i = 0; i < vlen; i++) {
        msghdr_t kmsg;
        iovec_t *iov;
        void    *kbuf;
        size_t   total_len;
        int64_t  ret;

        if (copy_from_user(&kmsg, (uint8_t *)msgvec + i * sizeof(msghdr_t), sizeof(msghdr_t))) {
            if (total == 0) return -EFAULT;
            break;
        }

        if (kmsg.msg_iovlen == 0 || !kmsg.msg_iov || kmsg.msg_iovlen > 1024) {
            if (total == 0) return -EINVAL;
            break;
        }

        iov = malloc(kmsg.msg_iovlen * sizeof(iovec_t));
        if (!iov) {
            if (total == 0) return -ENOMEM;
            break;
        }

        if (copy_from_user(iov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(iovec_t))) {
            free(iov);
            if (total == 0) return -EFAULT;
            break;
        }

        total_len = 0;
        for (size_t j = 0; j < kmsg.msg_iovlen; j++) {
            if (iov[j].iov_len > SOCK_BUF_MAX - total_len) {
                total_len = SOCK_BUF_MAX;
                break;
            }
            total_len += iov[j].iov_len;
        }

        if (total_len == 0) {
            free(iov);
            total++;
            continue;
        }

        if (total_len > SOCK_BUF_MAX) total_len = SOCK_BUF_MAX;

        kbuf = malloc(total_len);
        if (!kbuf) {
            free(iov);
            if (total == 0) return -ENOMEM;
            break;
        }

        ret = do_recvmsg_kern(fd, sk, &kmsg, iov, kbuf, total_len, flags);

        /* Write back the updated msghdr */
        if (copy_to_user((uint8_t *)msgvec + i * sizeof(msghdr_t), &kmsg, sizeof(msghdr_t))) {
            free(kbuf);
            free(iov);
            if (total == 0) return -EFAULT;
            break;
        }

        free(kbuf);
        free(iov);

        if (ret < 0) {
            if (total == 0) return ret;
            break;
        }
        total++;

        if (flags & MSG_DONTWAIT) break;
    }

    return total;
}

/* Subsystem initialization */

void socket_init(void)
{
    memset(sock_bound_tab, 0, sizeof(sock_bound_tab));
    if (!inet_backend_get()) (void)inet_builtin_backend_register();

    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("socket: Failed to allocate VFS callback.\n");
        return;
    }

    cb->mount      = socket_stub_mount;
    cb->unmount    = socket_stub_unmount;
    cb->open       = socket_stub_open;
    cb->close      = socket_vfs_close;
    cb->read       = socket_vfs_read;
    cb->write      = socket_vfs_write;
    cb->readlink   = socket_stub_readlink;
    cb->mkdir      = socket_stub_mk;
    cb->mkfile     = socket_stub_mk;
    cb->link       = socket_stub_mk;
    cb->symlink    = socket_stub_mk;
    cb->stat       = socket_stub_stat;
    cb->ioctl      = socket_stub_ioctl;
    cb->dup        = socket_stub_dup;
    cb->poll       = socket_vfs_poll;
    cb->free       = socket_vfs_free;
    cb->delete     = socket_stub_del;
    cb->rename     = socket_stub_rename;
    cb->file_read  = socket_vfs_file_read;
    cb->file_write = socket_vfs_file_write;

    socket_fsid = vfs_regist(cb);
    if (socket_fsid < 0) {
        plogk("socket: Failed to register VFS callback.\n");
        free(cb);
        return;
    }

    plogk("socket: UNIX domain socket family registered (fsid=%d)\n", socket_fsid);
}
