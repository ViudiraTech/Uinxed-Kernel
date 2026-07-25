/*
 *
 *      process.c
 *      Process management
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smp.h>
#include <chipset/common.h>
#include <drivers/tty_core.h>
#include <fs/vfs.h>
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
#include <proc/sched.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>
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
    pid_t target_pid = (pid_t)target->task->pid;
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

    pid_t pid = (pid_t)proc->task->pid;
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
    new_dir->table = pml4;

    page_directory_t *kern_dir  = get_kernel_pagedir();
    page_table_t     *kern_pml4 = kern_dir->table;

    for (int i = 256; i < 512; i++) { pml4->entries[i] = kern_pml4->entries[i]; }

    proc->kernel_page_dir      = kern_dir;
    proc->user_page_dir        = new_dir;
    proc->task->page_directory = new_dir;
    return 0;
}

static int clone_parent_mappings(process_t *child, const process_t *parent)
{
    page_table_t *src_pml4 = parent->user_page_dir->table;
    page_table_t *dst_pml4 = child->user_page_dir->table;

    for (int l4i = 0; l4i < 256; l4i++) {
        uint64_t l4e = src_pml4->entries[l4i].value;
        if (!(l4e & PTE_PRESENT)) continue;

        if (l4e & PTE_HUGE) {
            uint64_t frame = alloc_frames(512);
            if (!frame) return 1;
            memcpy(phys_to_virt(frame), phys_to_virt(l4e & PAGE_4K_MASK), PAGE_1G_SIZE);
            dst_pml4->entries[l4i].value = frame | (l4e & 0xFFFULL);
            continue;
        }

        page_table_t *src_l3   = (page_table_t *)phys_to_virt(l4e & PAGE_4K_MASK);
        uint64_t      l3_frame = alloc_frames(1);
        if (!l3_frame) return 1;
        page_table_t *dst_l3 = (page_table_t *)phys_to_virt(l3_frame);
        page_table_clear(dst_l3);
        dst_pml4->entries[l4i].value = l3_frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;

        for (int l3i = 0; l3i < 512; l3i++) {
            uint64_t l3e = src_l3->entries[l3i].value;
            if (!(l3e & PTE_PRESENT)) continue;

            if (l3e & PTE_HUGE) {
                uint64_t frame = alloc_frames(512);
                if (!frame) return 1;
                memcpy(phys_to_virt(frame), phys_to_virt(l3e & PAGE_4K_MASK), PAGE_2M_SIZE * 512);
                dst_l3->entries[l3i].value = frame | (l3e & 0xFFFULL);
                continue;
            }

            page_table_t *src_l2   = (page_table_t *)phys_to_virt(l3e & PAGE_4K_MASK);
            uint64_t      l2_frame = alloc_frames(1);
            if (!l2_frame) return 1;
            page_table_t *dst_l2 = (page_table_t *)phys_to_virt(l2_frame);
            page_table_clear(dst_l2);
            dst_l3->entries[l3i].value = l2_frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;

            for (int l2i = 0; l2i < 512; l2i++) {
                uint64_t l2e = src_l2->entries[l2i].value;
                if (!(l2e & PTE_PRESENT)) continue;
                uint64_t frame = alloc_frames(1);
                if (!frame) return 1;
                memcpy(phys_to_virt(frame), phys_to_virt(l2e & PAGE_4K_MASK), PAGE_4K_SIZE);
                dst_l2->entries[l2i].value = frame | (l2e & 0xFFFULL);
            }
        }
    }
    return 0;
}

vm_area_t *vm_area_alloc(uintptr_t start, uintptr_t end, vm_flags_t flags)
{
    vm_area_t *vma = malloc(sizeof(vm_area_t));
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

static void vm_area_free(vm_area_t *vma)
{
    while (vma) {
        vm_area_t *next = vma->next;
        free(vma);
        vma = next;
    }
}

static void mmap_list_free(process_t *proc)
{
    spin_lock(&proc->mmap_lock);
    vm_area_free(proc->mmap_list);
    proc->mmap_list = NULL;
    spin_unlock(&proc->mmap_lock);
}

void process_mmap_clear(process_t *proc)
{
    mmap_list_free(proc);
}

static void process_fd_table_init(process_t *proc)
{
    proc->fd_lock.lock   = 0;
    proc->fd_lock.rflags = 0;
}

static void process_file_get(process_file_t *file)
{
    if (!file) return;

    spin_lock(&file->lock);
    if (file->refcount > 0) file->refcount++;
    spin_unlock(&file->lock);
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
        process_file_put(file);
    }
    spin_unlock(&proc->fd_lock);
}

static void process_fd_table_copy(process_t *child, process_t *parent)
{
    process_fd_table_init(child);

    spin_lock(&parent->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        child->fds[i] = parent->fds[i];
        process_file_get(child->fds[i]);
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
    file->lock.lock   = 0;
    file->lock.rflags = 0;
    wait_queue_init(&file->io_wait);
    if (flags & O_APPEND) file->offset = node->size;

    /* Allocate per-open-instance private_data via the FS callback. */
    {
        void *priv = NULL;
        int   ret  = callbackof(node, file_open)(node, flags, &priv);
        if (ret == 0) {
            file->private_data = priv;
            file->file_opened  = true;
        } else if (ret != -ENOSYS) {
            /* Real error from the callback — abort. */
            free(file);
            return ret;
        }
    }

    spin_lock(&proc->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        if (!proc->fds[i]) {
            proc->fds[i] = file;
            spin_unlock(&proc->fd_lock);
            return i;
        }
    }
    spin_unlock(&proc->fd_lock);

    /* Failed to find a free FD slot — release private_data. */
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

    process_file_put(file);
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
            process_file_get(file);
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
    if (oldfd == newfd) return oldfd;

    spin_lock(&proc->fd_lock);
    process_file_t *file = proc->fds[oldfd];
    if (!file) {
        spin_unlock(&proc->fd_lock);
        return -EBADF;
    }

    process_file_t *old = proc->fds[newfd];
    process_file_get(file);
    proc->fds[newfd] = file;
    spin_unlock(&proc->fd_lock);

    process_file_put(old);
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

    task_t *task = proc->task;
    proc->task   = NULL;
    if (task) task->process = NULL;

    process_ctty_clear(proc);
    process_fd_table_close(proc);
    signal_state_free(&proc->signal);
    if (proc->user_page_dir) {
        free_page_table_recursive(proc->user_page_dir->table, 4);
        free(proc->user_page_dir);
    }
    mmap_list_free(proc);
    free(proc->kernel_stack);
    slist_destroy(&proc->children, NULL);
    free(proc);
    task_free(task);
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

    proc->task            = task;
    task->process         = proc;
    proc->refcount        = 1;
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

    proc->task            = task;
    task->process         = proc;
    proc->refcount        = 1;
    proc->kernel_page_dir = get_kernel_pagedir();
    proc->user_page_dir   = NULL;

    proc->task->state = TASK_READY;
    proc->uid         = 0;
    proc->gid         = 0;
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

    tty_core_t *tty = process_ctty_get(proc);
    if (tty) {
        pid_t sid  = proc->sid;
        pid_t pgid = sid == (pid_t)proc->task->pid ? process_ctty_disassociate(tty, sid) : -1;
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

    /* Notify parent via SIGCHLD */
    if (proc->parent) { signal_notify_child_exit(proc->parent, (pid_t)proc->task->pid, exit_code, 0); }

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

    /* Notify set_tid_address with 0 (futex wake on clear_child_tid) */
    if (proc->clear_child_tid) {
        uint64_t tid = 0;
        copy_to_user((void *)proc->clear_child_tid, &tid, sizeof(tid));
    }

    process_fd_table_close(proc);

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
        task_t *child_task = child->task;
        if (child_task->state == TASK_ZOMBIE && (child_task->cpu_id >= cpu_scheduler_count || cpu_rqs[child_task->cpu_id].curr != child_task)) {
            break;
        }
        spin_unlock(&process_table_lock);
        spin_unlock(&scheduler.lock);
        task_sleep_ticks(1);
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

    pid_set_locked(child->task->pid, NULL);
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

process_t *process_fork_status(int *error)
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
    child->task->state  = TASK_READY;
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->pgid         = parent->pgid;
    child->sid          = parent->sid;
    child->parent       = parent;
    child->exit_code    = 0;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    child->name[sizeof(child->name) - 1] = '\0';
    child->heap_brk                      = parent->heap_brk;
    child->stack_brk                     = parent->stack_brk;
    child->kernel_stack                  = malloc(PROCESS_KERNEL_STACK);
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

    if (clone_parent_mappings(child, parent)) {
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

        /* Bump the file reference if this VMA is file-backed.
         * The parent already holds a reference; the child needs
         * its own so the file isn't freed while the child lives. */
        if (copy->vm_file) copy->vm_file->refcount++;

        vm_area_insert(child, copy);
    }

    memcpy(&child_task->context, &current->context, sizeof(task_context_t));
    child_task->thread.fs_base = current->thread.fs_base;
    child_task->thread.gs_base = current->thread.gs_base;

    child_task->cpu_id = current->cpu_id;

    pid_set(child->task->pid, child);

    slist_insert_tail(&parent->children, child);

    enqueue_task(child_task);

    spin_unlock(&parent->mmap_lock);
    spin_unlock(&scheduler.lock);
    request_task_cpu(child_task);

    plogk("process: Forked process %llu from parent %llu\n", child->task->pid, parent->task->pid);
    return child;
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
    vm_area_t *found = NULL;

    if (!proc || !length) return -EINVAL;

    /* Remove the VMA covering @addr from the list. */
    spin_lock(&proc->mmap_lock);
    {
        vm_area_t **prev = &proc->mmap_list;
        while (*prev) {
            vm_area_t *vma = *prev;
            if (vma->start == addr) {
                *prev = vma->next;
                found = vma;
                break;
            }
            prev = &vma->next;
        }
    }
    spin_unlock(&proc->mmap_lock);

    if (!found) return -ENOENT;

    /* Release file reference if this VMA is file-backed. */
    if (found->vm_file) vfs_close(found->vm_file);

    free(found);
    return 0;
}
