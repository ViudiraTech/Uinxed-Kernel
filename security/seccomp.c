/*
 *
 *      seccomp.c
 *      Secure-computing syscall, filter stack, enforcement and user notification.
 *      The implementation follows Linux ABI semantics while using Uinxed-native
 *      task, VFS and wait-queue primitives.
 *
 *      2026/8/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/list/intrusive_list.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/ptrace.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <security/seccomp.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>
#include <syscall/poll.h>
#include <syscall/syscall.h>
#include <syscall/syscall_table.h>

#define SECCOMP_MAX_ERRNO 4095U
#define SECCOMP_SIGSYS_CODE 1

typedef enum {
    SECCOMP_NOTIFY_INIT,
    SECCOMP_NOTIFY_SENT,
    SECCOMP_NOTIFY_REPLIED,
} seccomp_notify_state_t;

typedef struct seccomp_listener seccomp_listener_t;

typedef struct seccomp_knotif {
        struct seccomp_knotif *next;
        task_t                *task;
        struct seccomp_notif   message;
        struct seccomp_notif_resp response;
        seccomp_notify_state_t state;
        uint32_t               active_ops;
        wait_queue_t           wait;
} seccomp_knotif_t;

struct seccomp_listener {
        uint32_t          refcount;
        spinlock_t        lock;
        wait_queue_t      recv_wait;
        vfs_poll_source_t poll_source;
        seccomp_knotif_t *head;
        seccomp_knotif_t *tail;
        uint64_t          filter_flags;
        uint64_t          fd_flags;
        bool              detached;
        bool              orphaned;
};

struct seccomp_filter {
        uint32_t               refcount;
        uint32_t               length;
        uint32_t               total_insns;
        uint64_t               flags;
        struct sock_filter    *program;
        struct seccomp_filter *prev;
        seccomp_listener_t    *listener;
};

static int      seccomp_fsid = -1;
static uint64_t seccomp_next_notification_id = 1;

#define SECCOMP_IOCTL_NOTIF_ID_VALID_WRONG_DIR _IOR(SECCOMP_IOC_MAGIC, 2, uint64_t)

static void seccomp_listener_get(seccomp_listener_t *listener)
{
    if (listener) __atomic_add_fetch(&listener->refcount, 1, __ATOMIC_RELAXED);
}

static bool seccomp_listener_has_state(seccomp_listener_t *listener, seccomp_notify_state_t state)
{
    for (seccomp_knotif_t *item = listener->head; item; item = item->next)
        if (item->state == state) return true;
    return false;
}

static void seccomp_listener_wake_all_locked(seccomp_listener_t *listener)
{
    wait_queue_wake_all(&listener->recv_wait);
    for (seccomp_knotif_t *item = listener->head; item; item = item->next) wait_queue_wake_all(&item->wait);
}

static void seccomp_listener_wake_receiver_locked(seccomp_listener_t *listener)
{
    if (listener->fd_flags & SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP)
        wait_queue_wake_one_sync(&listener->recv_wait);
    else
        wait_queue_wake_one(&listener->recv_wait);
}

static void seccomp_listener_wake_target_locked(seccomp_listener_t *listener, seccomp_knotif_t *item)
{
    if (listener->fd_flags & SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP)
        wait_queue_wake_one_sync(&item->wait);
    else
        wait_queue_wake_one(&item->wait);
}

static void seccomp_listener_detach(seccomp_listener_t *listener)
{
    if (!listener) return;
    spin_lock(&listener->lock);
    if (!listener->detached) {
        listener->detached = true;
        seccomp_listener_wake_all_locked(listener);
    }
    spin_unlock(&listener->lock);
    vfs_poll_source_close(&listener->poll_source, POLLHUP | POLLERR);
}

static void seccomp_listener_orphan(seccomp_listener_t *listener)
{
    if (!listener) return;
    spin_lock(&listener->lock);
    listener->orphaned = true;
    seccomp_listener_wake_all_locked(listener);
    spin_unlock(&listener->lock);
    vfs_poll_source_close(&listener->poll_source, POLLHUP);
}

static void seccomp_listener_put(seccomp_listener_t *listener)
{
    if (!listener || __atomic_sub_fetch(&listener->refcount, 1, __ATOMIC_ACQ_REL)) return;
    seccomp_listener_detach(listener);
    free(listener);
}

static seccomp_listener_t *seccomp_listener_alloc(uint64_t flags)
{
    seccomp_listener_t *listener = calloc(1, sizeof(*listener));
    if (!listener) return NULL;
    listener->refcount   = 1; /* owning filter */
    listener->lock.lock = 0;
    listener->lock.rflags = 0;
    listener->filter_flags = flags & SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV;
    wait_queue_init(&listener->recv_wait);
    vfs_poll_source_init(&listener->poll_source);
    return listener;
}

static void seccomp_filter_get(struct seccomp_filter *filter)
{
    if (filter) __atomic_add_fetch(&filter->refcount, 1, __ATOMIC_RELAXED);
}

static void seccomp_filter_put(struct seccomp_filter *filter)
{
    while (filter && __atomic_sub_fetch(&filter->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        struct seccomp_filter *previous = filter->prev;
        seccomp_listener_orphan(filter->listener);
        seccomp_listener_put(filter->listener);
        free(filter->program);
        free(filter);
        filter = previous;
    }
}

void seccomp_task_inherit(task_t *child, const task_t *parent)
{
    if (!child || !parent) return;
    child->seccomp_mode   = __atomic_load_n(&parent->seccomp_mode, __ATOMIC_ACQUIRE);
    child->no_new_privs   = __atomic_load_n(&parent->no_new_privs, __ATOMIC_ACQUIRE);
    child->seccomp_filter = __atomic_load_n(&parent->seccomp_filter, __ATOMIC_ACQUIRE);
    seccomp_filter_get(child->seccomp_filter);
}

void seccomp_task_release(task_t *task)
{
    if (!task) return;
    struct seccomp_filter *filter = task->seccomp_filter;
    task->seccomp_filter = NULL;
    task->seccomp_mode   = SECCOMP_MODE_DISABLED;
    seccomp_filter_put(filter);
}

void seccomp_task_get_status(const task_t *task, bool *no_new_privs, uint8_t *mode, uint32_t *filter_count)
{
    if (no_new_privs) *no_new_privs = task ? __atomic_load_n(&task->no_new_privs, __ATOMIC_ACQUIRE) : false;
    if (mode) *mode = task ? __atomic_load_n(&task->seccomp_mode, __ATOMIC_ACQUIRE) : SECCOMP_MODE_DISABLED;
    if (!filter_count) return;
    *filter_count = 0;
    if (!task || !task->process) return;
    process_t *proc = task->process;
    spin_lock(&proc->seccomp_lock);
    for (struct seccomp_filter *filter = task->seccomp_filter; filter; filter = filter->prev) (*filter_count)++;
    spin_unlock(&proc->seccomp_lock);
}

static bool seccomp_signal_pending(bool fatal_only)
{
    task_t    *task = current_task();
    process_t *proc = task ? task->process : NULL;
    if (!task || !proc) return false;
    spin_lock(&proc->signal.lock);
    bool pending;
    if (fatal_only) {
        pending = proc->signal.group_exit || sigismember(&proc->signal.pending, SIGKILL) || sigismember(&task->signal_pending, SIGKILL);
    } else {
        pending = signal_has_interrupting_pending(&proc->signal);
    }
    spin_unlock(&proc->signal.lock);
    return pending;
}

static void seccomp_listener_remove_locked(seccomp_listener_t *listener, seccomp_knotif_t *target)
{
    seccomp_knotif_t **link = &listener->head;
    while (*link && *link != target) link = &(*link)->next;
    if (*link) *link = target->next;
    if (listener->tail == target) {
        listener->tail = NULL;
        for (seccomp_knotif_t *item = listener->head; item; item = item->next) listener->tail = item;
    }
    target->next = NULL;
}

static seccomp_knotif_t *seccomp_listener_find_locked(seccomp_listener_t *listener, uint64_t id, seccomp_notify_state_t state)
{
    for (seccomp_knotif_t *item = listener->head; item; item = item->next)
        if (item->message.id == id && item->state == state) return item;
    return NULL;
}

static seccomp_knotif_t *seccomp_listener_find_any_locked(seccomp_listener_t *listener, uint64_t id)
{
    for (seccomp_knotif_t *item = listener->head; item; item = item->next)
        if (item->message.id == id) return item;
    return NULL;
}

/* Returns true only when the listener authorized the original syscall. */
static bool seccomp_notify_wait(seccomp_listener_t *listener, const struct seccomp_data *data, int64_t *result)
{
    if (!listener) {
        *result = -ENOSYS;
        return false;
    }

    seccomp_knotif_t notification;
    memset(&notification, 0, sizeof(notification));
    notification.task        = current_task();
    notification.message.id  = __atomic_add_fetch(&seccomp_next_notification_id, 1, __ATOMIC_RELAXED);
    notification.message.pid = notification.task ? (uint32_t)notification.task->pid : 0;
    notification.message.data = *data;
    notification.state = SECCOMP_NOTIFY_INIT;
    wait_queue_init(&notification.wait);

    spin_lock(&listener->lock);
    if (listener->detached) {
        spin_unlock(&listener->lock);
        *result = -ENOSYS;
        return false;
    }
    if (listener->tail)
        listener->tail->next = &notification;
    else
        listener->head = &notification;
    listener->tail = &notification;
    seccomp_listener_wake_receiver_locked(listener);
    spin_unlock(&listener->lock);
    vfs_poll_source_notify(&listener->poll_source, POLLIN);

    for (;;) {
        spin_lock(&listener->lock);
        if (notification.active_ops) {
            wait_queue_prepare(&notification.wait);
            spin_unlock(&listener->lock);
            wait_queue_sleep();
            continue;
        }
        if (notification.state == SECCOMP_NOTIFY_REPLIED) {
            struct seccomp_notif_resp response = notification.response;
            seccomp_listener_remove_locked(listener, &notification);
            spin_unlock(&listener->lock);
            if (response.flags & SECCOMP_USER_NOTIF_FLAG_CONTINUE) return true;
            *result = response.error ? response.error : response.val;
            return false;
        }
        if (listener->detached) {
            seccomp_listener_remove_locked(listener, &notification);
            spin_unlock(&listener->lock);
            *result = -ENOSYS;
            return false;
        }

        bool fatal_only = notification.state == SECCOMP_NOTIFY_SENT && (listener->filter_flags & SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV);
        if (seccomp_signal_pending(fatal_only)) {
            seccomp_listener_remove_locked(listener, &notification);
            spin_unlock(&listener->lock);
            vfs_poll_source_notify(&listener->poll_source, POLLIN | POLLOUT);
            *result = -ERESTARTSYS;
            return false;
        }
        wait_queue_prepare(&notification.wait);
        spin_unlock(&listener->lock);
        if (seccomp_signal_pending(fatal_only)) {
            wait_queue_cancel(&notification.wait);
            continue;
        }
        wait_queue_sleep();
    }
}

static bool seccomp_user_buffer_zero(const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    for (size_t i = 0; i < length; i++)
        if (bytes[i]) return false;
    return true;
}

static int seccomp_listener_recv(seccomp_listener_t *listener, uint64_t file_flags, void *user_buffer)
{
    struct seccomp_notif user_value;
    if (!user_buffer || copy_from_user(&user_value, user_buffer, sizeof(user_value))) return -EFAULT;
    if (!seccomp_user_buffer_zero(&user_value, sizeof(user_value))) return -EINVAL;

    for (;;) {
        spin_lock(&listener->lock);
        seccomp_knotif_t *item = NULL;
        for (item = listener->head; item; item = item->next)
            if (item->state == SECCOMP_NOTIFY_INIT) break;
        if (item) {
            item->state = SECCOMP_NOTIFY_SENT;
            struct seccomp_notif message = item->message;
            spin_unlock(&listener->lock);
            if (copy_to_user(user_buffer, &message, sizeof(message))) {
                spin_lock(&listener->lock);
                item = seccomp_listener_find_any_locked(listener, message.id);
                if (item && item->state == SECCOMP_NOTIFY_SENT) item->state = SECCOMP_NOTIFY_INIT;
                spin_unlock(&listener->lock);
                vfs_poll_source_notify(&listener->poll_source, POLLIN);
                return -EFAULT;
            }
            spin_lock(&listener->lock);
            item = seccomp_listener_find_any_locked(listener, message.id);
            if (item) seccomp_listener_wake_target_locked(listener, item);
            spin_unlock(&listener->lock);
            vfs_poll_source_notify(&listener->poll_source, POLLOUT);
            return EOK;
        }
        if (listener->detached || listener->orphaned) {
            spin_unlock(&listener->lock);
            return -ENOENT;
        }
        if (file_flags & O_NONBLOCK) {
            spin_unlock(&listener->lock);
            return -EAGAIN;
        }
        if (seccomp_signal_pending(false)) {
            spin_unlock(&listener->lock);
            return -ERESTARTSYS;
        }
        wait_queue_prepare(&listener->recv_wait);
        spin_unlock(&listener->lock);
        if (seccomp_signal_pending(false)) {
            wait_queue_cancel(&listener->recv_wait);
            return -ERESTARTSYS;
        }
        wait_queue_sleep();
    }
}

static bool seccomp_response_valid(const struct seccomp_notif_resp *response)
{
    if (response->flags & ~SECCOMP_USER_NOTIF_FLAG_CONTINUE) return false;
    if (response->flags & SECCOMP_USER_NOTIF_FLAG_CONTINUE) return response->error == 0 && response->val == 0;
    return true;
}

static int seccomp_listener_send(seccomp_listener_t *listener, void *user_buffer)
{
    struct seccomp_notif_resp response;
    if (!user_buffer || copy_from_user(&response, user_buffer, sizeof(response))) return -EFAULT;
    if (!seccomp_response_valid(&response)) return -EINVAL;

    spin_lock(&listener->lock);
    if (listener->detached) {
        spin_unlock(&listener->lock);
        return -ENOENT;
    }
    seccomp_knotif_t *item = seccomp_listener_find_any_locked(listener, response.id);
    if (!item) {
        spin_unlock(&listener->lock);
        return -ENOENT;
    }
    if (item->state != SECCOMP_NOTIFY_SENT) {
        spin_unlock(&listener->lock);
        return -EINPROGRESS;
    }
    item->response = response;
    item->state    = SECCOMP_NOTIFY_REPLIED;
    seccomp_listener_wake_target_locked(listener, item);
    spin_unlock(&listener->lock);
    vfs_poll_source_notify(&listener->poll_source, POLLIN | POLLOUT);
    return EOK;
}

static int seccomp_listener_id_valid(seccomp_listener_t *listener, void *user_buffer)
{
    uint64_t id;
    if (!user_buffer || copy_from_user(&id, user_buffer, sizeof(id))) return -EFAULT;
    spin_lock(&listener->lock);
    bool valid = !listener->detached && seccomp_listener_find_locked(listener, id, SECCOMP_NOTIFY_SENT) != NULL;
    spin_unlock(&listener->lock);
    return valid ? EOK : -ENOENT;
}

static int seccomp_listener_copy_addfd(struct seccomp_notif_addfd *request, void *user_buffer, size_t user_size)
{
    if (user_size < sizeof(*request) || user_size >= 4096U) return -EINVAL;
    if (!user_buffer) return -EFAULT;
    if (copy_from_user(request, user_buffer, sizeof(*request))) return -EFAULT;
    size_t offset = sizeof(*request);
    while (offset < user_size) {
        uint8_t bytes[32];
        size_t chunk = user_size - offset;
        if (chunk > sizeof(bytes)) chunk = sizeof(bytes);
        if (copy_from_user(bytes, (uint8_t *)user_buffer + offset, chunk)) return -EFAULT;
        if (!seccomp_user_buffer_zero(bytes, chunk)) return -E2BIG;
        offset += chunk;
    }
    return EOK;
}

static int seccomp_listener_addfd(seccomp_listener_t *listener, void *user_buffer, size_t user_size)
{
    struct seccomp_notif_addfd request;
    int copy_status = seccomp_listener_copy_addfd(&request, user_buffer, user_size);
    if (copy_status) return copy_status;
    if (request.flags & ~(SECCOMP_ADDFD_FLAG_SETFD | SECCOMP_ADDFD_FLAG_SEND)) return -EINVAL;
    if (request.newfd_flags & ~O_CLOEXEC) return -EINVAL;
    if (!(request.flags & SECCOMP_ADDFD_FLAG_SETFD) && request.newfd) return -EINVAL;

    process_t *supervisor = process_current();
    process_file_t *source = process_fd_get_for_transfer(supervisor, (int)request.srcfd);
    if (!source) return -EBADF;

    spin_lock(&listener->lock);
    if (listener->detached) {
        spin_unlock(&listener->lock);
        process_file_put_transfer(source);
        return -ENOENT;
    }
    seccomp_knotif_t *item = seccomp_listener_find_any_locked(listener, request.id);
    if (!item) {
        spin_unlock(&listener->lock);
        process_file_put_transfer(source);
        return -ENOENT;
    }
    if (item->state != SECCOMP_NOTIFY_SENT) {
        spin_unlock(&listener->lock);
        process_file_put_transfer(source);
        return -EINPROGRESS;
    }
    if (!item->task || !item->task->process) {
        spin_unlock(&listener->lock);
        process_file_put_transfer(source);
        return -ESRCH;
    }
    if ((request.flags & SECCOMP_ADDFD_FLAG_SEND) && item->active_ops) {
        spin_unlock(&listener->lock);
        process_file_put_transfer(source);
        return -EBUSY;
    }

    process_t *target = item->task->process;
    item->active_ops++;
    if (request.flags & SECCOMP_ADDFD_FLAG_SEND) item->state = SECCOMP_NOTIFY_REPLIED;
    spin_unlock(&listener->lock);

    int newfd;
    if (request.flags & SECCOMP_ADDFD_FLAG_SETFD)
        newfd = process_fd_install_file_at(target, source, (int)request.newfd, request.newfd_flags, true);
    else
        newfd = process_fd_install_file(target, source, request.newfd_flags);
    process_file_put_transfer(source);

    spin_lock(&listener->lock);
    item->active_ops--;
    if (request.flags & SECCOMP_ADDFD_FLAG_SEND) {
        if (newfd < 0) {
            item->state = SECCOMP_NOTIFY_SENT;
        } else {
            memset(&item->response, 0, sizeof(item->response));
            item->response.id  = request.id;
            item->response.val = newfd;
        }
    }
    seccomp_listener_wake_target_locked(listener, item);
    spin_unlock(&listener->lock);
    vfs_poll_source_notify(&listener->poll_source, POLLIN | POLLOUT);
    return newfd;
}

static int seccomp_listener_file_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    (void)flags;
    if (!node || !node->handle) return -ENODEV;
    *private_data = node->handle;
    return EOK;
}

static void seccomp_listener_file_release(vfs_node_t node, void *private_data)
{
    (void)node;
    (void)private_data;
}

static void seccomp_listener_descriptor_close(vfs_node_t node, void *private_data)
{
    (void)node;
    seccomp_listener_detach(private_data);
}

static int seccomp_listener_file_ioctl(vfs_node_t node, void *private_data, uint64_t flags, size_t request, void *argument)
{
    (void)node;
    seccomp_listener_t *listener = private_data;
    if (!listener) return -ENODEV;
    switch (request) {
        case SECCOMP_IOCTL_NOTIF_RECV : return seccomp_listener_recv(listener, flags, argument);
        case SECCOMP_IOCTL_NOTIF_SEND : return seccomp_listener_send(listener, argument);
        case SECCOMP_IOCTL_NOTIF_ID_VALID_WRONG_DIR :
        case SECCOMP_IOCTL_NOTIF_ID_VALID : return seccomp_listener_id_valid(listener, argument);
        case SECCOMP_IOCTL_NOTIF_ADDFD : return seccomp_listener_addfd(listener, argument, sizeof(struct seccomp_notif_addfd));
        case SECCOMP_IOCTL_NOTIF_SET_FLAGS : {
            uint64_t fd_flags = (uintptr_t)argument;
            if (fd_flags & ~SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP) return -EINVAL;
            spin_lock(&listener->lock);
            if (listener->detached) {
                spin_unlock(&listener->lock);
                return -ENOENT;
            }
            listener->fd_flags = fd_flags;
            spin_unlock(&listener->lock);
            return EOK;
        }
        default : {
            size_t size_mask = (size_t)_IOC_SIZEMASK << _IOC_SIZESHIFT;
            if ((request & ~size_mask) == ((size_t)SECCOMP_IOCTL_NOTIF_ADDFD & ~size_mask))
                return seccomp_listener_addfd(listener, argument, _IOC_SIZE(request));
            return -EINVAL;
        }
    }
}

static int seccomp_listener_file_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events)
{
    (void)node;
    (void)flags;
    seccomp_listener_t *listener = private_data;
    if (!listener) return POLLERR;
    int ready = 0;
    spin_lock(&listener->lock);
    if (seccomp_listener_has_state(listener, SECCOMP_NOTIFY_INIT)) ready |= POLLIN;
    if (seccomp_listener_has_state(listener, SECCOMP_NOTIFY_SENT)) ready |= POLLOUT;
    if (listener->orphaned) ready |= POLLHUP;
    if (listener->detached) ready |= POLLHUP | POLLERR;
    spin_unlock(&listener->lock);
    return ready & (events | POLLHUP | POLLERR);
}

static vfs_poll_source_t *seccomp_listener_file_poll_source(vfs_node_t node, void *private_data)
{
    (void)node;
    seccomp_listener_t *listener = private_data;
    return listener ? &listener->poll_source : NULL;
}

static int seccomp_listener_vfs_free(void *handle)
{
    seccomp_listener_put(handle);
    return EOK;
}

static vfs_node_t seccomp_listener_node_create(seccomp_listener_t *listener)
{
    if (!listener || seccomp_fsid < 0) return NULL;
    vfs_node_t node = vfs_node_alloc(NULL, "[seccomp]");
    if (!node) return NULL;
    seccomp_listener_get(listener); /* node handle */
    node->type   = file_stream;
    node->handle = listener;
    node->fsid   = seccomp_fsid;
    node->size   = 0;
    node->mode   = O_RDWR;
    return node;
}

static bool seccomp_action_supported(uint32_t action)
{
    switch (action) {
        case SECCOMP_RET_KILL_PROCESS :
        case SECCOMP_RET_KILL_THREAD :
        case SECCOMP_RET_TRAP :
        case SECCOMP_RET_ERRNO :
        case SECCOMP_RET_USER_NOTIF :
        case SECCOMP_RET_TRACE :
        case SECCOMP_RET_LOG :
        case SECCOMP_RET_ALLOW : return true;
        default : return false;
    }
}

static int seccomp_prepare_filter(uint64_t flags, uint64_t user_filter, struct seccomp_filter **result)
{
    struct sock_fprog user_program;
    if (!user_filter || copy_from_user(&user_program, (void *)user_filter, sizeof(user_program))) return -EFAULT;
    if (!user_program.len || user_program.len > SECCOMP_MAX_INSNS_PER_FILTER) return -EINVAL;
    if (!user_program.filter) return -EFAULT;

    struct seccomp_filter *filter = calloc(1, sizeof(*filter));
    if (!filter) return -ENOMEM;
    filter->program = malloc((size_t)user_program.len * sizeof(*filter->program));
    if (!filter->program) {
        free(filter);
        return -ENOMEM;
    }
    if (copy_from_user(filter->program, user_program.filter, (size_t)user_program.len * sizeof(*filter->program))) {
        free(filter->program);
        free(filter);
        return -EFAULT;
    }
    int validation = seccomp_bpf_validate(filter->program, user_program.len);
    if (validation) {
        free(filter->program);
        free(filter);
        return validation;
    }
    filter->refcount = 1;
    filter->length   = user_program.len;
    filter->flags    = flags;
    if (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER) {
        filter->listener = seccomp_listener_alloc(flags);
        if (!filter->listener) {
            seccomp_filter_put(filter);
            return -ENOMEM;
        }
    }
    *result = filter;
    return EOK;
}

static bool seccomp_filter_is_ancestor(struct seccomp_filter *ancestor, struct seccomp_filter *filter)
{
    if (!ancestor) return true;
    for (; filter; filter = filter->prev)
        if (filter == ancestor) return true;
    return false;
}

static bool seccomp_filter_chain_has_listener(struct seccomp_filter *filter)
{
    for (; filter; filter = filter->prev)
        if (filter->listener) return true;
    return false;
}

static int64_t seccomp_install_filter(uint64_t flags, uint64_t user_filter)
{
    const uint64_t valid_flags = SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG | SECCOMP_FILTER_FLAG_SPEC_ALLOW | SECCOMP_FILTER_FLAG_NEW_LISTENER
                               | SECCOMP_FILTER_FLAG_TSYNC_ESRCH | SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV;
    if (flags & ~valid_flags) return -EINVAL;
    if ((flags & SECCOMP_FILTER_FLAG_NEW_LISTENER) && (flags & SECCOMP_FILTER_FLAG_TSYNC)) return -EINVAL;
    if ((flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH) && !(flags & SECCOMP_FILTER_FLAG_TSYNC)) return -EINVAL;
    if ((flags & SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV) && !(flags & SECCOMP_FILTER_FLAG_NEW_LISTENER)) return -EINVAL;

    task_t    *current = current_task();
    process_t *proc    = current ? current->process : NULL;
    if (!current || !proc) return -ESRCH;
    if (!__atomic_load_n(&current->no_new_privs, __ATOMIC_ACQUIRE) && proc->uid != 0) return -EACCES;

    struct seccomp_filter *filter = NULL;
    int status = seccomp_prepare_filter(flags, user_filter, &filter);
    if (status) return status;

    spin_lock(&proc->seccomp_lock);
    if (current->seccomp_mode == SECCOMP_MODE_STRICT) {
        spin_unlock(&proc->seccomp_lock);
        seccomp_filter_put(filter);
        return -EINVAL;
    }
    uint32_t total = filter->length;
    if (current->seccomp_filter) total += current->seccomp_filter->total_insns + SECCOMP_FILTER_CHAIN_PENALTY;
    if (total > SECCOMP_MAX_INSNS_PER_PATH || ((flags & SECCOMP_FILTER_FLAG_NEW_LISTENER) && seccomp_filter_chain_has_listener(current->seccomp_filter))) {
        spin_unlock(&proc->seccomp_lock);
        seccomp_filter_put(filter);
        return total > SECCOMP_MAX_INSNS_PER_PATH ? -ENOMEM : -EBUSY;
    }

    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        for (ilist_node_t *node = proc->threads.next; node != &proc->threads; node = node->next) {
            task_t *thread = rb_entry(node, task_t, thread_node);
            if (thread == current || thread->state == TASK_ZOMBIE) continue;
            if (thread->seccomp_mode == SECCOMP_MODE_STRICT || !seccomp_filter_is_ancestor(thread->seccomp_filter, current->seccomp_filter)) {
                int64_t offender = (int64_t)thread->pid;
                spin_unlock(&proc->seccomp_lock);
                seccomp_filter_put(filter);
                return (flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH) ? -ESRCH : offender;
            }
        }
    }

    int listener_fd = 0;
    if (filter->listener) {
        vfs_node_t node = seccomp_listener_node_create(filter->listener);
        if (!node) {
            spin_unlock(&proc->seccomp_lock);
            seccomp_filter_put(filter);
            return -ENOMEM;
        }
        listener_fd = process_fd_install(proc, node, O_RDWR | O_CLOEXEC);
        if (listener_fd < 0) {
            vfs_close(node);
            spin_unlock(&proc->seccomp_lock);
            seccomp_filter_put(filter);
            return listener_fd;
        }
    }

    filter->total_insns = total;
    filter->prev        = current->seccomp_filter; /* transfer current's reference */
    __atomic_store_n(&current->seccomp_filter, filter, __ATOMIC_RELEASE);
    __atomic_store_n(&current->seccomp_mode, SECCOMP_MODE_FILTER, __ATOMIC_RELEASE);
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        for (ilist_node_t *node = proc->threads.next; node != &proc->threads; node = node->next) {
            task_t *thread = rb_entry(node, task_t, thread_node);
            if (thread == current || thread->state == TASK_ZOMBIE) continue;
            struct seccomp_filter *old = thread->seccomp_filter;
            seccomp_filter_get(filter);
            __atomic_store_n(&thread->seccomp_filter, filter, __ATOMIC_RELEASE);
            __atomic_store_n(&thread->seccomp_mode, SECCOMP_MODE_FILTER, __ATOMIC_RELEASE);
            if (__atomic_load_n(&current->no_new_privs, __ATOMIC_ACQUIRE)) __atomic_store_n(&thread->no_new_privs, true, __ATOMIC_RELEASE);
            seccomp_filter_put(old);
        }
    }
    spin_unlock(&proc->seccomp_lock);
    return filter->listener ? listener_fd : EOK;
}

static int64_t seccomp_install_strict(void)
{
    task_t *task = current_task();
    if (!task || !task->process) return -ESRCH;
    spin_lock(&task->process->seccomp_lock);
    if (task->seccomp_mode != SECCOMP_MODE_DISABLED) {
        spin_unlock(&task->process->seccomp_lock);
        return -EINVAL;
    }
    __atomic_store_n(&task->seccomp_mode, SECCOMP_MODE_STRICT, __ATOMIC_RELEASE);
    spin_unlock(&task->process->seccomp_lock);
    return EOK;
}

int64_t sys_seccomp(uint64_t operation, uint64_t flags, uint64_t user_args, uint64_t unused3, uint64_t unused4, uint64_t unused5)
{
    (void)unused3;
    (void)unused4;
    (void)unused5;
    switch (operation) {
        case SECCOMP_SET_MODE_STRICT :
            if (flags || user_args) return -EINVAL;
            return seccomp_install_strict();
        case SECCOMP_SET_MODE_FILTER : return seccomp_install_filter(flags, user_args);
        case SECCOMP_GET_ACTION_AVAIL : {
            if (flags) return -EINVAL;
            uint32_t action;
            if (copy_from_user(&action, (void *)user_args, sizeof(action))) return -EFAULT;
            return seccomp_action_supported(action) ? EOK : -EOPNOTSUPP;
        }
        case SECCOMP_GET_NOTIF_SIZES : {
            if (flags) return -EINVAL;
            struct seccomp_notif_sizes sizes = {
                .seccomp_notif      = sizeof(struct seccomp_notif),
                .seccomp_notif_resp = sizeof(struct seccomp_notif_resp),
                .seccomp_data       = sizeof(struct seccomp_data),
            };
            return copy_to_user((void *)user_args, &sizes, sizeof(sizes)) ? -EFAULT : EOK;
        }
        default : return -EINVAL;
    }
}

int64_t seccomp_prctl_set(uint64_t mode, uint64_t user_filter)
{
    if (mode == SECCOMP_MODE_STRICT) return seccomp_install_strict();
    if (mode == SECCOMP_MODE_FILTER) return seccomp_install_filter(0, user_filter);
    return -EINVAL;
}

int64_t seccomp_prctl_get(void)
{
    task_t *task = current_task();
    return task ? __atomic_load_n(&task->seccomp_mode, __ATOMIC_ACQUIRE) : -ESRCH;
}

int64_t seccomp_set_no_new_privs(uint64_t value, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    if (value != 1 || arg3 || arg4 || arg5) return -EINVAL;
    task_t *task = current_task();
    if (!task) return -ESRCH;
    __atomic_store_n(&task->no_new_privs, true, __ATOMIC_RELEASE);
    return EOK;
}

int64_t seccomp_get_no_new_privs(uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    if (arg2 || arg3 || arg4 || arg5) return -EINVAL;
    task_t *task = current_task();
    return task ? (__atomic_load_n(&task->no_new_privs, __ATOMIC_ACQUIRE) ? 1 : 0) : -ESRCH;
}

static struct seccomp_filter *seccomp_get_nth_filter(task_t *target, uint64_t offset)
{
    if (!target || !target->process) return NULL;
    process_t *proc = target->process;
    spin_lock(&proc->seccomp_lock);
    if (target->seccomp_mode != SECCOMP_MODE_FILTER) {
        spin_unlock(&proc->seccomp_lock);
        return NULL;
    }
    uint64_t count = 0;
    for (struct seccomp_filter *filter = target->seccomp_filter; filter; filter = filter->prev) count++;
    if (offset >= count) {
        spin_unlock(&proc->seccomp_lock);
        return NULL;
    }
    uint64_t from_head = count - offset - 1U;
    struct seccomp_filter *filter = target->seccomp_filter;
    while (from_head--) filter = filter->prev;
    seccomp_filter_get(filter);
    spin_unlock(&proc->seccomp_lock);
    return filter;
}

int64_t seccomp_ptrace_get_filter(task_t *target, uint64_t filter_offset, void *user_program)
{
    task_t    *current = current_task();
    process_t *proc    = process_current();
    if (!current || !proc) return -ESRCH;
    if (proc->uid != 0 || current->seccomp_mode != SECCOMP_MODE_DISABLED) return -EACCES;
    struct seccomp_filter *filter = seccomp_get_nth_filter(target, filter_offset);
    if (!filter) return target && target->seccomp_mode == SECCOMP_MODE_FILTER ? -ENOENT : -EINVAL;
    int64_t result = filter->length;
    if (user_program && copy_to_user(user_program, filter->program, (size_t)filter->length * sizeof(*filter->program))) result = -EFAULT;
    seccomp_filter_put(filter);
    return result;
}

int64_t seccomp_ptrace_get_metadata(task_t *target, size_t size, void *user_metadata)
{
    task_t    *current = current_task();
    process_t *proc    = process_current();
    if (!current || !proc) return -ESRCH;
    if (proc->uid != 0 || current->seccomp_mode != SECCOMP_MODE_DISABLED) return -EACCES;
    if (size < sizeof(uint64_t)) return -EINVAL;
    if (!user_metadata) return -EFAULT;
    if (size > sizeof(struct seccomp_metadata)) size = sizeof(struct seccomp_metadata);

    struct seccomp_metadata metadata = {0};
    if (copy_from_user(&metadata.filter_off, user_metadata, sizeof(metadata.filter_off))) return -EFAULT;
    struct seccomp_filter *filter = seccomp_get_nth_filter(target, metadata.filter_off);
    if (!filter) return target && target->seccomp_mode == SECCOMP_MODE_FILTER ? -ENOENT : -EINVAL;
    if (filter->flags & SECCOMP_FILTER_FLAG_LOG) metadata.flags |= SECCOMP_FILTER_FLAG_LOG;
    seccomp_filter_put(filter);
    return copy_to_user(user_metadata, &metadata, size) ? -EFAULT : (int64_t)size;
}

static struct seccomp_data seccomp_build_data(syscall_frame_t *frame, uint64_t syscall_nr)
{
    struct seccomp_data data = {
        .nr                  = (int32_t)syscall_nr,
        .arch                = AUDIT_ARCH_X86_64,
        .instruction_pointer = frame->rip,
        .args                 = {frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9},
    };
    return data;
}

static uint32_t seccomp_run_stack(struct seccomp_filter *head, const struct seccomp_data *data, struct seccomp_filter **winner)
{
    uint32_t result = SECCOMP_RET_ALLOW;
    *winner = NULL;
    for (struct seccomp_filter *filter = head; filter; filter = filter->prev) {
        uint32_t current = seccomp_bpf_run(filter->program, filter->length, data);
        if ((int32_t)(current & SECCOMP_RET_ACTION_FULL) < (int32_t)(result & SECCOMP_RET_ACTION_FULL)) {
            result  = current;
            *winner = filter;
        }
    }
    return result;
}

static void seccomp_log_action(task_t *task, const struct seccomp_data *data, uint32_t action)
{
    plogk("seccomp: pid=%llu syscall=%d arch=%x ip=%llx action=%x\n", task ? (unsigned long long)task->pid : 0, data->nr, data->arch,
          (unsigned long long)data->instruction_pointer, action);
}

static void seccomp_send_sigsys(task_t *task, const struct seccomp_data *data, uint16_t reason)
{
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo    = SIGSYS;
    info.si_errno    = reason;
    info.si_code     = SECCOMP_SIGSYS_CODE;
    info.si_call_addr = (void *)(uintptr_t)data->instruction_pointer;
    info.si_syscall  = data->nr;
    info.si_arch     = data->arch;
    signal_send_thread(task, SIGSYS, &info);
}

bool seccomp_enforce(syscall_frame_t *frame, uint64_t *syscall_nr, int64_t *result)
{
    task_t *task = current_task();
    if (!task || !frame || !syscall_nr || !result) return true;
    /* A tracer may cancel a syscall at the normal entry stop by setting a
     * negative syscall number.  Filtering is skipped in that case. */
    if ((int64_t)*syscall_nr < 0) return true;
    uint8_t mode = __atomic_load_n(&task->seccomp_mode, __ATOMIC_ACQUIRE);
    if (mode == SECCOMP_MODE_DISABLED || ptrace_seccomp_suspended(task)) return true;

    if (mode == SECCOMP_MODE_STRICT) {
        if (*syscall_nr == SYS_READ || *syscall_nr == SYS_WRITE || *syscall_nr == SYS_EXIT || *syscall_nr == SYS_RT_SIGRETURN) return true;
        process_exit(-SIGKILL);
    }

    bool trace_seen = false;
    for (;;) {
        struct seccomp_data data = seccomp_build_data(frame, *syscall_nr);
        struct seccomp_filter *winner;
        struct seccomp_filter *head = __atomic_load_n(&task->seccomp_filter, __ATOMIC_ACQUIRE);
        uint32_t decision = seccomp_run_stack(head, &data, &winner);
        uint32_t action   = decision & SECCOMP_RET_ACTION_FULL;
        uint16_t payload  = decision & SECCOMP_RET_DATA;
        if (!seccomp_action_supported(action)) action = SECCOMP_RET_KILL_PROCESS;

        if (action == SECCOMP_RET_LOG || (winner && (winner->flags & SECCOMP_FILTER_FLAG_LOG) && action != SECCOMP_RET_ALLOW)) seccomp_log_action(task, &data, action);

        switch (action) {
            case SECCOMP_RET_ALLOW : return true;
            case SECCOMP_RET_LOG : return true;
            case SECCOMP_RET_ERRNO :
                *result = -(int64_t)(payload > SECCOMP_MAX_ERRNO ? SECCOMP_MAX_ERRNO : payload);
                return false;
            case SECCOMP_RET_TRAP :
                *result = -ENOSYS;
                seccomp_send_sigsys(task, &data, payload);
                return false;
            case SECCOMP_RET_USER_NOTIF : return seccomp_notify_wait(winner ? winner->listener : NULL, &data, result);
            case SECCOMP_RET_TRACE : {
                if (trace_seen || !ptrace_seccomp_event(frame, payload, result)) {
                    if (trace_seen) return true;
                    *result = -ENOSYS;
                    return false;
                }
                trace_seen  = true;
                *syscall_nr = frame->rax;
                if ((int64_t)*syscall_nr < 0) return false;
                continue;
            }
            case SECCOMP_RET_KILL_THREAD : process_exit(-SIGSYS);
            case SECCOMP_RET_KILL_PROCESS : process_exit_group(-SIGSYS);
            default : process_exit_group(-SIGSYS);
        }
    }
}

void seccomp_init(void)
{
    if (seccomp_fsid >= 0) return;
    vfs_callback_t callback = calloc(1, sizeof(struct vfs_callback));
    if (!callback) {
        plogk("seccomp: failed to allocate listener callbacks\n");
        return;
    }
    callback->file_open             = seccomp_listener_file_open;
    callback->file_release          = seccomp_listener_file_release;
    callback->file_descriptor_close = seccomp_listener_descriptor_close;
    callback->file_ioctl            = seccomp_listener_file_ioctl;
    callback->file_poll             = seccomp_listener_file_poll;
    callback->file_poll_source      = seccomp_listener_file_poll_source;
    callback->free                  = seccomp_listener_vfs_free;
    seccomp_fsid = vfs_regist(callback);
    free(callback);
    if (seccomp_fsid < 0)
        plogk("seccomp: listener filesystem registration failed (%d)\n", seccomp_fsid);
    else
        plogk("seccomp: secure-computing subsystem registered (fsid=%d)\n", seccomp_fsid);
}
