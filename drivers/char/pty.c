/*
 *
 *      pty.c
 *      Pseudoterminal driver
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/char/pty.h>
#include <drivers/char/tty_core.h>
#include <fs/virtual/devtmpfs.h>
#include <fs/virtual/tmpfs.h>
#include <kernel/errno.h>
#include <kernel/termios.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>

#ifndef CONFIG_UNIX98_PTYS
#    define CONFIG_UNIX98_PTYS 1
#endif
#ifndef CONFIG_UNIX98_PTY_MAX
#    define CONFIG_UNIX98_PTY_MAX 64
#endif

#define PTY_BUFFER_SIZE TTY_CORE_BUFFER_SIZE
#define PTMX_MAJOR      5
#define PTMX_MINOR      2
#define PTS_MAJOR       136
#define POLLIN          0x001
#define POLLPRI         0x002
#define POLLOUT         0x004
#define POLLERR         0x008
#define POLLHUP         0x010

typedef struct pty_pair {
        spinlock_t   lock;
        wait_queue_t master_wait;
        wait_queue_t master_space_wait;
        tty_core_t   slave_tty;
        uint8_t      master_buffer[PTY_BUFFER_SIZE];
        size_t       master_head;
        size_t       master_tail;
        size_t       master_count;
        unsigned int number;
        unsigned int references;
        unsigned int slave_opens;
        bool         master_open;
        bool         slave_locked;
        bool         packet_mode;
        uint8_t      packet_status;
        bool         master_exclusive;
        bool         slave_exclusive;
        vfs_node_t   slave_node;
        char         slave_path[32];
} pty_pair_t;

typedef enum pty_endpoint_kind {
    PTY_MASTER,
    PTY_SLAVE,
} pty_endpoint_kind_t;

typedef struct pty_endpoint {
        pty_pair_t         *pair;
        pty_endpoint_kind_t kind;
} pty_endpoint_t;

static spinlock_t  pty_id_lock;
static spinlock_t  pty_lifetime_lock;
static uint64_t    pty_ids[(CONFIG_UNIX98_PTY_MAX + 63) / 64];
static pty_pair_t *pty_pairs[CONFIG_UNIX98_PTY_MAX];

static void pty_get(pty_pair_t *pair)
{
    spin_lock(&pair->lock);
    pair->references++;
    spin_unlock(&pair->lock);
}

static void pty_put(pty_pair_t *pair)
{
    bool destroy = false;
    spin_lock(&pair->lock);
    if (pair->references) pair->references--;
    destroy = pair->references == 0;
    spin_unlock(&pair->lock);
    if (!destroy) return;

    spin_lock(&pty_id_lock);
    pty_ids[pair->number / 64] &= ~(1ULL << (pair->number % 64));
    spin_unlock(&pty_id_lock);
    if (pair->slave_node) vfs_close(pair->slave_node);
    free(pair);
}

static void pty_tty_retain(void *context)
{
    pty_get(context);
}

static void pty_tty_release(void *context)
{
    pty_put(context);
}

static int pty_allocate_number(void)
{
    spin_lock(&pty_id_lock);
    for (unsigned int number = 0; number < CONFIG_UNIX98_PTY_MAX; number++) {
        uint64_t mask = 1ULL << (number % 64);
        if (!(pty_ids[number / 64] & mask)) {
            pty_ids[number / 64] |= mask;
            spin_unlock(&pty_id_lock);
            return (int)number;
        }
    }
    spin_unlock(&pty_id_lock);
    return -ENOSPC;
}

static int pty_slave_emit(void *context, const uint8_t *data, size_t size, uint64_t flags)
{
    pty_pair_t *pair   = context;
    size_t      copied = 0;

    while (copied < size) {
        spin_lock(&pair->lock);
        while (pair->master_count == PTY_BUFFER_SIZE && pair->master_open) {
            if (flags & O_NONBLOCK) {
                spin_unlock(&pair->lock);
                return copied ? (int)copied : -EAGAIN;
            }
            wait_queue_prepare(&pair->master_space_wait);
            spin_unlock(&pair->lock);
            wait_queue_sleep();
            spin_lock(&pair->lock);
        }
        if (!pair->master_open) {
            spin_unlock(&pair->lock);
            return copied ? (int)copied : -EIO;
        }
        pair->master_buffer[pair->master_head] = data[copied++];
        pair->master_head                      = (pair->master_head + 1) % PTY_BUFFER_SIZE;
        pair->master_count++;
        spin_unlock(&pair->lock);
    }
    if (copied) wait_queue_wake_all(&pair->master_wait);
    return (int)copied;
}

static void pty_slave_event(void *context, uint8_t event)
{
    pty_pair_t *pair = context;
    spin_lock(&pair->lock);
    if (event & TIOCPKT_FLUSHWRITE) pair->master_head = pair->master_tail = pair->master_count = 0;
    if (pair->packet_mode) {
        if (event & TIOCPKT_STOP) pair->packet_status &= ~TIOCPKT_START;
        if (event & TIOCPKT_START) pair->packet_status &= ~TIOCPKT_STOP;
        if (event & TIOCPKT_DOSTOP) pair->packet_status &= ~TIOCPKT_NOSTOP;
        if (event & TIOCPKT_NOSTOP) pair->packet_status &= ~TIOCPKT_DOSTOP;
        pair->packet_status |= event;
    }
    bool notify = pair->packet_mode && event;
    spin_unlock(&pair->lock);
    if (notify) wait_queue_wake_all(&pair->master_wait);
    if (event & TIOCPKT_FLUSHWRITE) wait_queue_wake_all(&pair->master_space_wait);
}

static int64_t pty_master_read(pty_pair_t *pair, uint64_t flags, void *buffer, size_t size)
{
    uint8_t *output = buffer;
    size_t   copied = 0;

    if (!size) return 0;
    spin_lock(&pair->lock);
    for (;;) {
        if (pair->packet_mode && pair->packet_status) {
            output[0]           = pair->packet_status;
            pair->packet_status = 0;
            spin_unlock(&pair->lock);
            return 1;
        }
        if (pair->master_count) break;
        if (!pair->slave_opens) {
            spin_unlock(&pair->lock);
            return -EIO;
        }
        if (flags & O_NONBLOCK) {
            spin_unlock(&pair->lock);
            return -EAGAIN;
        }
        wait_queue_prepare(&pair->master_wait);
        spin_unlock(&pair->lock);
        wait_queue_sleep();
        spin_lock(&pair->lock);
    }

    if (pair->packet_mode) output[copied++] = TIOCPKT_DATA;
    while (copied < size && pair->master_count) {
        output[copied++]  = pair->master_buffer[pair->master_tail];
        pair->master_tail = (pair->master_tail + 1) % PTY_BUFFER_SIZE;
        pair->master_count--;
    }
    spin_unlock(&pair->lock);
    wait_queue_wake_all(&pair->master_space_wait);
    return (int64_t)copied;
}

static int     pty_open(vfs_node_t node, uint64_t flags, void **private_data);
static void    pty_release(vfs_node_t node, void *private_data);
static int64_t pty_read(void *context, void *private_data, uint64_t flags, void *address, size_t offset, size_t size);
static int64_t pty_write(void *context, void *private_data, uint64_t flags, const void *address, size_t offset, size_t size);
static int     pty_poll(void *context, void *private_data, uint64_t flags, size_t events);
static int     pty_ioctl(void *context, void *private_data, uint64_t flags, size_t request, void *argument);

static const tmpfs_device_ops_t pty_slave_operations = {
    .open       = pty_open,
    .release    = pty_release,
    .file_read  = pty_read,
    .file_write = pty_write,
    .file_poll  = pty_poll,
    .file_ioctl = pty_ioctl,
};

static int pty_create_slave(pty_pair_t *pair)
{
    tmpfs_device_ops_t operations = pty_slave_operations;
    operations.ctx                = pair;
    int result = devtmpfs_register_char_device(pair->slave_path, MKDEV(PTS_MAJOR, pair->number), MKDEV(PTS_MAJOR, pair->number),
                                               file_pts | file_stream, &operations);
    if (result) return result;

    vfs_node_t node = vfs_open(pair->slave_path);
    if (!node) {
        devtmpfs_unregister_char_device(pair->slave_path);
        return -ENOENT;
    }
    process_t *current = process_current();
    node->mode         = 0620;
    node->owner        = current ? current->uid : 0;
    node->group        = current ? current->gid : 0;
    pair->slave_node   = node;
    return 0;
}

static int pty_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    if ((flags & O_PATH) || (flags & O_ACCMODE) == O_ACCMODE) return -EINVAL;
    pty_endpoint_t *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) return -ENOMEM;

    if (node->type & file_ptmx) {
        int number = pty_allocate_number();
        if (number < 0) {
            free(endpoint);
            return number;
        }
        pty_pair_t *pair = calloc(1, sizeof(*pair));
        if (!pair) {
            spin_lock(&pty_id_lock);
            pty_ids[number / 64] &= ~(1ULL << (number % 64));
            spin_unlock(&pty_id_lock);
            free(endpoint);
            return -ENOMEM;
        }
        pair->number       = (unsigned int)number;
        pair->references   = 1;
        pair->master_open  = true;
        pair->slave_locked = true;
        wait_queue_init(&pair->master_wait);
        wait_queue_init(&pair->master_space_wait);
        static const tty_core_ops_t core_operations
            = {.emit = pty_slave_emit, .event = pty_slave_event, .retain = pty_tty_retain, .release = pty_tty_release};
        tty_core_init(&pair->slave_tty, &core_operations, pair);
        snprintf(pair->slave_path, sizeof(pair->slave_path), "/dev/pts/%u", pair->number);
        spin_lock(&pty_lifetime_lock);
        pty_pairs[pair->number] = pair;
        spin_unlock(&pty_lifetime_lock);
        int result = pty_create_slave(pair);
        if (result) {
            spin_lock(&pty_lifetime_lock);
            pty_pairs[pair->number] = NULL;
            spin_unlock(&pty_lifetime_lock);
            pty_put(pair);
            free(endpoint);
            return result;
        }
        endpoint->pair = pair;
        endpoint->kind = PTY_MASTER;
    } else if (node->type & file_pts) {
        spin_lock(&pty_lifetime_lock);
        unsigned int number = MINOR(node->rdev);
        pty_pair_t  *pair   = ((tmpfs_file_t *)node->handle)->device.ctx;
        if (number >= CONFIG_UNIX98_PTY_MAX || pty_pairs[number] != pair) {
            spin_unlock(&pty_lifetime_lock);
            free(endpoint);
            return -EIO;
        }
        spin_lock(&pair->lock);
        if (pair->slave_locked) {
            spin_unlock(&pair->lock);
            spin_unlock(&pty_lifetime_lock);
            free(endpoint);
            return -EIO;
        }
        if (!pair->master_open) {
            spin_unlock(&pair->lock);
            spin_unlock(&pty_lifetime_lock);
            free(endpoint);
            return -EIO;
        }
        process_t *current = process_current();
        if (pair->slave_exclusive && (!current || current->uid != 0)) {
            spin_unlock(&pair->lock);
            spin_unlock(&pty_lifetime_lock);
            free(endpoint);
            return -EBUSY;
        }
        pair->references++;
        pair->slave_opens++;
        spin_unlock(&pair->lock);
        spin_unlock(&pty_lifetime_lock);
        endpoint->pair = pair;
        endpoint->kind = PTY_SLAVE;
        tty_core_auto_acquire(&pair->slave_tty, flags);
    } else {
        free(endpoint);
        return -ENODEV;
    }
    *private_data = endpoint;
    return 0;
}

static void pty_release(vfs_node_t node, void *private_data)
{
    (void)node;
    pty_endpoint_t *endpoint = private_data;
    if (!endpoint) return;
    pty_pair_t *pair = endpoint->pair;
    if (endpoint->kind == PTY_MASTER) {
        spin_lock(&pty_lifetime_lock);
        pty_pairs[pair->number] = NULL;
        spin_lock(&pair->lock);
        pair->master_open = false;
        spin_unlock(&pair->lock);
        spin_unlock(&pty_lifetime_lock);
        devtmpfs_unregister_char_device(pair->slave_path);
        wait_queue_wake_all(&pair->master_space_wait);
        tty_core_hangup(&pair->slave_tty);
    } else {
        spin_lock(&pair->lock);
        if (pair->slave_opens) pair->slave_opens--;
        spin_unlock(&pair->lock);
        wait_queue_wake_all(&pair->master_wait);
    }
    free(endpoint);
    pty_put(pair);
}

static int64_t pty_read(void *context, void *private_data, uint64_t flags, void *address, size_t offset, size_t size)
{
    (void)context;
    (void)offset;
    pty_endpoint_t *endpoint = private_data;
    if (!endpoint) return -EIO;
    if (endpoint->kind == PTY_MASTER) return pty_master_read(endpoint->pair, flags, address, size);
    return tty_core_read(&endpoint->pair->slave_tty, address, size, flags);
}

static int64_t pty_write(void *context, void *private_data, uint64_t flags, const void *address, size_t offset, size_t size)
{
    (void)context;
    (void)offset;
    pty_endpoint_t *endpoint = private_data;
    if (!endpoint) return -EIO;
    if (endpoint->kind == PTY_SLAVE) return tty_core_write(&endpoint->pair->slave_tty, address, size, flags);

    spin_lock(&endpoint->pair->lock);
    bool slave_open = endpoint->pair->slave_opens != 0;
    spin_unlock(&endpoint->pair->lock);
    if (!slave_open) return -EIO;
    return tty_core_receive(&endpoint->pair->slave_tty, address, size, flags);
}

static int pty_poll(void *context, void *private_data, uint64_t flags, size_t events)
{
    (void)context;
    (void)flags;
    pty_endpoint_t *endpoint = private_data;
    if (!endpoint) return POLLERR;
    pty_pair_t *pair = endpoint->pair;
    if (endpoint->kind == PTY_SLAVE) {
        int result = tty_core_poll(&pair->slave_tty, events);
        spin_lock(&pair->lock);
        if (!pair->master_open || pair->master_count == PTY_BUFFER_SIZE) result &= ~POLLOUT;
        if (!pair->master_open) result |= POLLHUP;
        spin_unlock(&pair->lock);
        return result;
    }

    int result = 0;
    spin_lock(&pair->slave_tty.lock);
    bool slave_room = pair->slave_tty.input_count < TTY_CORE_BUFFER_SIZE && !pair->slave_tty.hung_up;
    spin_unlock(&pair->slave_tty.lock);
    spin_lock(&pair->lock);
    if ((events & POLLIN) && (pair->master_count || (pair->packet_mode && pair->packet_status))) result |= POLLIN;
    if ((events & POLLPRI) && pair->packet_mode && pair->packet_status) result |= POLLPRI;
    if ((events & POLLOUT) && pair->slave_opens && slave_room) result |= POLLOUT;
    if (!pair->slave_opens) result |= POLLHUP;
    spin_unlock(&pair->lock);
    return result;
}

static unsigned int pty_linux_dev(unsigned int major, unsigned int minor)
{
    return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
}

static int pty_exclusive_ioctl(pty_pair_t *pair, bool master, size_t request, void *argument)
{
    int value;

    spin_lock(&pair->lock);
    bool *exclusive = master ? &pair->master_exclusive : &pair->slave_exclusive;
    if (request == TIOCEXCL)
        *exclusive = true;
    else if (request == TIOCNXCL)
        *exclusive = false;
    value = *exclusive;
    spin_unlock(&pair->lock);
    if (request == TIOCGEXCL) return copy_to_user(argument, &value, sizeof(value)) ? -EFAULT : 0;
    return 0;
}

static int pty_signal_slave(pty_pair_t *pair, int signal)
{
    if (signal != SIGINT && signal != SIGQUIT && signal != SIGTSTP) return -EINVAL;
    spin_lock(&pair->slave_tty.lock);
    int64_t pgid = pair->slave_tty.foreground_pgid;
    spin_unlock(&pair->slave_tty.lock);
    return pgid > 0 ? signal_send_pgrp(pgid, signal) : 0;
}

static int pty_master_ioctl(pty_pair_t *pair, uint64_t flags, size_t request, void *argument)
{
    int          value;
    unsigned int unsigned_value;

    switch (request) {
        case TIOCGPTN :
            unsigned_value = pair->number;
            return copy_to_user(argument, &unsigned_value, sizeof(unsigned_value)) ? -EFAULT : 0;
        case TIOCSPTLCK :
            if (copy_from_user(&value, argument, sizeof(value))) return -EFAULT;
            spin_lock(&pair->lock);
            pair->slave_locked = value != 0;
            spin_unlock(&pair->lock);
            return 0;
        case TIOCGPTLCK :
            spin_lock(&pair->lock);
            value = pair->slave_locked;
            spin_unlock(&pair->lock);
            return copy_to_user(argument, &value, sizeof(value)) ? -EFAULT : 0;
        case TIOCPKT :
            if (copy_from_user(&value, argument, sizeof(value))) return -EFAULT;
            spin_lock(&pair->lock);
            bool enabled        = value != 0;
            pair->packet_status = 0;
            pair->packet_mode   = enabled;
            spin_unlock(&pair->lock);
            return 0;
        case TIOCGPKT :
            spin_lock(&pair->lock);
            value = pair->packet_mode;
            spin_unlock(&pair->lock);
            return copy_to_user(argument, &value, sizeof(value)) ? -EFAULT : 0;
        case TIOCGDEV :
            unsigned_value = pty_linux_dev(PTS_MAJOR, pair->number);
            return copy_to_user(argument, &unsigned_value, sizeof(unsigned_value)) ? -EFAULT : 0;
        case FIONREAD :
            spin_lock(&pair->lock);
            value = (int)pair->master_count;
            if (pair->packet_mode && (pair->master_count || pair->packet_status)) value++;
            spin_unlock(&pair->lock);
            return copy_to_user(argument, &value, sizeof(value)) ? -EFAULT : 0;
        case TIOCGPTPEER : {
            uint64_t allowed    = O_ACCMODE | O_NOCTTY | O_NONBLOCK | O_CLOEXEC;
            uint64_t peer_flags = (uint64_t)(uintptr_t)argument;
            if ((peer_flags & ~allowed) || (peer_flags & O_ACCMODE) == O_ACCMODE) return -EINVAL;
            if (!pair->slave_node) return -EIO;
            uint32_t access        = (peer_flags & O_ACCMODE) == O_WRONLY ? VFS_ACCESS_W :
                                     (peer_flags & O_ACCMODE) == O_RDWR   ? VFS_ACCESS_R | VFS_ACCESS_W :
                                                                            VFS_ACCESS_R;
            int      access_result = vfs_access_check(pair->slave_node, access);
            if (access_result) return access_result;
            vfs_node_t slave = vfs_node_retain(pair->slave_node);
            if (!slave) return -EIO;
            process_t *current = process_current();
            int        fd      = current ? process_fd_install(current, slave, peer_flags) : -ESRCH;
            if (fd < 0) vfs_close(slave);
            return fd;
        }
        case TIOCEXCL :
        case TIOCNXCL :
        case TIOCGEXCL :
            return pty_exclusive_ioctl(pair, true, request, argument);
        case TIOCSIG :
            return pty_signal_slave(pair, (int)(uintptr_t)argument);
        case TIOCVHANGUP : {
            process_t *current = process_current();
            if (!current || current->uid != 0) return -EPERM;
            tty_core_hangup(&pair->slave_tty);
            return 0;
        }
        case TCGETS :
        case TCSETS :
        case TCSETSW :
        case TCSETSF :
        case TIOCGWINSZ :
        case TIOCSWINSZ :
        case TIOCOUTQ :
        case TCFLSH :
        case TCXONC :
        case TIOCGETD :
        case TIOCSETD :
            return tty_core_ioctl(&pair->slave_tty, flags, request, argument);
        default :
            return -ENOTTY;
    }
}

static int pty_ioctl(void *context, void *private_data, uint64_t flags, size_t request, void *argument)
{
    (void)context;
    pty_endpoint_t *endpoint = private_data;
    if (!endpoint) return -EIO;
    if (endpoint->kind == PTY_MASTER) return pty_master_ioctl(endpoint->pair, flags, request, argument);

    switch (request) {
        case TIOCGPTN :
        case TIOCSPTLCK :
        case TIOCGPTLCK :
        case TIOCPKT :
        case TIOCGPKT :
        case TIOCGPTPEER :
        case TIOCSIG :
            return -ENOTTY;
        case TIOCEXCL :
        case TIOCNXCL :
        case TIOCGEXCL :
            return pty_exclusive_ioctl(endpoint->pair, false, request, argument);
        case TIOCVHANGUP : {
            process_t *current = process_current();
            if (!current || current->uid != 0) return -EPERM;
            tty_core_hangup(&endpoint->pair->slave_tty);
            return 0;
        }
        case TIOCGDEV : {
            unsigned int device = pty_linux_dev(PTS_MAJOR, endpoint->pair->number);
            return copy_to_user(argument, &device, sizeof(device)) ? -EFAULT : 0;
        }
        default :
            return tty_core_ioctl(&endpoint->pair->slave_tty, flags, request, argument);
    }
}

void pty_init(void)
{
#if !CONFIG_UNIX98_PTYS
    return;
#endif
    static bool initialized;
    if (initialized) return;
    initialized                                     = true;
    static const tmpfs_device_ops_t ptmx_operations = {
        .open       = pty_open,
        .release    = pty_release,
        .file_read  = pty_read,
        .file_write = pty_write,
        .file_poll  = pty_poll,
        .file_ioctl = pty_ioctl,
    };
    vfs_mkdir("/dev/pts");
    if (!devtmpfs_register_char_device("/dev/ptmx", MKDEV(PTMX_MAJOR, PTMX_MINOR), MKDEV(PTMX_MAJOR, PTMX_MINOR), file_ptmx | file_stream,
                                       &ptmx_operations)) {
        vfs_node_t node = vfs_open("/dev/ptmx");
        if (node) {
            node->mode = 0666;
            vfs_close(node);
        }
    }
}
