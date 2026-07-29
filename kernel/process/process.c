/*
 *
 *      process.c
 *      Process management
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/eis.h>
#include <arch/smp.h>
#include <chipset/common.h>
#include <drivers/char/tty_core.h>
#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <ipc/epoll.h>
#include <ipc/futex.h>
#include <ipc/posix_mq.h>
#include <ipc/socket.h>
#include <ipc/sysv_ipc.h>
#include <kernel/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/ptrace.h>
#include <proc/sched.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/memfd.h>
#include <syscall/syscall.h>

/* Pipe init extern declaration */
extern void pipe_init(void);

#ifndef PROCESS_TABLE_SIZE
#    define PROCESS_TABLE_SIZE 4096
#endif

static process_t *process_table[PROCESS_TABLE_SIZE];
static spinlock_t process_table_lock;
process_t        *init_process;

static process_t *pid_to_process(pid_t pid)
{
    if (pid <= 0 || pid >= PROCESS_TABLE_SIZE) return NULL;
    spin_lock(&process_table_lock);
    process_t *proc = process_table[pid];
    spin_unlock(&process_table_lock);
    return proc;
}

static process_t *pid_to_process_locked(pid_t pid)
{
    if (pid <= 0 || pid >= PROCESS_TABLE_SIZE) return NULL;
    return process_table[pid];
}

static void pid_set(pid_t pid, process_t *proc)
{
    if (pid <= 0 || pid >= PROCESS_TABLE_SIZE) return;
    spin_lock(&process_table_lock);
    process_table[pid] = proc;
    spin_unlock(&process_table_lock);
}

static void pid_set_locked(pid_t pid, process_t *proc)
{
    if (pid <= 0 || pid >= PROCESS_TABLE_SIZE) return;
    process_table[pid] = proc;
}

static void process_free(process_t *proc);

static void process_get_locked(process_t *proc)
{
    if (proc) proc->refcount++;
}

void process_put(process_t *proc)
{
    if (!proc) return;

    bool destroy = false;
    spin_lock(&process_table_lock);
    if (proc->refcount && --proc->refcount == 0) destroy = true;
    spin_unlock(&process_table_lock);
    if (destroy) process_free(proc);
}

process_t *process_find_get(pid_t pid)
{
    spin_lock(&process_table_lock);
    process_t *proc = pid_to_process_locked(pid);
    process_get_locked(proc);
    spin_unlock(&process_table_lock);
    return proc;
}

process_t *process_iterate_get(size_t *pos)
{
    if (!pos) return NULL;

    spin_lock(&process_table_lock);
    for (; *pos < PROCESS_TABLE_SIZE; (*pos)++) {
        process_t *proc = process_table[*pos];
        if (proc) {
            (*pos)++;
            process_get_locked(proc);
            spin_unlock(&process_table_lock);
            return proc;
        }
    }
    spin_unlock(&process_table_lock);
    return NULL;
}

process_t *process_group_iterate_get(size_t *pos, pid_t pgid, pid_t sid)
{
    if (!pos || pgid <= 0) return NULL;

    spin_lock(&process_table_lock);
    for (; *pos < PROCESS_TABLE_SIZE; (*pos)++) {
        process_t *proc = process_table[*pos];
        if (proc && proc->pgid == pgid && (sid <= 0 || proc->sid == sid)) {
            (*pos)++;
            process_get_locked(proc);
            spin_unlock(&process_table_lock);
            return proc;
        }
    }
    spin_unlock(&process_table_lock);
    return NULL;
}

tty_core_t *process_ctty_get(process_t *proc)
{
    if (!proc) return NULL;

    spin_lock(&process_table_lock);
    tty_core_t *tty = proc->controlling_tty;
    if (tty) tty_core_retain(tty);
    spin_unlock(&process_table_lock);
    return tty;
}

int process_ctty_set(process_t *proc, tty_core_t *tty)
{
    if (!proc || !tty) return -EINVAL;

    spin_lock(&process_table_lock);
    if (proc->controlling_tty && proc->controlling_tty != tty) {
        spin_unlock(&process_table_lock);
        return -EPERM;
    }
    if (!proc->controlling_tty) {
        tty_core_retain(tty);
        proc->controlling_tty = tty;
    }
    spin_unlock(&process_table_lock);
    return 0;
}

void process_ctty_clear(process_t *proc)
{
    if (!proc) return;

    spin_lock(&process_table_lock);
    tty_core_t *tty       = proc->controlling_tty;
    proc->controlling_tty = NULL;
    spin_unlock(&process_table_lock);
    if (tty) tty_core_release(tty);
}

static void process_ctty_clear_matching(tty_core_t *tty, pid_t sid, bool match_session)
{
    if (!tty) return;

    for (;;) {
        tty_core_t *release = NULL;
        spin_lock(&process_table_lock);
        for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
            process_t *proc = process_table[i];
            if (proc && proc->controlling_tty == tty && (!match_session || proc->sid == sid)) {
                proc->controlling_tty = NULL;
                release               = tty;
                break;
            }
        }
        spin_unlock(&process_table_lock);
        if (!release) break;
        tty_core_release(release);
    }
}

void process_ctty_clear_session(tty_core_t *tty, pid_t sid)
{
    process_ctty_clear_matching(tty, sid, true);
}

void process_ctty_clear_all(tty_core_t *tty)
{
    process_ctty_clear_matching(tty, 0, false);
}

void process_ctty_inherit(process_t *child, process_t *parent)
{
    if (!child || !parent) return;

    tty_core_t *tty = process_ctty_get(parent);
    if (!tty) return;

    spin_lock(&tty->lock);
    spin_lock(&process_table_lock);
    if (parent->controlling_tty == tty && tty->session == parent->sid && !tty->hung_up) {
        child->controlling_tty = tty;
        tty_core_retain(tty);
    }
    spin_unlock(&process_table_lock);
    spin_unlock(&tty->lock);
    tty_core_release(tty);
}

bool process_pgrp_in_session(pid_t pgid, pid_t sid)
{
    bool found = false;

    spin_lock(&process_table_lock);
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
        process_t *proc = process_table[i];
        if (proc && proc->pgid == pgid && proc->sid == sid) {
            found = true;
            break;
        }
    }
    spin_unlock(&process_table_lock);
    return found;
}

int process_ctty_set_foreground(tty_core_t *tty, pid_t sid, pid_t pgid)
{
    if (!tty || sid <= 0 || pgid <= 0) return -EINVAL;

    spin_lock(&tty->lock);
    spin_lock(&process_table_lock);
    if (tty->session != sid) {
        spin_unlock(&process_table_lock);
        spin_unlock(&tty->lock);
        return -ENOTTY;
    }
    bool found = false;
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
        process_t *proc = process_table[i];
        if (proc && proc->pgid == pgid && proc->sid == sid) {
            found = true;
            break;
        }
    }
    if (found) tty->foreground_pgid = pgid;
    spin_unlock(&process_table_lock);
    spin_unlock(&tty->lock);
    return found ? 0 : -EPERM;
}

int process_ctty_acquire(process_t *proc, tty_core_t *tty, bool force, pid_t *old_sid, pid_t *old_pgid)
{
    if (!proc || !tty) return -EINVAL;

    size_t releases = 0;
    spin_lock(&tty->lock);
    spin_lock(&process_table_lock);
    if (tty->hung_up || (proc->controlling_tty && proc->controlling_tty != tty) || (tty->session && tty->session != proc->sid && !force)) {
        spin_unlock(&process_table_lock);
        spin_unlock(&tty->lock);
        return tty->hung_up ? -EIO : -EPERM;
    }

    pid_t previous_sid  = tty->session;
    pid_t previous_pgid = tty->foreground_pgid;
    if (previous_sid && previous_sid != proc->sid) {
        for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
            process_t *member = process_table[i];
            if (member && member->sid == previous_sid && member->controlling_tty == tty) {
                member->controlling_tty = NULL;
                releases++;
            }
        }
    }
    if (!proc->controlling_tty) {
        tty_core_retain(tty);
        proc->controlling_tty = tty;
    }
    tty->session         = proc->sid;
    tty->foreground_pgid = proc->pgid;
    spin_unlock(&process_table_lock);
    spin_unlock(&tty->lock);

    while (releases--) tty_core_release(tty);
    if (old_sid) *old_sid = previous_sid;
    if (old_pgid) *old_pgid = previous_pgid;
    return 0;
}

pid_t process_ctty_disassociate(tty_core_t *tty, pid_t sid)
{
    if (!tty || sid <= 0) return -1;

    size_t releases = 0;
    pid_t  old_pgid = -1;
    spin_lock(&tty->lock);
    spin_lock(&process_table_lock);
    if (tty->session == sid) {
        old_pgid             = tty->foreground_pgid;
        tty->session         = 0;
        tty->foreground_pgid = 0;
        for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
            process_t *member = process_table[i];
            if (member && member->sid == sid && member->controlling_tty == tty) {
                member->controlling_tty = NULL;
                releases++;
            }
        }
    }
    spin_unlock(&process_table_lock);
    spin_unlock(&tty->lock);
    while (releases--) tty_core_release(tty);
    return old_pgid;
}

int process_setpgid(process_t *caller, pid_t pid, pid_t pgid)
{
    if (!caller || pid < 0 || pgid < 0) return -EINVAL;

    spin_lock(&process_table_lock);
    process_t *target = pid ? pid_to_process_locked(pid) : caller;
    if (!target) {
        spin_unlock(&process_table_lock);
        return -ESRCH;
    }
    if (target != caller && target->parent != caller) {
        spin_unlock(&process_table_lock);
        return -ESRCH;
    }
    pid_t target_pid = (pid_t)target->task->tgid;
    if (target->sid != caller->sid || target->sid == target_pid) {
        spin_unlock(&process_table_lock);
        return -EPERM;
    }
    if (!pgid) pgid = target_pid;
    if (pgid != target_pid) {
        bool valid_group = false;
        for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
            process_t *member = process_table[i];
            if (member && member->pgid == pgid && member->sid == caller->sid) {
                valid_group = true;
                break;
            }
        }
        if (!valid_group) {
            spin_unlock(&process_table_lock);
            return -EPERM;
        }
    }
    target->pgid = pgid;
    spin_unlock(&process_table_lock);
    return 0;
}

int process_setsid(process_t *proc, pid_t *sid)
{
    if (!proc || !proc->task) return -ESRCH;

    pid_t pid = (pid_t)proc->task->tgid;
    spin_lock(&process_table_lock);
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
        process_t *member = process_table[i];
        if (member && member->pgid == pid) {
            spin_unlock(&process_table_lock);
            return -EPERM;
        }
    }
    proc->sid = proc->pgid = pid;
    spin_unlock(&process_table_lock);
    process_ctty_clear(proc);
    if (sid) *sid = pid;
    return 0;
}

bool process_pgrp_is_orphaned(pid_t pgid, pid_t sid)
{
    bool found = false;

    spin_lock(&process_table_lock);
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
        process_t *proc = process_table[i];
        if (!proc || proc->pgid != pgid || proc->sid != sid) continue;
        found             = true;
        process_t *parent = proc->parent;
        if (parent && parent != proc && parent->sid == sid && parent->pgid != pgid) {
            spin_unlock(&process_table_lock);
            return false;
        }
    }
    spin_unlock(&process_table_lock);
    return found;
}

int setup_process_page_dir(process_t *proc)
{
    page_directory_t *new_dir = malloc(sizeof(page_directory_t));
    if (!new_dir) return 1;

    uint64_t pml4_frame = alloc_frames(1);
    if (!pml4_frame) {
        free(new_dir);
        return 1;
    }

    page_table_t *pml4 = (page_table_t *)phys_to_virt(pml4_frame);
    page_table_clear(pml4);
    new_dir->table       = pml4;
    new_dir->lock.lock   = 0;
    new_dir->lock.rflags = 0;

    page_directory_t *kern_dir  = get_kernel_pagedir();
    page_table_t     *kern_pml4 = kern_dir->table;

    for (int i = 256; i < 512; i++) { pml4->entries[i] = kern_pml4->entries[i]; }

    proc->kernel_page_dir      = kern_dir;
    proc->user_page_dir        = new_dir;
    proc->task->page_directory = new_dir;
    return 0;
}

vm_area_t *vm_area_alloc(uintptr_t start, uintptr_t end, vm_flags_t flags)
{
    vm_area_t *vma = calloc(1, sizeof(vm_area_t));
    if (!vma) return NULL;
    vma->start = start;
    vma->end   = end;
    vma->flags = flags;
    vma->type  = VM_REGION_MMAP;
    vma->next  = NULL;
    return vma;
}

int vm_area_insert(process_t *proc, vm_area_t *vma)
{
    spin_lock(&proc->mmap_lock);
    if (!proc->mmap_list) {
        proc->mmap_list = vma;
    } else {
        vm_area_t *prev = NULL;
        for (vm_area_t *cur = proc->mmap_list; cur; cur = cur->next) {
            if (vma->end <= cur->start) break;
            prev = cur;
        }
        if (prev) {
            vma->next  = prev->next;
            prev->next = vma;
        } else {
            vma->next       = proc->mmap_list;
            proc->mmap_list = vma;
        }
    }
    spin_unlock(&proc->mmap_lock);
    return 0;
}

static void vm_area_free(vm_area_t *vma, uint32_t pid)
{
    while (vma) {
        vm_area_t *next = vma->next;
        if (vma->type == VM_REGION_SHM && vma->vm_private_data) sysv_shm_vma_put(vma->vm_private_data, pid);
        if (vma->vm_file) {
            if (vma->vm_pagecache) vfs_cache_mapping_unpin(vma->vm_file);
            memfd_vma_release(vma->vm_file, vma->flags);
            vfs_close(vma->vm_file);
        }
        free(vma);
        vma = next;
    }
}

static void mmap_list_free(process_t *proc, uint32_t pid)
{
    spin_lock(&proc->mmap_lock);
    vm_area_t *list = proc->mmap_list;
    proc->mmap_list = NULL;
    spin_unlock(&proc->mmap_lock);
    vm_area_free(list, pid);
}

void process_mmap_clear(process_t *proc)
{
    mmap_list_free(proc, proc && proc->task ? (uint32_t)proc->task->pid : 0);
}

static void process_fd_table_init(process_t *proc)
{
    proc->fd_lock.lock   = 0;
    proc->fd_lock.rflags = 0;
}

void process_file_get(process_file_t *file)
{
    if (!file) return;

    spin_lock(&file->lock);
    if (file->refcount > 0) file->refcount++;
    spin_unlock(&file->lock);
}

static void process_file_fd_get(process_file_t *file)
{
    if (!file) return;
    spin_lock(&file->lock);
    if (file->refcount > 0) {
        file->refcount++;
        file->fd_refcount++;
    }
    spin_unlock(&file->lock);
}

static void process_file_fd_put(process_file_t *file)
{
    if (!file) return;
    spin_lock(&file->lock);
    if (file->fd_refcount) file->fd_refcount--;
    bool closed = file->fd_refcount == 0 && !file->descriptors_closed;
    if (closed) file->descriptors_closed = true;
    spin_unlock(&file->lock);
    if (closed) {
        spin_lock(&file->close_source.lock);
        file->close_source.closed = true;
        spin_unlock(&file->close_source.lock);
        vfs_poll_source_notify(&file->close_source, UINT32_MAX);
    }
    process_file_put(file);
}

static void process_file_io_lock(process_file_t *file)
{
    for (;;) {
        spin_lock(&file->lock);
        if (!file->io_busy) {
            file->io_busy = true;
            spin_unlock(&file->lock);
            return;
        }

        wait_queue_prepare(&file->io_wait);
        spin_unlock(&file->lock);
        wait_queue_sleep();
    }
}

static void process_file_io_unlock(process_file_t *file)
{
    spin_lock(&file->lock);
    file->io_busy = false;
    spin_unlock(&file->lock);
    wait_queue_wake_one(&file->io_wait);
}

void process_file_put(process_file_t *file)
{
    if (!file) return;

    spin_lock(&file->lock);
    if (file->refcount > 1) {
        file->refcount--;
        spin_unlock(&file->lock);
        return;
    }
    file->refcount = 0;
    spin_unlock(&file->lock);

    inotify_notify(file->node, (file->flags & O_ACCMODE) == O_RDONLY ? IN_CLOSE_NOWRITE : IN_CLOSE_WRITE);

    /* Release per-open-instance private_data. */
    if (file->file_opened) callbackof(file->node, file_release)(file->node, file->private_data);

    vfs_close(file->node);
    free(file);
}

static void process_fd_table_close(process_t *proc)
{
    if (!proc) return;

    spin_lock(&proc->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        process_file_t *file = proc->fds[i];
        proc->fds[i]         = NULL;
        process_file_fd_put(file);
    }
    spin_unlock(&proc->fd_lock);
}

static void process_fd_table_copy(process_t *child, process_t *parent)
{
    process_fd_table_init(child);

    spin_lock(&parent->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        child->fds[i] = parent->fds[i];
        process_file_fd_get(child->fds[i]);
    }
    spin_unlock(&parent->fd_lock);
}

process_file_t *process_fd_get(process_t *proc, int fd)
{
    if (!proc || fd < 0 || fd >= PROCESS_MAX_FD) return NULL;

    spin_lock(&proc->fd_lock);
    process_file_t *file = proc->fds[fd];
    process_file_get(file);
    spin_unlock(&proc->fd_lock);
    return file;
}

int process_fd_install(process_t *proc, vfs_node_t node, uint64_t flags)
{
    if (!proc || !node) return -EINVAL;

    process_file_t *file = calloc(1, sizeof(process_file_t));
    if (!file) return -ENOMEM;

    file->node        = node;
    file->flags       = flags;
    file->refcount    = 1;
    file->fd_refcount = 1;
    file->lock.lock   = 0;
    file->lock.rflags = 0;
    wait_queue_init(&file->io_wait);
    vfs_poll_source_init(&file->close_source);
    if (flags & O_APPEND) file->offset = node->size;

    /* Allocate per-open-instance private_data via the FS callback. */
    {
        void *priv = NULL;
        int   ret  = callbackof(node, file_open)(node, flags, &priv);
        if (ret == 0) {
            file->private_data = priv;
            file->file_opened  = true;
        } else if (ret != -ENOSYS) {
            /* Real error from the callback --abort. */
            free(file);
            return ret;
        }
    }

    spin_lock(&proc->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        if (!proc->fds[i]) {
            proc->fds[i] = file;
            spin_unlock(&proc->fd_lock);
            inotify_notify(node, IN_OPEN);
            return i;
        }
    }
    spin_unlock(&proc->fd_lock);

    /* Failed to find a free FD slot --release private_data. */
    if (file->file_opened) callbackof(node, file_release)(node, file->private_data);
    free(file);
    return -EMFILE;
}

int process_fd_close(process_t *proc, int fd)
{
    if (!proc || fd < 0 || fd >= PROCESS_MAX_FD) return -EBADF;

    spin_lock(&proc->fd_lock);
    process_file_t *file = proc->fds[fd];
    if (!file) {
        spin_unlock(&proc->fd_lock);
        return -EBADF;
    }
    proc->fds[fd] = NULL;
    spin_unlock(&proc->fd_lock);

    process_file_fd_put(file);
    return EOK;
}

int process_fd_dup(process_t *proc, int oldfd)
{
    if (!proc || oldfd < 0 || oldfd >= PROCESS_MAX_FD) return -EBADF;

    spin_lock(&proc->fd_lock);
    process_file_t *file = proc->fds[oldfd];
    if (!file) {
        spin_unlock(&proc->fd_lock);
        return -EBADF;
    }

    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        if (!proc->fds[i]) {
            process_file_fd_get(file);
            proc->fds[i] = file;
            spin_unlock(&proc->fd_lock);
            return i;
        }
    }
    spin_unlock(&proc->fd_lock);
    return -EMFILE;
}

int process_fd_dup2(process_t *proc, int oldfd, int newfd)
{
    if (!proc || oldfd < 0 || oldfd >= PROCESS_MAX_FD || newfd < 0 || newfd >= PROCESS_MAX_FD) return -EBADF;
    if (oldfd == newfd) {
        /* Even when oldfd == newfd, POSIX requires EBADF if oldfd is not open */
        spin_lock(&proc->fd_lock);
        int valid = (proc->fds[oldfd] != NULL);
        spin_unlock(&proc->fd_lock);
        return valid ? oldfd : -EBADF;
    }

    spin_lock(&proc->fd_lock);
    process_file_t *file = proc->fds[oldfd];
    if (!file) {
        spin_unlock(&proc->fd_lock);
        return -EBADF;
    }

    process_file_t *old = proc->fds[newfd];
    process_file_fd_get(file);
    proc->fds[newfd] = file;
    spin_unlock(&proc->fd_lock);

    process_file_fd_put(old);
    return newfd;
}

int64_t process_fd_read(process_t *proc, int fd, void *buf, size_t size)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    bool stream = (file->node->type & file_stream) != 0;
    if (!stream) process_file_io_lock(file);

    spin_lock(&file->lock);
    uint64_t flags  = file->flags;
    size_t   offset = stream ? 0 : file->offset;
    spin_unlock(&file->lock);

    if ((flags & O_ACCMODE) == O_WRONLY) {
        if (!stream) process_file_io_unlock(file);
        process_file_put(file);
        return -EBADF;
    }

    int64_t ret = vfs_file_read(file->node, file->private_data, flags, buf, offset, size);
    if (!stream) {
        if (ret >= 0) {
            spin_lock(&file->lock);
            file->offset = offset + (size_t)ret;
            spin_unlock(&file->lock);
        }
        process_file_io_unlock(file);
    }

    process_file_put(file);
    return ret;
}

int64_t process_fd_write(process_t *proc, int fd, const void *buf, size_t size)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    bool stream = (file->node->type & file_stream) != 0;
    if (!stream) process_file_io_lock(file);

    spin_lock(&file->lock);
    uint64_t flags  = file->flags;
    size_t   offset = stream ? 0 : file->offset;
    if (!stream && (flags & O_APPEND)) offset = file->node->size;
    spin_unlock(&file->lock);

    if ((flags & O_ACCMODE) == O_RDONLY) {
        if (!stream) process_file_io_unlock(file);
        process_file_put(file);
        return -EBADF;
    }

    int64_t ret = vfs_file_write(file->node, file->private_data, flags, buf, offset, size);
    if (!stream) {
        if (ret >= 0) {
            spin_lock(&file->lock);
            file->offset = offset + (size_t)ret;
            spin_unlock(&file->lock);
        }
        process_file_io_unlock(file);
    }

    process_file_put(file);
    return ret;
}

int64_t process_fd_seek(process_t *proc, int fd, int64_t offset, int whence)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    process_file_io_lock(file);
    spin_lock(&file->lock);
    int64_t base;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = (int64_t)file->offset;
    } else if (whence == SEEK_END) {
        base = (int64_t)file->node->size;
    } else {
        spin_unlock(&file->lock);
        process_file_io_unlock(file);
        process_file_put(file);
        return -EINVAL;
    }

    int64_t next = base + offset;
    if (next < 0) {
        spin_unlock(&file->lock);
        process_file_io_unlock(file);
        process_file_put(file);
        return -EINVAL;
    }
    file->offset = (size_t)next;
    spin_unlock(&file->lock);
    process_file_io_unlock(file);

    process_file_put(file);
    return next;
}

int process_fd_ioctl(process_t *proc, int fd, size_t req, void *arg)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;
    int ret = vfs_file_ioctl(file->node, file->private_data, file->flags, req, arg);
    process_file_put(file);
    return ret;
}

int process_fd_poll(process_t *proc, int fd, size_t events)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;
    int ret = vfs_file_poll(file->node, file->private_data, file->flags, events);
    process_file_put(file);
    return ret;
}

int process_resolve_path_at(process_t *proc, int dirfd, const char *path, char *resolved, size_t size)
{
    char        base[VFS_PATH_MAX];
    char        root[VFS_PATH_MAX];
    const char *process_root;
    int         ret;

    if (!proc || !path || !resolved) return -EINVAL;
    if (!path[0]) return -ENOENT;

    process_root = proc->root[0] ? proc->root : "/";
    ret          = vfs_resolve_path("/", process_root, root, sizeof(root));
    if (ret != EOK) return ret;

    if (path[0] == '/') {
        while (*path == '/') path++;
        ret = vfs_resolve_path(root, path, resolved, size);
    } else if (dirfd == PROCESS_AT_FDCWD) {
        const char *cwd = proc->cwd[0] ? proc->cwd : root;
        ret             = vfs_resolve_path(cwd, path, resolved, size);
    } else {
        process_file_t *file = process_fd_get(proc, dirfd);
        if (!file) return -EBADF;
        if (!(file->node->type & file_dir)) {
            process_file_put(file);
            return -ENOTDIR;
        }
        ret = vfs_node_path(file->node, base, sizeof(base));
        process_file_put(file);
        if (ret == EOK) ret = vfs_resolve_path(base, path, resolved, size);
    }

    if (ret != EOK) return ret;

    size_t root_len = strlen(root);
    if (root_len != 1 && (strncmp(resolved, root, root_len) || (resolved[root_len] && resolved[root_len] != '/'))) return -EPERM;
    return EOK;
}

int process_file_poll(process_file_t *file, size_t events)
{
    if (!file) return -EBADF;
    return vfs_file_poll(file->node, file->private_data, file->flags, events);
}

int process_fd_stat(process_t *proc, int fd, process_fd_stat_t *stat)
{
    if (!stat) return -EINVAL;

    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    vfs_update(file->node);
    stat->dev   = file->node->dev;
    stat->inode = file->node->inode;
    stat->mode  = file->node->mode;
    stat->type  = file->node->type;
    stat->rdev  = file->node->rdev;
    stat->size  = file->node->size;
    stat->blksz = file->node->blksz;

    process_file_put(file);
    return EOK;
}

static void process_free(process_t *proc)
{
    if (!proc) return;

    task_t  *task = proc->task;
    uint32_t pid  = task ? (uint32_t)task->tgid : 0;
    proc->task    = NULL;

    process_ctty_clear(proc);
    process_fd_table_close(proc);
    signal_state_free(&proc->signal);
    if (proc->user_page_dir) {
        page_destroy_user_space(proc->user_page_dir);
        free(proc->user_page_dir);
    }
    mmap_list_free(proc, pid);
    if (task && task->kernel_stack == proc->kernel_stack) task->kernel_stack = NULL;
    free(proc->kernel_stack);
    slist_destroy(&proc->children, NULL);
    ilist_node_t *node = proc->threads.next;
    while (node != &proc->threads) {
        ilist_node_t *next   = node->next;
        task_t       *member = rb_entry(node, task_t, thread_node);
        member->process      = NULL;
        task_free(member);
        node = next;
    }
    free(proc);
}

void process_init(void)
{
    process_table_lock.lock   = 0;
    process_table_lock.rflags = 0;
    plogk("process: Process table initialized (%u slots)\n", PROCESS_TABLE_SIZE);
}

process_t *process_create(const char *name, void (*entry)(void *), void *arg)
{
    (void)entry;
    (void)arg;

    process_t *proc = calloc(1, sizeof(process_t));
    if (!proc) return NULL;

    task_t *task = task_alloc(name);
    if (!task) {
        free(proc);
        return NULL;
    }

    proc->task         = task;
    task->process      = proc;
    proc->refcount     = 1;
    proc->thread_count = 1;
    ilist_init(&proc->threads);
    ilist_insert_before(&proc->threads, &task->thread_node);
    proc->kernel_page_dir = get_kernel_pagedir();
    proc->kernel_stack    = malloc(PROCESS_KERNEL_STACK);
    if (!proc->kernel_stack) {
        task_free(task);
        free(proc);
        return NULL;
    }

    if (setup_process_page_dir(proc)) {
        free(proc->kernel_stack);
        task_free(task);
        free(proc);
        return NULL;
    }

    proc->task->state = TASK_RUNNING;
    proc->uid         = 1000;
    proc->gid         = 1000;
    proc->umask       = 022;
    proc->pgid        = 0;
    proc->sid         = 0;
    proc->heap_brk    = PROCESS_HEAP_START;
    proc->stack_brk   = PROCESS_STACK_BASE - PROCESS_STACK_SIZE;
    proc->parent      = init_process;
    proc->exit_code   = 0;
    slist_init(&proc->children);
    proc->mmap_lock.lock   = 0;
    proc->mmap_lock.rflags = 0;
    process_fd_table_init(proc);
    signal_state_init(&proc->signal);
    task_name_copy(task, name);

    strncpy(proc->name, name ? name : "user", PROCESS_NAME_LEN - 1);
    proc->name[PROCESS_NAME_LEN - 1] = '\0';

    pid_set(proc->task->pid, proc);

    if (proc->parent && proc->parent != proc) { slist_insert_tail(&proc->parent->children, proc); }

    plogk("process: Created user process skeleton %llu (%s)\n", proc->task->pid, proc->task->name);
    return proc;
}

process_t *process_create_kernel(const char *name, void (*entry)(void *), void *arg)
{
    process_t *proc = calloc(1, sizeof(process_t));
    if (!proc) return NULL;

    proc->kernel_stack = malloc(PROCESS_KERNEL_STACK);
    if (!proc->kernel_stack) {
        free(proc);
        return NULL;
    }

    task_t *task;
    if (entry) {
        task = kthread_create(name, (kthread_entry_t)entry, arg);
    } else {
        task = task_alloc(name);
        if (!task) {
            free(proc->kernel_stack);
            free(proc);
            return NULL;
        }
    }
    if (!task) {
        free(proc->kernel_stack);
        free(proc);
        return NULL;
    }

    proc->task         = task;
    task->process      = proc;
    proc->refcount     = 1;
    proc->thread_count = 1;
    ilist_init(&proc->threads);
    ilist_insert_before(&proc->threads, &task->thread_node);
    proc->kernel_page_dir = get_kernel_pagedir();
    proc->user_page_dir   = NULL;

    proc->task->state = TASK_READY;
    proc->uid         = 0;
    proc->gid         = 0;
    proc->umask       = 022;
    proc->pgid        = 0;
    proc->sid         = 0;
    proc->heap_brk    = 0;
    proc->stack_brk   = 0;
    proc->parent      = init_process;
    proc->exit_code   = 0;
    slist_init(&proc->children);
    proc->mmap_lock.lock   = 0;
    proc->mmap_lock.rflags = 0;
    process_fd_table_init(proc);
    signal_state_init(&proc->signal);
    task_name_copy(task, name);
    strncpy(proc->name, name ? name : "kthread", PROCESS_NAME_LEN - 1);
    proc->name[PROCESS_NAME_LEN - 1] = '\0';

    pid_set(proc->task->pid, proc);

    if (proc->parent && proc->parent != proc) { slist_insert_tail(&proc->parent->children, proc); }

    plogk("process: Created kernel thread %llu (%s)\n", proc->task->pid, proc->task->name);
    return proc;
}

void process_exit(int exit_code)
{
    task_t *current = current_task();
    if (!current || !current->process) {
        plogk("process: process_exit called from non-process context.\n");
        task_exit();
        return;
    }

    process_t *proc = current->process;
    if (proc == init_process) panic("init: Attempt to kill init!");

    ptrace_exit_event(exit_code);
    ptrace_tracer_exit((int64_t)current->pid);

    spin_lock(&process_table_lock);
    bool sibling_exit = proc->thread_count > 1;
    if (sibling_exit) {
        proc->thread_count--;
        current->state = TASK_ZOMBIE;
        if (proc->task == current) {
            for (ilist_node_t *node = proc->threads.next; node != &proc->threads; node = node->next) {
                task_t *member = rb_entry(node, task_t, thread_node);
                if (member != current && member->state != TASK_ZOMBIE) {
                    proc->task = member;
                    break;
                }
            }
        }
    }
    spin_unlock(&process_table_lock);

    if (sibling_exit) {
        if (current->clear_child_tid) {
            uint32_t zero = 0;
            copy_to_user((void *)current->clear_child_tid, &zero, sizeof(zero));
            sys_futex((uint32_t *)current->clear_child_tid, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, NULL, 0);
        }
        ptrace_exit_notify(exit_code);
        task_exit();
        return;
    }

    tty_core_t *tty = process_ctty_get(proc);
    if (tty) {
        pid_t sid  = proc->sid;
        pid_t pgid = sid == (pid_t)current->tgid ? process_ctty_disassociate(tty, sid) : -1;
        if (pgid < 0) process_ctty_clear(proc);
        if (pgid > 0) {
            signal_send_pgrp_session(pgid, sid, SIGHUP);
            signal_send_pgrp_session(pgid, sid, SIGCONT);
        }
        tty_core_release(tty);
    }

    disable_intr();
    spin_lock(&scheduler.lock);
    spin_lock(&process_table_lock);

    proc->task->state = TASK_ZOMBIE;
    proc->exit_code   = exit_code;

    /* Take a reference on parent before releasing locks (prevent use-after-free) */
    process_t *parent = proc->parent;
    if (parent) process_get_locked(parent);

    slist_node_t *node = proc->children.head;
    while (node) {
        slist_node_t *next  = node->next;
        process_t    *child = (process_t *)node->data;
        slist_remove(&proc->children, child);
        if (child && child != proc && child->task->state != TASK_ZOMBIE) {
            child->parent = init_process;
            slist_insert_tail(&init_process->children, child);
        }
        node = next;
    }

    spin_unlock(&process_table_lock);
    spin_unlock(&scheduler.lock);

    /* Notify parent via SIGCHLD (outside the locks, because signal_notify_child_exit
     * calls task_wakeup which acquires scheduler.lock, and we have just released it). */
    if (parent) {
        signal_notify_child_exit(parent, (pid_t)current->tgid, exit_code, 0);
        process_put(parent);
    }

    /* Notify set_tid_address with 0 (futex wake on clear_child_tid) */
    if (current->clear_child_tid) {
        uint32_t zero = 0;
        copy_to_user((void *)current->clear_child_tid, &zero, sizeof(zero));
        sys_futex((uint32_t *)current->clear_child_tid, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, NULL, 0);
    }

    process_fd_table_close(proc);

    ptrace_exit_notify(exit_code);

    plogk("process: Process %llu (%s) exited with code %d\n", proc->task->pid, proc->task->name, exit_code);

    proc->task->state = TASK_ZOMBIE;
    task_exit();
}

int process_wait(pid_t pid, int *exit_code)
{
    if (!init_process) return 1;

    disable_intr();
    spin_lock(&scheduler.lock);
    spin_lock(&process_table_lock);

    process_t *child = pid_to_process_locked(pid);
    if (!child || child->parent != current_task()->process) {
        spin_unlock(&process_table_lock);
        spin_unlock(&scheduler.lock);
        return 1;
    }

    for (;;) {
        task_t *child_task    = child->task;
        bool    group_stopped = child_task->state == TASK_ZOMBIE;
        for (ilist_node_t *node = child->threads.next; group_stopped && node != &child->threads; node = node->next) {
            task_t *member = rb_entry(node, task_t, thread_node);
            if (member->state != TASK_ZOMBIE || __atomic_load_n(&member->on_cpu, __ATOMIC_ACQUIRE)) group_stopped = false;
        }
        /* A real parent cannot reap a zombie until its ptrace tracer has
         * consumed the final wait status and released the tracee. */
        if (group_stopped && ptrace_tracer_pid(child_task)) group_stopped = false;
        if (group_stopped) break;

        /*
         * Check for any pending signals that should interrupt the wait.
         * A non-SIGCHLD signal (e.g., SIGINT, SIGTERM) should cause
         * wait4 to return -ERESTARTSYS so the kernel can either restart
         * it or convert to -EINTR after signal delivery.
         */
        process_t *parent = current_task()->process;
        if (parent) {
            signal_state_t *ss = &parent->signal;
            spin_lock(&ss->lock);
            bool has_signal = signal_has_pending(ss);
            spin_unlock(&ss->lock);
            if (has_signal) {
                spin_unlock(&process_table_lock);
                spin_unlock(&scheduler.lock);
                return -ERESTARTSYS;
            }
        }

        /*
         * Release locks and block until woken (by child exit or signal).
         * We use task_block() rather than task_sleep_ticks() so we
         * don't waste CPU polling. signal_notify_child_exit() and
         * signal_send() both call task_wakeup() which will wake us.
         */
        spin_unlock(&process_table_lock);
        spin_unlock(&scheduler.lock);
        task_block();
        spin_lock(&scheduler.lock);
        spin_lock(&process_table_lock);

        child = pid_to_process_locked(pid);
        if (!child || child->parent != current_task()->process) {
            spin_unlock(&process_table_lock);
            spin_unlock(&scheduler.lock);
            return 1;
        }
    }

    if (exit_code) *exit_code = child->exit_code;

    pid_set_locked(child->task->tgid, NULL);
    slist_remove(&child->parent->children, child);
    process_t *saved_child = child;

    spin_unlock(&process_table_lock);
    spin_unlock(&scheduler.lock);

    process_put(saved_child);
    return 0;
}

int process_kill(pid_t pid)
{
    process_t *proc = process_find_get(pid);
    if (!proc) return 1;
    if (proc->task->state == TASK_ZOMBIE) {
        process_put(proc);
        return 1;
    }
    if (proc == init_process) panic("Attempt to kill init!");

    process_t *cur = process_current();
    if (cur && cur->uid != 0 && cur->uid != proc->uid) {
        process_put(proc);
        return -EPERM;
    }

    int ret = signal_send(proc, SIGKILL, NULL);
    process_put(proc);
    return ret;
}

process_t *process_find(pid_t pid)
{
    return pid_to_process(pid);
}

process_t *process_iterate(size_t *pos)
{
    if (!pos) return NULL;

    spin_lock(&process_table_lock);
    for (; *pos < PROCESS_TABLE_SIZE; (*pos)++) {
        process_t *proc = process_table[*pos];
        if (proc) {
            (*pos)++;
            spin_unlock(&process_table_lock);
            return proc;
        }
    }
    spin_unlock(&process_table_lock);
    return NULL;
}

process_t *process_current(void)
{
    task_t *task = current_task();
    return task ? task->process : NULL;
}

process_t *process_fork_status_event(int *error, uint32_t ptrace_event)
{
    task_t    *current = current_task();
    process_t *parent  = current ? current->process : NULL;
    if (error) *error = EOK;
    if (!parent || parent->task->state == TASK_ZOMBIE) {
        if (error) *error = -ESRCH;
        return NULL;
    }

    disable_intr();
    spin_lock(&scheduler.lock);
    spin_lock(&parent->mmap_lock);

    process_t *child = calloc(1, sizeof(process_t));
    if (!child) {
        if (error) *error = -ENOMEM;
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    int     task_error = EOK;
    task_t *child_task = task_alloc_status(parent->task->name, &task_error);
    if (!child_task) {
        if (error) *error = task_error;
        free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    child->task         = child_task;
    child_task->process = child;
    child->refcount     = 1;
    child->thread_count = 1;
    ilist_init(&child->threads);
    ilist_insert_before(&child->threads, &child_task->thread_node);
    child->task->state = TASK_READY;
    child->uid         = parent->uid;
    child->gid         = parent->gid;
    child->umask       = parent->umask;
    child->pgid        = parent->pgid;
    child->sid         = parent->sid;
    child->parent      = parent;
    child->exit_code   = 0;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    child->name[sizeof(child->name) - 1] = '\0';
    child->heap_brk                      = parent->heap_brk;
    child->stack_brk                     = parent->stack_brk;
    memcpy(child->root, parent->root, sizeof(child->root));
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->kernel_stack = malloc(PROCESS_KERNEL_STACK);
    if (!child->kernel_stack) {
        if (error) *error = -ENOMEM;
        task_free(child_task);
        free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }
    child->mmap_lock.lock   = 0;
    child->mmap_lock.rflags = 0;
    strncpy(child->name, parent->name, PROCESS_NAME_LEN - 1);
    child->name[PROCESS_NAME_LEN - 1] = '\0';
    process_fd_table_copy(child, parent);
    signal_state_copy(&child->signal, &parent->signal);
    process_ctty_inherit(child, parent);
    slist_init(&child->children);

    if (setup_process_page_dir(child)) {
        if (error) *error = -ENOMEM;
        process_free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    if (page_clone_user_cow(child->user_page_dir, parent->user_page_dir)) {
        if (error) *error = -ENOMEM;
        process_free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    for (vm_area_t *vma = parent->mmap_list; vma; vma = vma->next) {
        vm_area_t *copy = vm_area_alloc(vma->start, vma->end, vma->flags);
        if (!copy) {
            if (error) *error = -ENOMEM;
            process_free(child);
            spin_unlock(&parent->mmap_lock);
            spin_unlock(&scheduler.lock);
            return NULL;
        }
        copy->type            = vma->type;
        copy->vm_file         = vma->vm_file;
        copy->vm_pgoff        = vma->vm_pgoff;
        copy->vm_private_data = vma->vm_private_data;
        copy->vm_pagecache    = vma->vm_pagecache;

        /* Bump the file reference if this VMA is file-backed.
         * The parent already holds a reference; the child needs
         * its own so the file isn't freed while the child lives. */
        if (copy->vm_file) copy->vm_file->refcount++;
        if (copy->vm_file && copy->vm_pagecache) (void)vfs_cache_mapping_pin(copy->vm_file);
        if (copy->vm_file) memfd_vma_retain(copy->vm_file, copy->flags);
        if (copy->type == VM_REGION_SHM && sysv_shm_vma_get(copy->vm_private_data, (uint32_t)child->task->pid)) {
            free(copy);
            if (error) *error = -ENOMEM;
            process_free(child);
            spin_unlock(&parent->mmap_lock);
            spin_unlock(&scheduler.lock);
            return NULL;
        }

        vm_area_insert(child, copy);
    }

    memcpy(&child_task->context, &current->context, sizeof(task_context_t));
    child_task->thread.fs_base = current->thread.fs_base;
    child_task->thread.gs_base = current->thread.gs_base;
    fpu_task_clone(current, child_task);

    child_task->cpu_id = current->cpu_id;

    pid_set(child->task->pid, child);

    slist_insert_tail(&parent->children, child);

    bool ptrace_stopped = ptrace_fork_child(current, child_task, ptrace_event);
    if (!ptrace_stopped) enqueue_task(child_task);

    spin_unlock(&parent->mmap_lock);
    spin_unlock(&scheduler.lock);
    request_task_cpu(child_task);

    // plogk("process: Forked process %llu from parent %llu\n", child->task->pid, parent->task->pid); it is very noisy
    return child;
}

task_t *process_task_find_get(pid_t pid, process_t **owner)
{
    if (owner) *owner = NULL;
    if (pid <= 0) return NULL;

    spin_lock(&process_table_lock);
    for (size_t index = 1; index < PROCESS_TABLE_SIZE; index++) {
        process_t *proc = process_table[index];
        if (!proc) continue;
        for (ilist_node_t *node = proc->threads.next; node != &proc->threads; node = node->next) {
            task_t *task = rb_entry(node, task_t, thread_node);
            if ((pid_t)task->pid != pid) continue;
            process_get_locked(proc);
            if (owner) *owner = proc;
            spin_unlock(&process_table_lock);
            return task;
        }
    }
    spin_unlock(&process_table_lock);
    return NULL;
}

process_t *process_fork_status(int *error)
{
    return process_fork_status_event(error, PTRACE_EVENT_FORK);
}

process_t *process_fork(void)
{
    return process_fork_status(NULL);
}

process_t *process_fork_from_syscall(syscall_frame_t *frame)
{
    task_t    *current = current_task();
    process_t *child   = process_fork();

    if (!child || !frame || !current || !current->process) return child;

    uint64_t  kstack_top = (uint64_t)(child->kernel_stack + PROCESS_KERNEL_STACK);
    uint64_t *kstack     = (uint64_t *)ALIGN_DOWN(kstack_top, 16ULL);

    syscall_frame_t child_frame = *frame;
    child_frame.rax             = 0;

    kstack -= sizeof(syscall_frame_t) / sizeof(uint64_t);
    memcpy(kstack, &child_frame, sizeof(syscall_frame_t));
    *(--kstack)              = (uint64_t)syscall_return;
    child->task->context.rsp = (uint64_t)kstack;
    return child;
}

task_t *process_clone_thread(syscall_frame_t *frame, uintptr_t child_stack, uintptr_t parent_tid, uintptr_t child_set_tid,
                             uintptr_t child_clear_tid, uintptr_t tls, int *error)
{
    task_t    *current = current_task();
    process_t *proc    = current ? current->process : NULL;
    if (error) *error = EOK;
    if (!frame || !child_stack || !proc || current->state == TASK_ZOMBIE) {
        if (error) *error = !child_stack || !frame ? -EINVAL : -ESRCH;
        return NULL;
    }
    if (user_access_ok((void *)(child_stack - 1), 1, 1) || (parent_tid && user_access_ok((void *)parent_tid, sizeof(uint32_t), 1))
        || (child_set_tid && user_access_ok((void *)child_set_tid, sizeof(uint32_t), 1))
        || (child_clear_tid && user_access_ok((void *)child_clear_tid, sizeof(uint32_t), 1))) {
        if (error) *error = -EFAULT;
        return NULL;
    }

    int     task_error = EOK;
    task_t *child      = task_alloc_status(current->name, &task_error);
    if (!child) {
        if (error) *error = task_error;
        return NULL;
    }
    child->kernel_stack = malloc(TASK_KERNEL_STACK);
    if (!child->kernel_stack) {
        task_free(child);
        if (error) *error = -ENOMEM;
        return NULL;
    }

    uint32_t tid = (uint32_t)child->pid;
    if ((child_set_tid && copy_to_user((void *)child_set_tid, &tid, sizeof(tid)))
        || (parent_tid && copy_to_user((void *)parent_tid, &tid, sizeof(tid)))) {
        task_free(child);
        if (error) *error = -EFAULT;
        return NULL;
    }

    syscall_frame_t child_frame = *frame;
    child_frame.rax             = 0;
    child_frame.rsp             = child_stack;
    uint64_t *kstack            = (uint64_t *)ALIGN_DOWN((uint64_t)(child->kernel_stack + TASK_KERNEL_STACK), 16ULL);
    kstack -= sizeof(child_frame) / sizeof(uint64_t);
    memcpy(kstack, &child_frame, sizeof(child_frame));
    *(--kstack) = (uint64_t)syscall_return;

    child->context.rsp    = (uint64_t)kstack;
    child->thread.fs_base = tls;
    child->thread.gs_base = current->thread.gs_base;
    fpu_task_clone(current, child);
    child->page_directory  = proc->user_page_dir;
    child->process         = proc;
    child->tgid            = proc->task->tgid;
    child->clear_child_tid = child_clear_tid;
    child->cpu_id          = current->cpu_id;
    child->state           = TASK_READY;

    disable_intr();
    spin_lock(&scheduler.lock);
    if (proc->kernel_stack) {
        proc->task->kernel_stack = proc->kernel_stack;
        proc->kernel_stack       = NULL;
    }
    spin_lock(&process_table_lock);
    ilist_insert_before(&proc->threads, &child->thread_node);
    proc->thread_count++;
    spin_unlock(&process_table_lock);
    bool ptrace_stopped = ptrace_fork_child(current, child, PTRACE_EVENT_CLONE);
    if (!ptrace_stopped) enqueue_task(child);
    spin_unlock(&scheduler.lock);
    request_task_cpu(child);
    return child;
}

pid_t process_next_pid(void)
{
    return (pid_t)task_next_pid();
}

int process_mmap(process_t *proc, uintptr_t addr, size_t length, vm_flags_t flags)
{
    if (!proc || !length) return 1;
    size_t pages = ALIGN_UP(length, PAGE_4K_SIZE) / PAGE_4K_SIZE;
    for (size_t i = 0; i < pages; i++) {
        uint64_t frame = alloc_frames(1);
        if (!frame) return 1;
        uint64_t pte_flags = PTE_USER | PTE_PRESENT;
        if (flags & VM_WRITE) pte_flags |= PTE_WRITEABLE;
        if (flags & VM_SHARED) pte_flags |= PTE_SHARED;
        if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;
        page_map_to(proc->user_page_dir, addr + i * PAGE_4K_SIZE, frame, pte_flags);
    }
    vm_area_t *vma = vm_area_alloc(addr, addr + pages * PAGE_4K_SIZE, flags);
    if (!vma) return 1;
    vma->type = VM_REGION_MMAP;
    vm_area_insert(proc, vma);
    return 0;
}

int process_munmap(process_t *proc, uintptr_t addr, size_t length)
{
    if (!proc || !length) return -EINVAL;

    spin_lock(&proc->mmap_lock);
    uintptr_t end = 0;
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (vma->start == addr) {
            end = vma->end;
            break;
        }
    }
    spin_unlock(&proc->mmap_lock);

    if (!end) return -ENOENT;
    return process_unmap_complete_range(proc, addr, end - addr);
}

int process_unmap_complete_range(process_t *proc, uintptr_t addr, size_t length)
{
    if (!proc || !length || addr > UINT64_MAX - length) return -EINVAL;
    uintptr_t end = addr + length;

    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (addr < vma->end && end > vma->start && (vma->start < addr || vma->end > end)) {
            spin_unlock(&proc->mmap_lock);
            return -EINVAL;
        }
    }
    spin_unlock(&proc->mmap_lock);

    for (uintptr_t va = addr; va < end; va += PAGE_4K_SIZE) {
        if (page_unmap_release(proc->user_page_dir, va) < 0) return -ENOMEM;
    }

    vm_area_t *removed = NULL;
    spin_lock(&proc->mmap_lock);
    vm_area_t **prev = &proc->mmap_list;
    while (*prev) {
        vm_area_t *vma = *prev;
        if (vma->start >= addr && vma->end <= end) {
            *prev     = vma->next;
            vma->next = removed;
            removed   = vma;
            continue;
        }
        prev = &vma->next;
    }
    spin_unlock(&proc->mmap_lock);
    vm_area_free(removed, proc->task ? (uint32_t)proc->task->pid : 0);
    return EOK;
}
