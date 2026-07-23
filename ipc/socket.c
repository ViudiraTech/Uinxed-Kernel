/*
 *
 *      socket.c
 *      BSD Socket API implementation — UNIX domain sockets
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/vfs.h>
#include <ipc/netlink.h>
#include <ipc/socket.h>
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
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define SOCK_ACCEPT_QUEUE_INIT 16
#define SOCK_ACCEPT_QUEUE_MAX  1024
#define SOCK_BLOCKED_MAX       128
#define SOCK_BOUND_MAX         256

/* ------------------------------------------------------------------ */
/*  Blocked-socket tracking – maps a blocked socket to its task         */
/* ------------------------------------------------------------------ */

typedef struct sock_blocked {
        socket_t *sk;
        task_t   *task;
} sock_blocked_t;

static sock_blocked_t sock_blocked_tab[SOCK_BLOCKED_MAX];
static spinlock_t     sock_blocked_lock;

/* ------------------------------------------------------------------ */
/*  Bound-address registry – UNIX-domain namespace                      */
/* ------------------------------------------------------------------ */

typedef struct sock_bound {
        socket_t     *sk;
        sockaddr_un_t addr;
        uint32_t      addrlen;
        int           abstract; /* 1 = abstract namespace, 0 = pathname */
} sock_bound_t;

static sock_bound_t sock_bound_tab[SOCK_BOUND_MAX];
static spinlock_t   sock_bound_lock;

/* ------------------------------------------------------------------ */
/*  VFS filesystem id for socket nodes                                  */
/* ------------------------------------------------------------------ */

static int socket_fsid = -1;

/* ------------------------------------------------------------------ */
/*  Forward declarations – internal helpers                             */
/* ------------------------------------------------------------------ */

static void sock_blocked_register(socket_t *sk, task_t *task);
static void sock_blocked_unregister(socket_t *sk);
static void sock_blocked_wake(socket_t *sk);
static void sock_blocked_wake_all(socket_t *sk);

static int  sock_bound_lookup(const sockaddr_un_t *addr, uint32_t addrlen, int abstract, socket_t **out);
static int  sock_bound_add(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen, int abstract);
static void sock_bound_remove(socket_t *sk);

static int unix_addr_parse(const sockaddr_un_t *addr, uint32_t addrlen, int *is_abstract);
static int unix_autobind(socket_t *sk);
static int unix_bind(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen);
static int unix_listen(socket_t *sk, uint32_t backlog);
static int unix_accept(socket_t *sk, sockaddr_un_t *addr, uint32_t *addrlen, int flags);
static int unix_stream_connect(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen);
static int unix_stream_send(socket_t *sk, const void *buf, size_t len, int flags);
static int unix_stream_recv(socket_t *sk, void *buf, size_t len, int flags);
static int unix_dgram_send(socket_t *sk, const void *buf, size_t len, const sockaddr_un_t *addr, uint32_t addrlen, int flags);
static int unix_dgram_recv(socket_t *sk, void *buf, size_t len, sockaddr_un_t *addr, uint32_t *addrlen, int flags);

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

/* ------------------------------------------------------------------ */
/*  Circular buffer helpers                                             */
/* ------------------------------------------------------------------ */

static void sock_buf_init(sock_buf_t *buf, uint32_t capacity)
{
    if (capacity > SOCK_BUF_MAX) capacity = SOCK_BUF_MAX;
    if (capacity == 0) capacity = SOCK_BUF_SIZE;

    buf->data     = calloc(1, capacity);
    buf->head     = 0;
    buf->tail     = 0;
    buf->size     = 0;
    buf->capacity = capacity;
    /* spinlock is zero-initialised by calloc */
}

static void sock_buf_free(sock_buf_t *buf)
{
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
    return buf->size;
}

static uint32_t sock_buf_space(sock_buf_t *buf)
{
    return buf->capacity - buf->size;
}

static uint32_t sock_buf_write(sock_buf_t *buf, const void *data, uint32_t len)
{
    uint32_t space = buf->capacity - buf->size;
    uint32_t written;

    if (len > space) len = space;
    if (len == 0) return 0;

    written = 0;
    while (written < len) {
        uint32_t chunk;
        uint32_t pos = buf->tail;

        if (pos >= buf->head && buf->size > 0) {
            /* tail is at or after head, wrap at capacity */
            chunk = buf->capacity - pos;
        } else {
            /* tail is before head */
            chunk = buf->head - pos;
        }
        if (chunk > len - written) chunk = len - written;

        memcpy(buf->data + pos, (const uint8_t *)data + written, chunk);
        written += chunk;
        buf->tail = (pos + chunk) % buf->capacity;
        buf->size += chunk;
    }

    return written;
}

static uint32_t sock_buf_read(sock_buf_t *buf, void *data, uint32_t len)
{
    uint32_t rd;

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

/* ------------------------------------------------------------------ */
/*  Blocked-socket tracking                                             */
/* ------------------------------------------------------------------ */

static void sock_blocked_register(socket_t *sk, task_t *task)
{
    spin_lock(&sock_blocked_lock);
    for (int i = 0; i < SOCK_BLOCKED_MAX; i++) {
        if (sock_blocked_tab[i].sk == NULL) {
            sock_blocked_tab[i].sk   = sk;
            sock_blocked_tab[i].task = task;
            spin_unlock(&sock_blocked_lock);
            return;
        }
    }
    spin_unlock(&sock_blocked_lock);
    plogk("socket: blocked table overflow\n");
}

static void sock_blocked_unregister(socket_t *sk)
{
    spin_lock(&sock_blocked_lock);
    for (int i = 0; i < SOCK_BLOCKED_MAX; i++) {
        if (sock_blocked_tab[i].sk == sk) {
            sock_blocked_tab[i].sk   = NULL;
            sock_blocked_tab[i].task = NULL;
            break;
        }
    }
    spin_unlock(&sock_blocked_lock);
}

static void sock_blocked_wake(socket_t *sk)
{
    spin_lock(&sock_blocked_lock);
    for (int i = 0; i < SOCK_BLOCKED_MAX; i++) {
        if (sock_blocked_tab[i].sk == sk) {
            task_t *t                = sock_blocked_tab[i].task;
            sock_blocked_tab[i].sk   = NULL;
            sock_blocked_tab[i].task = NULL;
            spin_unlock(&sock_blocked_lock);
            if (t) task_wakeup(t);
            return;
        }
    }
    spin_unlock(&sock_blocked_lock);
}

static void sock_blocked_wake_all(socket_t *sk)
{
    spin_lock(&sock_blocked_lock);
    for (int i = 0; i < SOCK_BLOCKED_MAX; i++) {
        if (sock_blocked_tab[i].sk == sk) {
            task_t *t                = sock_blocked_tab[i].task;
            sock_blocked_tab[i].sk   = NULL;
            sock_blocked_tab[i].task = NULL;
            if (t) task_wakeup(t);
        }
    }
    spin_unlock(&sock_blocked_lock);
}

/* ------------------------------------------------------------------ */
/*  Bound-address registry                                              */
/* ------------------------------------------------------------------ */

static int sock_bound_lookup(const sockaddr_un_t *addr, uint32_t addrlen, int abstract, socket_t **out)
{
    spin_lock(&sock_bound_lock);
    for (int i = 0; i < SOCK_BOUND_MAX; i++) {
        if (sock_bound_tab[i].sk == NULL) continue;
        if (sock_bound_tab[i].abstract != abstract) continue;
        if (sock_bound_tab[i].addrlen != addrlen) continue;

        if (abstract) {
            /* Abstract: compare entire path including leading NUL */
            if (memcmp(sock_bound_tab[i].addr.sun_path, addr->sun_path, addrlen - sizeof(uint16_t)) == 0) {
                *out = sock_bound_tab[i].sk;
                spin_unlock(&sock_bound_lock);
                return EOK;
            }
        } else {
            /* Pathname: compare as NUL-terminated strings */
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
            sock_bound_tab[i].sk = NULL;
            break;
        }
    }
    spin_unlock(&sock_bound_lock);
}

/* ------------------------------------------------------------------ */
/*  UNIX address parsing                                                */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Socket lifecycle                                                    */
/* ------------------------------------------------------------------ */

static socket_t *socket_alloc(uint16_t family, uint16_t type, uint16_t protocol)
{
    socket_t *sk;

    /* Validate family — Netlink handled by netlink layer */
    if (family == AF_NETLINK) {
        return netlink_sock_alloc(protocol);
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

    sock_buf_init(&sk->recv_buf, SOCK_BUF_SIZE);
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

    /* Set credentials from current process */
    {
        process_t *proc = process_current();
        if (proc) {
            sk->pid = (uint32_t)(proc->task ? proc->task->pid : 0);
            sk->uid = proc->uid;
            sk->gid = proc->gid;
        }
    }

    /* Set polymorphic operations */
    if (type == SOCK_DGRAM) {
        sk->socket_read  = NULL; /* dgram uses recvfrom directly */
        sk->socket_write = NULL; /* dgram uses sendto directly */
    } else {
        sk->socket_read  = NULL; /* handled by type-specific paths */
        sk->socket_write = NULL;
    }
    sk->socket_poll  = NULL;
    sk->socket_close = NULL;

    return sk;
}

static void socket_free(socket_t *sk)
{
    if (!sk) return;

    sk->refcount--;
    if (sk->refcount > 0) return;

    /* Netlink cleanup */
    if (sk->family == AF_NETLINK && sk->priv) {
        netlink_close(sk);
    }

    /* Remove from bound registry */
    sock_bound_remove(sk);

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
            if (sk->accept_queue[i]) { socket_free(sk->accept_queue[i]); }
        }
        free(sk->accept_queue);
        sk->accept_queue = NULL;
    }

    /* Free buffers */
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

/* ------------------------------------------------------------------ */
/*  Socket fd installation                                              */
/* ------------------------------------------------------------------ */

int socket_fd_install(socket_t *sk)
{
    process_t *proc;
    vfs_node_t node;
    int        fd;

    if (!sk) return -EINVAL;

    proc = process_current();
    if (!proc) return -ESRCH;

    node = vfs_node_alloc(NULL, "[socket]");
    if (!node) return -ENOMEM;

    node->type   = file_socket;
    node->handle = sk;
    node->fsid   = socket_fsid;
    node->size   = 0;
    node->mode   = 0600;

    sk->node = node;
    socket_ref(sk);

    fd = process_fd_install(proc, node, 0);
    if (fd < 0) {
        sk->node = NULL;
        vfs_free(node);
        return fd;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/*  socket_from_fd – find a socket by fd in the current process         */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  UNIX autobind                                                       */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  UNIX bind                                                           */
/* ------------------------------------------------------------------ */

static int unix_bind(socket_t *sk, const sockaddr_un_t *addr, uint32_t addrlen)
{
    int abstract;
    int ret;

    if (sk->state != SOCK_STATE_UNCONNECTED) return -EINVAL;

    ret = unix_addr_parse(addr, addrlen, &abstract);
    if (ret != EOK) return ret;

    /* Unnamed address -> autobind */
    if (addrlen == sizeof(uint16_t)) { return unix_autobind(sk); }

    if (!abstract) {
        /* Pathname socket: create VFS entry */
        const char *path = addr->sun_path;
        vfs_node_t  file_node;

        /* Check path length */
        if (strnlen_local(path, UNIX_PATH_MAX) >= UNIX_PATH_MAX) return -ENAMETOOLONG;

        /* Try to create the file */
        ret = vfs_mkfile(path);
        if (ret != EOK) {
            /* If file exists, try to unlink it first (if SO_REUSEADDR) */
            if (sk->reuseaddr) {
                vfs_node_t existing = vfs_open(path);
                if (existing) {
                    if (existing->type == file_socket) { vfs_delete(existing); }
                    vfs_close(existing);
                }
                ret = vfs_mkfile(path);
                if (ret != EOK) return -EADDRINUSE;
            } else {
                return -EADDRINUSE;
            }
        }

        file_node = vfs_open(path);
        if (!file_node) return -EADDRNOTAVAIL;

        file_node->type   = file_socket;
        file_node->handle = sk;
        file_node->fsid   = socket_fsid;
        sk->node          = file_node;

        /* Mark as visited */
        file_node->visited = 1;
    }

    /* Register in bound table */
    ret = sock_bound_add(sk, addr, addrlen, abstract);
    if (ret != EOK) {
        if (!abstract && sk->node) { /* Clean up VFS node */
        }
        return ret;
    }

    memcpy(&sk->local_addr, addr, sizeof(sockaddr_un_t));
    sk->local_addr_len = addrlen;

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  UNIX listen                                                         */
/* ------------------------------------------------------------------ */

static int unix_listen(socket_t *sk, uint32_t backlog)
{
    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    if (sk->state == SOCK_STATE_CONNECTED) return -EISCONN;

    spin_lock(&sk->lock);

    if (sk->state == SOCK_STATE_LISTENING) {
        /* Already listening – just update backlog */
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

/* ------------------------------------------------------------------ */
/*  UNIX stream connect                                                 */
/* ------------------------------------------------------------------ */

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

    /* Check backlog */
    if (listener->accept_queue_len >= listener->accept_queue_cap) {
        spin_unlock(&listener->lock);
        return -ECONNREFUSED;
    }

    /* Create a new server-side socket */
    socket_t *server = calloc(1, sizeof(socket_t));
    if (!server) {
        spin_unlock(&listener->lock);
        return -ENOMEM;
    }

    server->state    = SOCK_STATE_CONNECTED;
    server->family   = sk->family;
    server->type     = sk->type;
    server->protocol = sk->protocol;
    server->flags    = 0;
    server->refcount = 1;

    sock_buf_init(&server->recv_buf, SOCK_BUF_SIZE);
    sock_buf_init(&server->send_buf, SOCK_BUF_SIZE);
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

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  UNIX accept                                                         */
/* ------------------------------------------------------------------ */

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
        task_block();
        spin_lock(&sk->lock);
        sock_blocked_unregister(sk);
    }

    /* Dequeue first client */
    client = sk->accept_queue[0];
    sk->accept_queue_len--;

    /* Shift remaining entries */
    for (uint32_t i = 0; i < sk->accept_queue_len; i++) { sk->accept_queue[i] = sk->accept_queue[i + 1]; }
    sk->accept_queue[sk->accept_queue_len] = NULL;

    spin_unlock(&sk->lock);

    /* Copy peer address to user */
    if (addr && addrlen) {
        uint32_t kaddrlen;
        if (client->peer_addr_len > 0) {
            kaddrlen = client->peer_addr_len;
            uint32_t userlen;
            if (copy_from_user(&userlen, addrlen, sizeof(uint32_t))) {
                socket_unref(client);
                return -EFAULT;
            }
            if (kaddrlen > userlen) kaddrlen = userlen;
            if (copy_to_user(addr, &client->peer_addr, kaddrlen)) {
                socket_unref(client);
                return -EFAULT;
            }
            if (copy_to_user(addrlen, &kaddrlen, sizeof(uint32_t))) {
                socket_unref(client);
                return -EFAULT;
            }
        }
    }

    /* Install the client socket into the current process */
    return socket_fd_install(client);
}

/* ------------------------------------------------------------------ */
/*  UNIX stream send                                                    */
/* ------------------------------------------------------------------ */

static int unix_stream_send(socket_t *sk, const void *buf, size_t len, int flags)
{
    socket_t *peer;
    int       is_nonblock;
    uint32_t  total_written = 0;
    int       ret;

    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);

    spin_lock(&sk->lock);

    if (sk->shutdown_mask & SHUT_WR) {
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

    if (peer->shutdown_mask & SHUT_RD) {
        spin_unlock(&peer->lock);
        socket_unref(peer);
        return -EPIPE;
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
            task_block();
            spin_lock(&peer->lock);
            sock_blocked_unregister(sk);

            if (peer->shutdown_mask & SHUT_RD) {
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

    spin_unlock(&peer->lock);

    /* Wake the peer if it's blocked on recv */
    sock_blocked_wake(peer);

    socket_unref(peer);

    ret = (int)total_written;
    if (ret == 0 && !is_nonblock) ret = -EPIPE;
    return ret;
}

/* ------------------------------------------------------------------ */
/*  UNIX stream recv                                                    */
/* ------------------------------------------------------------------ */

static int unix_stream_recv(socket_t *sk, void *buf, size_t len, int flags)
{
    int      is_nonblock;
    int      peek;
    uint32_t total_read = 0;
    int      ret;

    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);
    peek        = (flags & MSG_PEEK) ? 1 : 0;

    spin_lock(&sk->lock);

    while (total_read < len) {
        uint32_t avail = sock_buf_available(&sk->recv_buf);

        if (avail == 0) {
            if (total_read > 0) break;
            if (sk->shutdown_mask & SHUT_RD) {
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
            if (!sk->peer || sk->peer->state == SOCK_STATE_DISCONNECTING) {
                spin_unlock(&sk->lock);
                return 0;
            }
            sock_blocked_register(sk, current_task());
            spin_unlock(&sk->lock);
            task_block();
            spin_lock(&sk->lock);
            sock_blocked_unregister(sk);
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

    /* Wake peer if it was blocked on send (buffer space freed) */
    if (!peek && total_read > 0 && sk->peer) { sock_blocked_wake(sk->peer); }

    ret = (int)total_read;
    if (ret == 0 && !(flags & MSG_PEEK) && !(sk->shutdown_mask & SHUT_RD)) { return 0; /* EOF */ }
    return ret;
}

/* ------------------------------------------------------------------ */
/*  UNIX datagram send                                                  */
/* ------------------------------------------------------------------ */

static int unix_dgram_send(socket_t *sk, const void *buf, size_t len, const sockaddr_un_t *addr, uint32_t addrlen, int flags)
{
    socket_t *dest = NULL;
    int       abstract;
    int       ret;
    int       is_nonblock;

    if (sk->type != SOCK_DGRAM) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);

    if (len > SOCK_BUF_MAX) return -EMSGSIZE;

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

    /* Check if there's enough space */
    uint32_t space = sock_buf_space(&dest->recv_buf);
    if (len > space) {
        if (is_nonblock) {
            spin_unlock(&dest->lock);
            return -EAGAIN;
        }
        /* Wait for space */
        while (sock_buf_space(&dest->recv_buf) < (uint32_t)len) {
            sock_blocked_register(sk, current_task());
            spin_unlock(&dest->lock);
            task_block();
            spin_lock(&dest->lock);
            sock_blocked_unregister(sk);
        }
    }

    /* For dgram, we need to preserve message boundaries. */
    /* We prepend a small header with the sender address and length. */

    /* Calculate total: 4 bytes length + sender address */
    uint32_t hdr_len = 4 + sizeof(sockaddr_un_t);
    uint32_t total   = hdr_len + (uint32_t)len;

    if (total > sock_buf_space(&dest->recv_buf)) {
        spin_unlock(&dest->lock);
        return -ENOBUFS;
    }

    /* Write header: message length */
    uint32_t msg_len = (uint32_t)len;
    sock_buf_write(&dest->recv_buf, &msg_len, 4);

    /* Write header: sender address */
    sock_buf_write(&dest->recv_buf, &sk->local_addr, sizeof(sockaddr_un_t));

    /* Write payload */
    uint32_t written = sock_buf_write(&dest->recv_buf, buf, (uint32_t)len);

    spin_unlock(&dest->lock);

    /* Wake destination */
    sock_blocked_wake(dest);

    if (written < (uint32_t)len) return -ENOBUFS;

    return (int)written;
}

/* ------------------------------------------------------------------ */
/*  UNIX datagram recv                                                  */
/* ------------------------------------------------------------------ */

static int unix_dgram_recv(socket_t *sk, void *buf, size_t len, sockaddr_un_t *addr, uint32_t *addrlen, int flags)
{
    int           is_nonblock;
    int           peek;
    uint32_t      msg_len;
    sockaddr_un_t sender_addr;

    if (sk->type != SOCK_DGRAM) return -EOPNOTSUPP;

    is_nonblock = (flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK);
    peek        = (flags & MSG_PEEK) ? 1 : 0;

    spin_lock(&sk->lock);

    /* Wait for at least the header */
    while (sock_buf_available(&sk->recv_buf) < 4 + sizeof(sockaddr_un_t)) {
        if (is_nonblock) {
            spin_unlock(&sk->lock);
            return -EAGAIN;
        }
        sock_blocked_register(sk, current_task());
        spin_unlock(&sk->lock);
        task_block();
        spin_lock(&sk->lock);
        sock_blocked_unregister(sk);
    }

    /* Read header */
    if (peek) {
        sock_buf_peek(&sk->recv_buf, &msg_len, 4);
        sock_buf_peek(&sk->recv_buf, &sender_addr, sizeof(sockaddr_un_t));
        /* Need to peek past the header - skip 4 bytes then peek addr */
        /* Actually, peek is sequential, so we need to re-peek from start */
        /* Let's read and then push back. For simplicity, read and discard. */
        /* For proper peek, we'd need a more complex approach. */
        /* For now, peek reads the header but doesn't advance. */
        uint8_t hdr_buf[4 + sizeof(sockaddr_un_t)];
        sock_buf_peek(&sk->recv_buf, hdr_buf, sizeof(hdr_buf));
        memcpy(&msg_len, hdr_buf, 4);
        memcpy(&sender_addr, hdr_buf + 4, sizeof(sockaddr_un_t));
    } else {
        sock_buf_read(&sk->recv_buf, &msg_len, 4);
        sock_buf_read(&sk->recv_buf, &sender_addr, sizeof(sockaddr_un_t));
    }

    /* Now read the payload */
    uint32_t payload = (uint32_t)len;
    if (payload > msg_len) payload = msg_len;

    uint32_t avail = sock_buf_available(&sk->recv_buf);
    if (payload > avail) payload = avail;

    uint32_t rd;
    if (peek) {
        rd = sock_buf_peek(&sk->recv_buf, buf, payload);
    } else {
        rd = sock_buf_read(&sk->recv_buf, buf, payload);
    }

    /* If we read less than the full message, discard the rest */
    if (!peek && msg_len > rd) {
        uint32_t remaining  = msg_len - rd;
        uint32_t to_discard = remaining;
        if (to_discard > sock_buf_available(&sk->recv_buf)) to_discard = sock_buf_available(&sk->recv_buf);
        /* Discard by advancing head */
        while (to_discard > 0) {
            uint32_t chunk     = to_discard;
            uint32_t avail_now = sock_buf_available(&sk->recv_buf);
            if (chunk > avail_now) chunk = avail_now;
            sk->recv_buf.head = (sk->recv_buf.head + chunk) % sk->recv_buf.capacity;
            sk->recv_buf.size -= chunk;
            to_discard -= chunk;
        }
    }

    spin_unlock(&sk->lock);

    /* Copy sender address to user */
    if (addr && addrlen) {
        uint32_t userlen;
        if (copy_from_user(&userlen, addrlen, sizeof(uint32_t))) { return -EFAULT; }
        uint32_t copy_len = sizeof(sockaddr_un_t);
        if (copy_len > userlen) copy_len = userlen;
        if (copy_to_user(addr, &sender_addr, copy_len)) { return -EFAULT; }
        if (copy_to_user(addrlen, &copy_len, sizeof(uint32_t))) { return -EFAULT; }
    }

    return (int)rd;
}

/* ------------------------------------------------------------------ */
/*  Socket poll support                                                 */
/* ------------------------------------------------------------------ */

static int socket_poll(socket_t *sk, size_t events)
{
    int revents = 0;

    if (!sk) return 0;

    spin_lock(&sk->lock);

    switch (sk->state) {
        case SOCK_STATE_LISTENING :
            /* POLLIN = connection waiting */
            if (sk->accept_queue_len > 0) revents |= 0x001; /* POLLIN */
            revents |= 0x004;                               /* POLLOUT (always writable for listen) */
            break;

        case SOCK_STATE_CONNECTED :
            /* POLLIN = data available or peer closed */
            if (sock_buf_available(&sk->recv_buf) > 0) revents |= 0x001;
            if (sk->shutdown_mask & SHUT_RD) revents |= 0x001; /* EOF readable */

            /* POLLOUT = send buffer not full */
            if (sk->peer && sock_buf_space(&sk->peer->recv_buf) > 0) revents |= 0x004;
            if (sk->shutdown_mask & SHUT_WR) revents |= 0x004; /* write will error, but poll says writable */

            /* POLLHUP = peer disconnected */
            if (!sk->peer || sk->peer->state == SOCK_STATE_DISCONNECTING) revents |= 0x010; /* POLLHUP */
            break;

        case SOCK_STATE_UNCONNECTED :
            /* DGRAM sockets can always send/recv if bound */
            if (sk->type == SOCK_DGRAM) {
                if (sock_buf_available(&sk->recv_buf) > 0) revents |= 0x001;
                revents |= 0x004; /* dgram always writable */
            } else {
                revents |= 0x010; /* POLLHUP - not connected */
            }
            break;

        case SOCK_STATE_DISCONNECTING :
            revents |= 0x010; /* POLLHUP */
            if (sock_buf_available(&sk->recv_buf) > 0) revents |= 0x001;
            break;

        default :
            break;
    }

    /* Check error */
    if (sk->so_error) revents |= 0x008; /* POLLERR */

    spin_unlock(&sk->lock);

    return revents & (int)events;
}

/* ------------------------------------------------------------------ */
/*  VFS callbacks                                                       */
/* ------------------------------------------------------------------ */

static size_t socket_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    socket_t *sk = (socket_t *)file;
    int       ret;
    (void)offset;

    if (!sk) return (size_t)-1;

    /* Use polymorphic op if set (netlink) */
    if (sk->socket_read) {
        ret = sk->socket_read(sk, addr, size, NULL, NULL);
        if (ret < 0) return (size_t)-1;
        return (size_t)ret;
    }

    if (sk->type == SOCK_DGRAM) {
        ret = unix_dgram_recv(sk, addr, size, NULL, NULL, sk->flags);
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

    /* Use polymorphic op if set (netlink) */
    if (sk->socket_write) {
        ret = sk->socket_write(sk, addr, size, NULL, 0);
        if (ret < 0) return (size_t)-1;
        return (size_t)ret;
    }

    if (sk->type == SOCK_DGRAM) {
        ret = unix_dgram_send(sk, addr, size, NULL, 0, sk->flags);
    } else {
        ret = unix_stream_send(sk, addr, size, 0);
    }

    if (ret < 0) return (size_t)-1;
    return (size_t)ret;
}

static int socket_vfs_poll(void *file, size_t events)
{
    socket_t *sk = (socket_t *)file;
    if (!sk) return 0;
    /* Use polymorphic op if set (netlink) */
    if (sk->socket_poll) return sk->socket_poll(sk, events);
    return socket_poll(sk, events);
}

static void socket_vfs_close(void *current)
{
    socket_t *sk = (socket_t *)current;
    if (!sk) return;

    /* Wake blocked tasks */
    sock_blocked_wake_all(sk);
}

static int socket_vfs_free(void *handle)
{
    socket_t *sk = (socket_t *)handle;
    if (!sk) return -EINVAL;

    /* Netlink cleanup */
    if (sk->family == AF_NETLINK && sk->priv) {
        netlink_close(sk);
    }

    /* Remove from bound registry */
    sock_bound_remove(sk);

    /* Wake all blocked tasks */
    sock_blocked_wake_all(sk);

    /* Disconnect from peer */
    if (sk->peer) {
        socket_t *peer = sk->peer;
        sk->peer       = NULL;
        peer->peer     = NULL;
        peer->state    = SOCK_STATE_DISCONNECTING;
        sock_blocked_wake_all(peer);
        socket_unref(peer);
    }

    /* Free accept queue */
    if (sk->accept_queue) {
        for (uint32_t i = 0; i < sk->accept_queue_len; i++) {
            if (sk->accept_queue[i]) { socket_unref(sk->accept_queue[i]); }
        }
        free(sk->accept_queue);
        sk->accept_queue = NULL;
    }

    /* Free buffers */
    sock_buf_free(&sk->recv_buf);
    sock_buf_free(&sk->send_buf);

    /* Clear VFS node reference */
    sk->node = NULL;

    free(sk);
    return EOK;
}

/* ---- VFS stubs ---- */

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
    (void)f;
    (void)o;
    (void)a;
    return -ENOSYS;
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

/* ------------------------------------------------------------------ */
/*  System call implementations                                         */
/* ------------------------------------------------------------------ */

/* ---- sys_socket ---- */

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
    if (type & SOCK_CLOEXEC) {
        /* CLOEXEC handled by fd install flags */
        type &= ~SOCK_CLOEXEC;
    }

    sock_family = (uint16_t)family;
    sock_type   = (uint16_t)type;

    if (sock_family == AF_NETLINK) {
        /* Netlink uses SOCK_RAW or SOCK_DGRAM */
        if (sock_type != SOCK_RAW && sock_type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    } else if (sock_family != AF_UNIX && sock_family != AF_LOCAL) {
        return -EAFNOSUPPORT;
    } else {
        if (sock_type != SOCK_STREAM && sock_type != SOCK_DGRAM && sock_type != SOCK_SEQPACKET)
            return -ESOCKTNOSUPPORT;
    }

    sk = socket_alloc(sock_family, sock_type, (uint16_t)protocol);
    if (!sk) return -ENOMEM;

    sk->flags = extra_flags;

    fd = socket_fd_install(sk);
    if (fd < 0) {
        socket_free(sk);
        return fd;
    }

    return (int64_t)fd;
}

/* ---- sys_bind ---- */

int64_t sys_bind(int fd, const sockaddr_un_t *addr, uint32_t addrlen)
{
    socket_t     *sk;
    sockaddr_un_t kaddr;
    int           ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!addr) return -EINVAL;

    /* Netlink bind */
    if (sk->family == AF_NETLINK) {
        sockaddr_nl_t nladdr;
        if (addrlen != sizeof(sockaddr_nl_t)) return -EINVAL;
        if (copy_from_user(&nladdr, (const void *)addr, addrlen)) return -EFAULT;
        return (int64_t)netlink_bind(sk, &nladdr, addrlen);
    }

    if (addrlen > sizeof(sockaddr_un_t)) return -EINVAL;

    if (copy_from_user(&kaddr, addr, addrlen)) return -EFAULT;

    spin_lock(&sk->lock);
    ret = unix_bind(sk, &kaddr, addrlen);
    spin_unlock(&sk->lock);

    return (int64_t)ret;
}

/* ---- sys_listen ---- */

int64_t sys_listen(int fd, int backlog)
{
    socket_t *sk;
    int       ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (backlog < 0) backlog = 0;

    ret = unix_listen(sk, (uint32_t)backlog);

    return (int64_t)ret;
}

/* ---- sys_accept ---- */

int64_t sys_accept(int fd, sockaddr_un_t *addr, uint32_t *addrlen, int flags)
{
    socket_t *sk;
    int       ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    ret = unix_accept(sk, addr, addrlen, flags);

    return (int64_t)ret;
}

/* ---- sys_connect ---- */

int64_t sys_connect(int fd, const sockaddr_un_t *addr, uint32_t addrlen)
{
    socket_t     *sk;
    sockaddr_un_t kaddr;
    int           ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    /* Netlink is connectionless */
    if (sk->family == AF_NETLINK) return -EOPNOTSUPP;

    if (!addr) return -EINVAL;

    if (addrlen > sizeof(sockaddr_un_t)) return -EINVAL;

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

    /* Auto-bind if not already bound */
    if (sk->local_addr_len == 0) {
        ret = unix_autobind(sk);
        if (ret != EOK) return (int64_t)ret;
    }

    ret = unix_stream_connect(sk, &kaddr, addrlen);

    return (int64_t)ret;
}

/* ---- sys_sendto ---- */

int64_t sys_sendto(int fd, const void *buf, size_t len, int flags, const sockaddr_un_t *addr, uint32_t addrlen)
{
    socket_t     *sk;
    sockaddr_un_t kaddr;
    void         *kbuf;
    int           ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!buf && len > 0) return -EFAULT;

    if (len > SOCK_BUF_MAX) return -EMSGSIZE;

    /* Netlink: use polymorphic write op */
    if (sk->socket_write) {
        void *kbuf_nl = malloc(len);
        if (!kbuf_nl) return -ENOMEM;
        if (copy_from_user(kbuf_nl, buf, len)) { free(kbuf_nl); return -EFAULT; }
        int ret_nl = sk->socket_write(sk, kbuf_nl, len, (void *)addr, addrlen);
        free(kbuf_nl);
        return (int64_t)ret_nl;
    }

    kbuf = malloc(len);
    if (!kbuf) return -ENOMEM;

    if (copy_from_user(kbuf, buf, len)) {
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
    } else {
        ret = unix_stream_send(sk, kbuf, len, flags);
    }

    free(kbuf);
    return (int64_t)ret;
}

/* ---- sys_recvfrom ---- */

int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags, sockaddr_un_t *addr, uint32_t *addrlen)
{
    socket_t *sk;
    void     *kbuf;
    int       ret;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!buf || len == 0) return -EINVAL;

    if (len > SOCK_BUF_MAX) len = SOCK_BUF_MAX;

    /* Netlink: use polymorphic read op */
    if (sk->socket_read) {
        kbuf = malloc(len);
        if (!kbuf) return -ENOMEM;
        ret = sk->socket_read(sk, kbuf, len, addr, addrlen);
        if (ret > 0) {
            if (copy_to_user(buf, kbuf, (size_t)ret)) { free(kbuf); return -EFAULT; }
        }
        free(kbuf);
        return (int64_t)ret;
    }

    kbuf = malloc(len);
    if (!kbuf) return -ENOMEM;

    if (sk->type == SOCK_DGRAM) {
        ret = unix_dgram_recv(sk, kbuf, len, addr, addrlen, flags);
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

/* ---- Internal: sendmsg/recvmsg with kernel buffers ---- */

static int64_t do_sendmsg_kern(int fd, socket_t *sk, const msghdr_t *kmsg, const iovec_t *iov, const void *kbuf, size_t total_len, int flags)
{
    int ret;
    (void)fd;
    (void)iov;

    /* Netlink: use polymorphic write */
    if (sk->socket_write) {
        return (int64_t)sk->socket_write(sk, kbuf, total_len,
                                         kmsg->msg_name, kmsg->msg_namelen);
    }

    if (sk->type == SOCK_DGRAM) {
        sockaddr_un_t kaddr;
        if (kmsg->msg_name && kmsg->msg_namelen > 0) {
            if (kmsg->msg_namelen > sizeof(sockaddr_un_t)) return -EINVAL;
            if (copy_from_user(&kaddr, kmsg->msg_name, kmsg->msg_namelen)) return -EFAULT;
            ret = unix_dgram_send(sk, kbuf, total_len, &kaddr, kmsg->msg_namelen, flags);
        } else {
            ret = unix_dgram_send(sk, kbuf, total_len, NULL, 0, flags);
        }
    } else {
        ret = unix_stream_send(sk, kbuf, total_len, flags);
    }

    return (int64_t)ret;
}

static int64_t do_recvmsg_kern(int fd, socket_t *sk, msghdr_t *kmsg, const iovec_t *iov, void *kbuf, size_t total_len, int flags)
{
    int ret;
    int msg_flags = 0;
    (void)fd;

    /* Netlink: use polymorphic read */
    if (sk->socket_read) {
        ret = sk->socket_read(sk, kbuf, total_len,
                              (void *)(uintptr_t)kmsg->msg_name,
                              (uint32_t *)(uintptr_t)kmsg->msg_namelen);
        if (ret > 0) {
            uint8_t *src = (uint8_t *)kbuf;
            size_t remaining = (size_t)ret;
            for (size_t i = 0; i < kmsg->msg_iovlen && remaining > 0; i++) {
                size_t chunk = iov[i].iov_len;
                if (chunk > remaining) chunk = remaining;
                if (copy_to_user(iov[i].iov_base, src, chunk)) return -EFAULT;
                src += chunk;
                remaining -= chunk;
            }
        }
        kmsg->msg_flags = 0;
        kmsg->msg_controllen = 0;
        return (int64_t)ret;
    }

    if (sk->type == SOCK_DGRAM) {
        ret = unix_dgram_recv(sk, kbuf, total_len, (sockaddr_un_t *)(uintptr_t)kmsg->msg_name, (uint32_t *)(uintptr_t)kmsg->msg_namelen, flags);
    } else {
        ret = unix_stream_recv(sk, kbuf, total_len, flags);
    }

    if (ret > 0) {
        /* Scatter data to iovecs */
        uint8_t *src       = (uint8_t *)kbuf;
        size_t   remaining = (size_t)ret;
        for (size_t i = 0; i < kmsg->msg_iovlen && remaining > 0; i++) {
            size_t chunk = iov[i].iov_len;
            if (chunk > remaining) chunk = remaining;
            if (copy_to_user(iov[i].iov_base, src, chunk)) return -EFAULT;
            src += chunk;
            remaining -= chunk;
        }
    }

    /* Write back msg_flags */
    kmsg->msg_flags      = msg_flags;
    kmsg->msg_controllen = 0;

    return (int64_t)ret;
}

/* ---- sys_sendmsg ---- */

int64_t sys_sendmsg(int fd, const msghdr_t *msg, int flags)
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
    for (size_t i = 0; i < kmsg.msg_iovlen; i++) total_len += iov[i].iov_len;

    if (total_len > SOCK_BUF_MAX) {
        free(iov);
        return -EMSGSIZE;
    }

    if (total_len == 0) {
        free(iov);
        return 0;
    }

    kbuf = malloc(total_len);
    if (!kbuf) {
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

    ret = do_sendmsg_kern(fd, sk, &kmsg, iov, kbuf, total_len, flags);

    free(kbuf);
    free(iov);
    return ret;
}

/* ---- sys_recvmsg ---- */

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
    for (size_t i = 0; i < kmsg.msg_iovlen; i++) total_len += iov[i].iov_len;

    if (total_len == 0) {
        free(iov);
        return 0;
    }

    if (total_len > SOCK_BUF_MAX) total_len = SOCK_BUF_MAX;

    kbuf = malloc(total_len);
    if (!kbuf) {
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

/* ---- sys_shutdown ---- */

int64_t sys_shutdown(int fd, int how)
{
    socket_t *sk;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    /* Netlink is connectionless */
    if (sk->family == AF_NETLINK) return -EOPNOTSUPP;

    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -EINVAL;

    spin_lock(&sk->lock);

    if (sk->state != SOCK_STATE_CONNECTED && sk->state != SOCK_STATE_LISTENING) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    sk->shutdown_mask |= (uint32_t)how;

    /* Wake blocked tasks */
    sock_blocked_wake_all(sk);

    if (sk->peer) { sock_blocked_wake_all(sk->peer); }

    spin_unlock(&sk->lock);

    return EOK;
}

/* ---- sys_socketpair ---- */

int64_t sys_socketpair(int domain, int type, int protocol, int sv[2])
{
    socket_t *sk1, *sk2;
    int       fd1, fd2;
    int       sv_kern[2];

    if (domain != AF_UNIX && domain != AF_LOCAL) return -EAFNOSUPPORT;

    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) return -ESOCKTNOSUPPORT;

    if (!sv) return -EFAULT;

    /* Create two connected sockets */
    sk1 = socket_alloc((uint16_t)domain, (uint16_t)type, (uint16_t)protocol);
    if (!sk1) return -ENOMEM;

    sk2 = socket_alloc((uint16_t)domain, (uint16_t)type, (uint16_t)protocol);
    if (!sk2) {
        socket_free(sk1);
        return -ENOMEM;
    }

    /* Autobind both */
    {
        int r = unix_autobind(sk1);
        if (r != EOK) {
            socket_free(sk1);
            socket_free(sk2);
            return (int64_t)r;
        }
        r = unix_autobind(sk2);
        if (r != EOK) {
            socket_free(sk1);
            socket_free(sk2);
            return (int64_t)r;
        }
    }

    /* Link them */
    sk1->peer = sk2;
    socket_ref(sk2);
    sk2->peer = sk1;
    socket_ref(sk1);

    sk1->state = SOCK_STATE_CONNECTED;
    sk2->state = SOCK_STATE_CONNECTED;

    /* Copy peer addresses */
    memcpy(&sk1->peer_addr, &sk2->local_addr, sizeof(sockaddr_un_t));
    sk1->peer_addr_len = sk2->local_addr_len;
    memcpy(&sk2->peer_addr, &sk1->local_addr, sizeof(sockaddr_un_t));
    sk2->peer_addr_len = sk1->local_addr_len;

    /* Install file descriptors */
    fd1 = socket_fd_install(sk1);
    if (fd1 < 0) {
        socket_free(sk1);
        socket_free(sk2);
        return (int64_t)fd1;
    }

    fd2 = socket_fd_install(sk2);
    if (fd2 < 0) {
        process_t *proc = process_current();
        if (proc) process_fd_close(proc, fd1);
        socket_free(sk1);
        socket_free(sk2);
        return (int64_t)fd2;
    }

    sv_kern[0] = fd1;
    sv_kern[1] = fd2;

    if (copy_to_user(sv, sv_kern, sizeof(sv_kern))) {
        process_t *proc = process_current();
        if (proc) {
            process_fd_close(proc, fd1);
            process_fd_close(proc, fd2);
        }
        socket_free(sk1);
        socket_free(sk2);
        return -EFAULT;
    }

    return EOK;
}

/* ---- sys_getsockname ---- */

int64_t sys_getsockname(int fd, sockaddr_un_t *addr, uint32_t *addrlen)
{
    socket_t *sk;
    uint32_t  kaddrlen;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!addr || !addrlen) return -EINVAL;

    spin_lock(&sk->lock);

    kaddrlen = sk->local_addr_len;
    if (kaddrlen == 0) {
        spin_unlock(&sk->lock);
        return -EADDRNOTAVAIL;
    }

    uint32_t userlen;
    spin_unlock(&sk->lock);

    if (copy_from_user(&userlen, addrlen, sizeof(uint32_t))) return -EFAULT;

    if (kaddrlen > userlen) kaddrlen = userlen;

    if (copy_to_user(addr, &sk->local_addr, kaddrlen)) return -EFAULT;

    if (copy_to_user(addrlen, &kaddrlen, sizeof(uint32_t))) return -EFAULT;

    return EOK;
}

/* ---- sys_getpeername ---- */

int64_t sys_getpeername(int fd, sockaddr_un_t *addr, uint32_t *addrlen)
{
    socket_t *sk;
    uint32_t  kaddrlen;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

    if (!addr || !addrlen) return -EINVAL;

    spin_lock(&sk->lock);

    if (sk->state != SOCK_STATE_CONNECTED) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    kaddrlen = sk->peer_addr_len;
    spin_unlock(&sk->lock);

    uint32_t userlen;
    if (copy_from_user(&userlen, addrlen, sizeof(uint32_t))) return -EFAULT;

    if (kaddrlen > userlen) kaddrlen = userlen;

    if (copy_to_user(addr, &sk->peer_addr, kaddrlen)) return -EFAULT;

    if (copy_to_user(addrlen, &kaddrlen, sizeof(uint32_t))) return -EFAULT;

    return EOK;
}

/* ---- sys_setsockopt ---- */

int64_t sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen)
{
    socket_t *sk;
    int       ival;
    linger_t  linger;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

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
            /* Timeouts not implemented in this version */
            spin_unlock(&sk->lock);
            return -ENOPROTOOPT;

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

/* ---- sys_getsockopt ---- */

int64_t sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen)
{
    socket_t *sk;
    int       ival;
    uint32_t  koptlen;
    linger_t  linger;

    sk = socket_from_fd(fd);
    if (!sk) return -EBADF;

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
            sk->so_error = 0; /* Clear on read */
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
            /* Not implemented */
            spin_unlock(&sk->lock);
            return -ENOPROTOOPT;

        default :
            spin_unlock(&sk->lock);
            return -ENOPROTOOPT;
    }

    spin_unlock(&sk->lock);

    if (koptlen == sizeof(int)) {
        if (copy_to_user(optval, &ival, sizeof(int))) return -EFAULT;
    }

    if (copy_to_user(optlen, &koptlen, sizeof(uint32_t))) return -EFAULT;

    return EOK;
}

/* ---- sys_sendmmsg ---- */

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
        for (size_t j = 0; j < kmsg.msg_iovlen; j++) total_len += iov[j].iov_len;

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

        ret = do_sendmsg_kern(fd, sk, &kmsg, iov, kbuf, total_len, flags);

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

/* ---- sys_recvmmsg ---- */

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
        for (size_t j = 0; j < kmsg.msg_iovlen; j++) total_len += iov[j].iov_len;

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

/* ------------------------------------------------------------------ */
/*  Subsystem initialization                                            */
/* ------------------------------------------------------------------ */

void socket_init(void)
{
    memset(sock_blocked_tab, 0, sizeof(sock_blocked_tab));
    memset(sock_bound_tab, 0, sizeof(sock_bound_tab));

    vfs_callback_t cb = calloc(1, sizeof(struct vfs_callback));
    if (!cb) {
        plogk("socket: Failed to allocate VFS callback.\n");
        return;
    }

    cb->mount    = socket_stub_mount;
    cb->unmount  = socket_stub_unmount;
    cb->open     = socket_stub_open;
    cb->close    = socket_vfs_close;
    cb->read     = socket_vfs_read;
    cb->write    = socket_vfs_write;
    cb->readlink = socket_stub_readlink;
    cb->mkdir    = socket_stub_mk;
    cb->mkfile   = socket_stub_mk;
    cb->link     = socket_stub_mk;
    cb->symlink  = socket_stub_mk;
    cb->stat     = socket_stub_stat;
    cb->ioctl    = socket_stub_ioctl;
    cb->dup      = socket_stub_dup;
    cb->poll     = socket_vfs_poll;
    cb->free     = socket_vfs_free;
    cb->delete   = socket_stub_del;
    cb->rename   = socket_stub_rename;

    socket_fsid = vfs_regist(cb);
    if (socket_fsid < 0) {
        plogk("socket: Failed to register VFS callback.\n");
        free(cb);
        return;
    }

    plogk("socket: UNIX domain socket subsystem initialized (fsid=%d)\n", socket_fsid);
}