/*
 *
 *      ofd_lock.c
 *      Whole-file open-description locks for service-manager lock files
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/fcntl.h>
#include <syscall/syscall.h>

typedef struct {
        int16_t type;
        int16_t whence;
        int64_t start;
        int64_t length;
        int32_t pid;
} linux_flock_t;

_Static_assert(sizeof(linux_flock_t) == 32, "x86-64 flock ABI");

typedef struct ofd_record {
        process_file_t    *owner; // valid until the open description's final put
        int16_t            type;
        struct ofd_record *next;
} ofd_record_t;

static spinlock_t    ofd_guard;
static wait_queue_t  ofd_wait;
static bool          ofd_ready;
static ofd_record_t *ofd_records;

static bool same_inode(const process_file_t *a, const process_file_t *b)
{
    if (a->node == b->node) return true;
    if (a->node->fsid != b->node->fsid) return false;
    return (a->node->inode && a->node->inode == b->node->inode) || (a->node->handle && a->node->handle == b->node->handle);
}

/* Release also wakes waiters on descriptions shared through dup/fork/SCM_RIGHTS. */
void ofd_lock_release(process_file_t *file)
{
    ofd_record_t *removed = NULL;
    spin_lock(&ofd_guard);
    ofd_record_t **link = &ofd_records;
    while (*link) {
        if ((*link)->owner != file) {
            link = &(*link)->next;
            continue;
        }
        removed = *link;
        *link   = removed->next;
        break;
    }
    if (removed) wait_queue_wake_all(&ofd_wait);
    spin_unlock(&ofd_guard);
    free(removed);
}

/* Byte ranges remain explicitly unsupported; never pretend they are locked. */
int ofd_lock_command(process_file_t *file, int command, uint64_t user)
{
    linux_flock_t request;
    if (!user || copy_from_user(&request, (const void *)user, sizeof(request))) return -EFAULT;
    if (request.pid || request.whence) return -EINVAL;
    if (request.start || request.length) return -EOPNOTSUPP;
    if (command == F_OFD_GETLK && request.type == F_UNLCK) return -EINVAL;
    if (request.type != F_RDLCK && request.type != F_WRLCK && request.type != F_UNLCK) return -EINVAL;
    spin_lock(&file->lock);
    uint64_t status_flags = file->flags;
    spin_unlock(&file->lock);
    if (status_flags & O_PATH) return -EBADF;
    if (request.type == F_WRLCK && (status_flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (request.type == F_RDLCK && (status_flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (request.type == F_UNLCK && command != F_OFD_GETLK) {
        ofd_lock_release(file);
        return 0;
    }
    ofd_record_t *spare = command == F_OFD_GETLK ? NULL : calloc(1, sizeof(*spare));
    if (command != F_OFD_GETLK && !spare) return -ENOMEM;
    int result = 0;
    for (;;) {
        spin_lock(&ofd_guard);
        if (!ofd_ready) {
            wait_queue_init(&ofd_wait);
            ofd_ready = true;
        }
        ofd_record_t *own = NULL, *conflict = NULL;
        for (ofd_record_t *record = ofd_records; record; record = record->next) {
            if (record->owner == file) {
                own = record;
                continue;
            }
            if (same_inode(record->owner, file) && (request.type == F_WRLCK || record->type == F_WRLCK)) {
                conflict = record;
                break;
            }
        }
        if (command == F_OFD_GETLK) {
            request.type = conflict ? conflict->type : F_UNLCK;
            request.pid  = conflict ? -1 : 0;
            spin_unlock(&ofd_guard);
            result = copy_to_user((void *)user, &request, sizeof(request)) ? -EFAULT : 0;
            break;
        }
        if (!conflict) {
            if (own)
                own->type = request.type;
            else {
                spare->owner = file;
                spare->type  = request.type;
                spare->next  = ofd_records;
                ofd_records  = spare;
                spare        = NULL;
            }
            wait_queue_wake_all(&ofd_wait);
            spin_unlock(&ofd_guard);
            break;
        }
        if (command == F_OFD_SETLK) {
            spin_unlock(&ofd_guard);
            result = -EAGAIN;
            break;
        }
        if (signal_has_interrupting_pending(&process_current()->signal)) {
            spin_unlock(&ofd_guard);
            result = -EINTR;
            break;
        }
        wait_queue_prepare(&ofd_wait);
        spin_unlock(&ofd_guard);
        (void)wait_queue_wait_timed(&ofd_wait, sched_ticks() + CONFIG_TIMER_HZ);
    }
    free(spare);
    return result;
}
