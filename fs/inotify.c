/*
 *
 *      inotify.c
 *      Linux-compatible filesystem event notification
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/inotify.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/termios.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <syscall/fcntl.h>

#define INOTIFY_EVENT_MASK  (IN_ALL_EVENTS | IN_UNMOUNT | IN_Q_OVERFLOW | IN_IGNORED)
#define INOTIFY_WATCH_FLAGS (IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK | IN_MASK_CREATE | IN_MASK_ADD | IN_ONESHOT)

struct inotify_watch {
        inotify_watch_t *next;
        inotify_watch_t *release_next;
        vfs_node_t       node;
        int32_t          wd;
        uint32_t         mask;
};

static spinlock_t         inotify_global_lock;
static inotify_context_t *inotify_contexts;
static uint32_t           inotify_cookie;
#ifndef INOTIFY_HOST_TEST
static int inotify_fsid = -1;
#endif

static size_t inotify_name_size(const char *name)
{
    if (!name || !name[0]) return 0;
    return (strlen(name) + 1U + 3U) & ~(size_t)3U;
}

static bool inotify_events_equal(const inotify_queue_event_t *queued, int32_t wd, uint32_t mask, uint32_t cookie, const char *name,
                                 size_t name_size)
{
    if (!queued || queued->event.wd != wd || queued->event.mask != mask || queued->event.cookie != cookie || queued->event.len != name_size)
        return false;
    if (!name_size) return true;
    return memcmp(queued->event.name, name, strlen(name) + 1U) == 0;
}

int inotify_queue_event(inotify_context_t *context, int32_t wd, uint32_t mask, uint32_t cookie, const char *name)
{
    if (!context) return -EINVAL;

    size_t name_size  = inotify_name_size(name);
    size_t event_size = sizeof(struct inotify_event) + name_size;

    spin_lock(&context->lock);
    if (context->closed) {
        spin_unlock(&context->lock);
        return -EBADF;
    }
    if (inotify_events_equal(context->tail, wd, mask, cookie, name, name_size)) {
        spin_unlock(&context->lock);
        return EOK;
    }
    if (context->queued_events >= INOTIFY_MAX_QUEUED_EVENTS) {
        if (context->overflow_queued) {
            spin_unlock(&context->lock);
            return EOK;
        }
        wd         = -1;
        mask       = IN_Q_OVERFLOW;
        cookie     = 0;
        name       = NULL;
        name_size  = 0;
        event_size = sizeof(struct inotify_event);
    }

    inotify_queue_event_t *queued = calloc(1, sizeof(*queued) + name_size);
    if (!queued) {
        spin_unlock(&context->lock);
        return -ENOMEM;
    }
    queued->size         = event_size;
    queued->event.wd     = wd;
    queued->event.mask   = mask;
    queued->event.cookie = cookie;
    queued->event.len    = (uint32_t)name_size;
    if (name_size) memcpy(queued->event.name, name, strlen(name) + 1U);

    if (context->tail)
        context->tail->next = queued;
    else
        context->head = queued;
    context->tail = queued;
    context->queued_events++;
    context->queued_bytes += event_size;
    if (mask == IN_Q_OVERFLOW) context->overflow_queued = true;
    spin_unlock(&context->lock);

    wait_queue_wake_all(&context->wait_queue);
    vfs_poll_notify(context->node, 0x001U);
    return EOK;
}

static bool inotify_signal_pending(void)
{
#ifdef INOTIFY_HOST_TEST
    return false;
#else
    process_t *process = process_current();
    return process && signal_has_pending(&process->signal);
#endif
}

int64_t inotify_read_events(inotify_context_t *context, uint64_t flags, void *buffer, size_t size)
{
    if (!context || (!buffer && size)) return -EINVAL;

    for (;;) {
        spin_lock(&context->lock);
        if (context->head) break;
        if (context->closed) {
            spin_unlock(&context->lock);
            return 0;
        }
        if (flags & IN_NONBLOCK) {
            spin_unlock(&context->lock);
            return -EAGAIN;
        }
        if (inotify_signal_pending()) {
            spin_unlock(&context->lock);
            return -ERESTARTSYS;
        }
        wait_queue_prepare(&context->wait_queue);
        spin_unlock(&context->lock);
        wait_queue_sleep();
    }

    size_t copied = 0;
    while (context->head) {
        inotify_queue_event_t *queued = context->head;
        if (queued->size > size - copied) {
            if (!copied) {
                spin_unlock(&context->lock);
                return -EINVAL;
            }
            break;
        }
        memcpy((uint8_t *)buffer + copied, &queued->event, queued->size);
        copied += queued->size;
        context->head = queued->next;
        if (!context->head) context->tail = NULL;
        context->queued_events--;
        context->queued_bytes -= queued->size;
        if (queued->event.mask == IN_Q_OVERFLOW) context->overflow_queued = false;
        free(queued);
    }
    spin_unlock(&context->lock);
    return (int64_t)copied;
}

static void inotify_release_watches(inotify_watch_t *watches)
{
    while (watches) {
        inotify_watch_t *next = watches->release_next;
        vfs_close(watches->node);
        free(watches);
        watches = next;
    }
}

static void inotify_release_append(inotify_watch_t **head, inotify_watch_t *watch)
{
    watch->release_next = *head;
    *head               = watch;
}

static void inotify_emit(vfs_node_t target, uint32_t mask, uint32_t cookie, const char *name, bool force_remove)
{
    if (!target || !(mask & INOTIFY_EVENT_MASK)) return;

    inotify_watch_t *release = NULL;
    spin_lock(&inotify_global_lock);
    for (inotify_context_t *context = inotify_contexts; context; context = context->next) {
        inotify_watch_t **link = &context->watches;
        while (*link) {
            inotify_watch_t *watch = *link;
            if (watch->node != target) {
                link = &watch->next;
                continue;
            }
            bool matched = (watch->mask & mask & INOTIFY_EVENT_MASK) != 0;
            if (matched) (void)inotify_queue_event(context, watch->wd, mask, cookie, name);
            if (force_remove || (matched && (watch->mask & IN_ONESHOT))) {
                *link = watch->next;
                (void)inotify_queue_event(context, watch->wd, IN_IGNORED, 0, NULL);
                inotify_release_append(&release, watch);
            } else {
                link = &watch->next;
            }
        }
    }
    spin_unlock(&inotify_global_lock);
    inotify_release_watches(release);
}

void inotify_notify(vfs_node_t node, uint32_t mask)
{
    if (!node) return;
    uint32_t type_mask = (node->type & file_dir) ? IN_ISDIR : 0;
    inotify_emit(node, mask | type_mask, 0, NULL, false);
    if (node->parent && !(node->flags & VFS_NODE_UNLINKED)) {
        inotify_emit(node->parent, mask | type_mask, 0, node->name, false);
    } else if (node->parent && (node->flags & VFS_NODE_UNLINKED)) {
        /* IN_EXCL_UNLINK is evaluated per watch below; non-exclusive watches still receive the event. */
        inotify_watch_t *release = NULL;
        spin_lock(&inotify_global_lock);
        for (inotify_context_t *context = inotify_contexts; context; context = context->next) {
            inotify_watch_t **link = &context->watches;
            while (*link) {
                inotify_watch_t *watch = *link;
                if (watch->node != node->parent || (watch->mask & IN_EXCL_UNLINK) || !(watch->mask & mask)) {
                    link = &watch->next;
                    continue;
                }
                (void)inotify_queue_event(context, watch->wd, mask | type_mask, 0, node->name);
                if (watch->mask & IN_ONESHOT) {
                    *link = watch->next;
                    (void)inotify_queue_event(context, watch->wd, IN_IGNORED, 0, NULL);
                    inotify_release_append(&release, watch);
                } else {
                    link = &watch->next;
                }
            }
        }
        spin_unlock(&inotify_global_lock);
        inotify_release_watches(release);
    }
}

void inotify_notify_create(vfs_node_t parent, vfs_node_t node)
{
    if (!parent || !node) return;
    inotify_emit(parent, IN_CREATE | ((node->type & file_dir) ? IN_ISDIR : 0), 0, node->name, false);
}

void inotify_notify_delete(vfs_node_t node)
{
    if (!node) return;
    uint32_t type_mask = (node->type & file_dir) ? IN_ISDIR : 0;
    if (node->parent) inotify_emit(node->parent, IN_DELETE | type_mask, 0, node->name, false);
    inotify_emit(node, IN_DELETE_SELF | type_mask, 0, NULL, true);
}

uint32_t inotify_next_cookie(void)
{
    uint32_t cookie = __atomic_add_fetch(&inotify_cookie, 1, __ATOMIC_RELAXED);
    if (!cookie) cookie = __atomic_add_fetch(&inotify_cookie, 1, __ATOMIC_RELAXED);
    return cookie;
}

void inotify_notify_move(vfs_node_t node, const char *old_name, const char *new_name)
{
    if (!node || !node->parent || !old_name || !new_name) return;
    uint32_t cookie    = inotify_next_cookie();
    uint32_t type_mask = (node->type & file_dir) ? IN_ISDIR : 0;
    inotify_emit(node->parent, IN_MOVED_FROM | type_mask, cookie, old_name, false);
    inotify_emit(node->parent, IN_MOVED_TO | type_mask, cookie, new_name, false);
    inotify_emit(node, IN_MOVE_SELF | type_mask, 0, NULL, false);
}

void inotify_notify_unmount(vfs_node_t mount_root)
{
    if (!mount_root) return;
    inotify_watch_t *release = NULL;
    spin_lock(&inotify_global_lock);
    for (inotify_context_t *context = inotify_contexts; context; context = context->next) {
        inotify_watch_t **link = &context->watches;
        while (*link) {
            inotify_watch_t *watch = *link;
            if (watch->node != mount_root && watch->node->root != mount_root) {
                link = &watch->next;
                continue;
            }
            *link = watch->next;
            (void)inotify_queue_event(context, watch->wd, IN_UNMOUNT | ((watch->node->type & file_dir) ? IN_ISDIR : 0), 0, NULL);
            (void)inotify_queue_event(context, watch->wd, IN_IGNORED, 0, NULL);
            inotify_release_append(&release, watch);
        }
    }
    spin_unlock(&inotify_global_lock);
    inotify_release_watches(release);
}

#ifndef INOTIFY_HOST_TEST
static int64_t inotify_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *address, size_t offset, size_t size)
{
    (void)private_data;
    (void)offset;
    return inotify_read_events((inotify_context_t *)node->handle, flags, address, size);
}

static int inotify_file_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events)
{
    (void)private_data;
    (void)flags;
    inotify_context_t *context = (inotify_context_t *)node->handle;
    if (!context) return 0;
    spin_lock(&context->lock);
    int ready = context->head ? 0x001 : 0;
    if (context->closed) ready |= 0x010;
    spin_unlock(&context->lock);
    return ready & (int)events;
}

static int inotify_file_ioctl(vfs_node_t node, void *private_data, uint64_t flags, size_t request, void *argument)
{
    (void)private_data;
    (void)flags;
    if (request != FIONREAD) return -ENOTTY;
    if (!argument) return -EFAULT;
    inotify_context_t *context = (inotify_context_t *)node->handle;
    spin_lock(&context->lock);
    int bytes = context->queued_bytes > 0x7fffffffU ? 0x7fffffff : (int)context->queued_bytes;
    spin_unlock(&context->lock);
    return copy_to_user(argument, &bytes, sizeof(bytes)) ? -EFAULT : EOK;
}

static void inotify_close(void *handle)
{
    inotify_context_t *context = (inotify_context_t *)handle;
    if (!context) return;
    spin_lock(&context->lock);
    context->closed = true;
    spin_unlock(&context->lock);
    wait_queue_wake_all(&context->wait_queue);
}

static int inotify_free(void *handle)
{
    inotify_context_t *context = (inotify_context_t *)handle;
    if (!context) return -EINVAL;

    spin_lock(&inotify_global_lock);
    inotify_context_t **context_link = &inotify_contexts;
    while (*context_link && *context_link != context) context_link = &(*context_link)->next;
    if (*context_link) *context_link = context->next;
    inotify_watch_t *watches = context->watches;
    context->watches         = NULL;
    spin_unlock(&inotify_global_lock);

    for (inotify_watch_t *watch = watches; watch; watch = watch->next) watch->release_next = watch->next;
    inotify_release_watches(watches);
    while (context->head) {
        inotify_queue_event_t *next = context->head->next;
        free(context->head);
        context->head = next;
    }
    free(context);
    return EOK;
}

static vfs_node_t inotify_node_create(int *error)
{
    if (error) *error = -ENOMEM;
    if (inotify_fsid < 0) return NULL;
    process_t *process = process_current();
    if (!process) return NULL;
    inotify_context_t *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    wait_queue_init(&context->wait_queue);
    context->next_watch_descriptor = 1;
    context->owner_uid             = process->uid;

    vfs_node_t node = vfs_node_alloc(NULL, "[inotify]");
    if (!node) {
        free(context);
        return NULL;
    }
    node->type    = file_stream;
    node->handle  = context;
    node->fsid    = (uint16_t)inotify_fsid;
    node->mode    = O_RDONLY;
    context->node = node;

    spin_lock(&inotify_global_lock);
    uint32_t instances = 0;
    for (inotify_context_t *current = inotify_contexts; current; current = current->next)
        if (current->owner_uid == context->owner_uid) instances++;
    if (instances >= INOTIFY_MAX_USER_INSTANCES) {
        spin_unlock(&inotify_global_lock);
        node->handle = NULL;
        vfs_free(node);
        free(context);
        if (error) *error = -EMFILE;
        return NULL;
    }
    context->next    = inotify_contexts;
    inotify_contexts = context;
    spin_unlock(&inotify_global_lock);
    return node;
}

static inotify_context_t *inotify_context_get(int fd, process_file_t **file_out)
{
    process_t *process = process_current();
    if (!process) return NULL;
    process_file_t *file = process_fd_get(process, fd);
    if (!file) return NULL;
    if (!file->node || file->node->fsid != (uint16_t)inotify_fsid || !file->node->handle) {
        process_file_put(file);
        return NULL;
    }
    *file_out = file;
    return (inotify_context_t *)file->node->handle;
}

int sys_inotify_init1(int flags)
{
    if (flags & ~(IN_CLOEXEC | IN_NONBLOCK)) return -EINVAL;
    process_t *process = process_current();
    if (!process) return -ESRCH;
    int        error;
    vfs_node_t node = inotify_node_create(&error);
    if (!node) return error;
    int fd = process_fd_install(process, node, O_RDONLY | (uint64_t)flags);
    if (fd < 0) vfs_close(node);
    return fd;
}

int sys_inotify_init(void)
{
    return sys_inotify_init1(0);
}

static int32_t inotify_allocate_wd(inotify_context_t *context)
{
    int32_t candidate = context->next_watch_descriptor;
    if (candidate <= 0) candidate = 1;
    for (uint32_t tries = 0; tries < 0x7fffffffU; tries++) {
        bool used = false;
        for (inotify_watch_t *watch = context->watches; watch; watch = watch->next) {
            if (watch->wd == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            context->next_watch_descriptor = candidate == 0x7fffffff ? 1 : candidate + 1;
            return candidate;
        }
        candidate = candidate == 0x7fffffff ? 1 : candidate + 1;
    }
    return -1;
}

int sys_inotify_add_watch(int fd, const char *pathname, uint32_t mask)
{
    if (!pathname) return -EFAULT;
    if (!(mask & IN_ALL_EVENTS) || (mask & ~(INOTIFY_EVENT_MASK | INOTIFY_WATCH_FLAGS))) return -EINVAL;
    if ((mask & (IN_MASK_ADD | IN_MASK_CREATE)) == (IN_MASK_ADD | IN_MASK_CREATE)) return -EINVAL;

    char path[VFS_PATH_MAX];
    int  copied = strncpy_from_user(path, pathname, sizeof(path));
    if (copied < 0) return copied;
    path[sizeof(path) - 1] = '\0';

    process_t *process = process_current();
    if (!process) return -ESRCH;

    process_file_t    *file    = NULL;
    inotify_context_t *context = inotify_context_get(fd, &file);
    if (!context) return -EBADF;

    char resolved[VFS_PATH_MAX];
    int  result = process_resolve_path_at(process, PROCESS_AT_FDCWD, path, resolved, sizeof(resolved));
    if (result) {
        process_file_put(file);
        return result;
    }
    vfs_node_t node = (mask & IN_DONT_FOLLOW) ? vfs_open_nofollow(resolved) : vfs_open(resolved);
    if (!node) {
        process_file_put(file);
        return -ENOENT;
    }
    if ((mask & IN_ONLYDIR) && !(node->type & file_dir)) {
        process_file_put(file);
        vfs_close(node);
        return -ENOTDIR;
    }
    if (vfs_access_check(node, VFS_ACCESS_R)) {
        process_file_put(file);
        vfs_close(node);
        return -EACCES;
    }

    spin_lock(&inotify_global_lock);
    for (inotify_watch_t *watch = context->watches; watch; watch = watch->next) {
        if (watch->node != node) continue;
        if (mask & IN_MASK_CREATE) {
            result = -EEXIST;
        } else {
            uint32_t stored = mask & (IN_ALL_EVENTS | IN_EXCL_UNLINK | IN_ONESHOT);
            watch->mask     = (mask & IN_MASK_ADD) ? (watch->mask | stored) : stored;
            result          = watch->wd;
        }
        spin_unlock(&inotify_global_lock);
        process_file_put(file);
        vfs_close(node);
        return result;
    }

    uint32_t watches = 0;
    for (inotify_context_t *current = inotify_contexts; current; current = current->next) {
        if (current->owner_uid != context->owner_uid) continue;
        for (inotify_watch_t *entry = current->watches; entry; entry = entry->next) watches++;
    }
    if (watches >= INOTIFY_MAX_USER_WATCHES) {
        spin_unlock(&inotify_global_lock);
        process_file_put(file);
        vfs_close(node);
        return -ENOSPC;
    }

    inotify_watch_t *watch = calloc(1, sizeof(*watch));
    if (!watch) {
        spin_unlock(&inotify_global_lock);
        process_file_put(file);
        vfs_close(node);
        return -ENOMEM;
    }
    watch->wd = inotify_allocate_wd(context);
    if (watch->wd < 0) {
        free(watch);
        spin_unlock(&inotify_global_lock);
        process_file_put(file);
        vfs_close(node);
        return -ENOSPC;
    }
    watch->node      = node;
    watch->mask      = mask & (IN_ALL_EVENTS | IN_EXCL_UNLINK | IN_ONESHOT);
    watch->next      = context->watches;
    context->watches = watch;
    result           = watch->wd;
    spin_unlock(&inotify_global_lock);
    process_file_put(file);
    return result;
}

int sys_inotify_rm_watch(int fd, int wd)
{
    process_file_t    *file    = NULL;
    inotify_context_t *context = inotify_context_get(fd, &file);
    if (!context) return -EBADF;

    spin_lock(&inotify_global_lock);
    inotify_watch_t **link = &context->watches;
    while (*link && (*link)->wd != wd) link = &(*link)->next;
    if (!*link) {
        spin_unlock(&inotify_global_lock);
        process_file_put(file);
        return -EINVAL;
    }
    inotify_watch_t *watch = *link;
    *link                  = watch->next;
    (void)inotify_queue_event(context, watch->wd, IN_IGNORED, 0, NULL);
    spin_unlock(&inotify_global_lock);
    vfs_close(watch->node);
    free(watch);
    process_file_put(file);
    return EOK;
}

void inotify_init(void)
{
    vfs_callback_t callback = calloc(1, sizeof(*callback));
    if (!callback) return;
    callback->close      = inotify_close;
    callback->free       = inotify_free;
    callback->file_read  = inotify_file_read;
    callback->file_ioctl = inotify_file_ioctl;
    callback->file_poll  = inotify_file_poll;
    inotify_fsid         = vfs_regist(callback);
    free(callback);
    if (inotify_fsid < 0) plogk("inotify: Failed to register VFS callbacks (%d).\n", inotify_fsid);
}
#endif
