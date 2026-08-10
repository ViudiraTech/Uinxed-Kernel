/*
 *
 *      process.c
 *      Process management
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/fpu.h>
#include <arch/smp.h>
#include <chipset/common.h>
#include <drivers/tty/tty_core.h>
#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <ipc/epoll.h>
#include <ipc/futex.h>
#include <ipc/pipe.h>
#include <ipc/posix_mq.h>
#include <ipc/socket.h>
#include <ipc/sysv_ipc.h>
#include <kernel/debug/debug.h>
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
#include <syscall/fcntl.h>
#include <syscall/memfd.h>
#include <syscall/syscall.h>

/* Pipe init extern declaration */

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

/*
 * Copy process identifiers while holding the table lock, without handing a
 * process reference to the caller.  This is used by procfs while the VFS
 * namespace lock is held: dropping a process reference there can run the
 * final process destructor, close files, and recursively acquire the VFS
 * namespace lock.
 */
size_t process_snapshot_pids(pid_t *pids, size_t capacity)
{
    if (!pids || !capacity) return 0;

    size_t count = 0;
    spin_lock(&process_table_lock);
    for (size_t i = 0; i < PROCESS_TABLE_SIZE && count < capacity; i++) {
        process_t *proc = process_table[i];
        if (!proc || !proc->task || proc->task->tgid == 0) continue;
        pids[count++] = (pid_t)proc->task->tgid;
    }
    spin_unlock(&process_table_lock);
    return count;
}

void process_debug_dump_tasks(void)
{
    spin_lock(&process_table_lock);
    plogk("task-dump: Begin\n");
    for (size_t i = 1; i < PROCESS_TABLE_SIZE; i++) {
        process_t *proc = process_table[i];
        if (!proc) continue;
        for (ilist_node_t *node = proc->threads.next; node != &proc->threads; node = node->next) {
            task_t *task = rb_entry(node, task_t, thread_node);
            if (task->state == TASK_ZOMBIE) continue;
            plogk("task-dump: pid=%llu name=%s state=%u cpu=%u on_cpu=%llu wait=%p wake=%u tick=%llu\n", task->pid, task->name, task->state,
                  task->cpu_id, task->on_cpu, task->wait_queue, task->wake_reason, task->wake_tick);
        }
    }
    plogk("task-dump: End\n");
    spin_unlock(&process_table_lock);
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
    if (!new_dir) {
        plogk("process: %s: page directory struct allocation failed.\n", proc ? proc->name : "?");
        return 1;
    }

    uint64_t pml4_frame = alloc_frames(1);
    if (!pml4_frame) {
        plogk("process: %s: page directory frame allocation failed.\n", proc ? proc->name : "?");
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
    if (!vma) {
        plogk("process: VMA allocation failed (start=%#lx end=%#lx)\n", (unsigned long)start, (unsigned long)end);
        return NULL;
    }
    vma->start = start;
    vma->end   = end;
    vma->flags = flags;
    vma->type  = VM_REGION_MMAP;
    vma->next  = NULL;
    return vma;
}

int vm_area_insert(process_t *proc, vm_area_t *vma)
{
    if (!proc || !vma || vma->start >= vma->end || (vma->start & (PAGE_4K_SIZE - 1)) || (vma->end & (PAGE_4K_SIZE - 1))
        || vma->end > PROCESS_USER_STACK_TOP)
        return -EINVAL;

    spin_lock(&proc->mmap_lock);
    vm_area_t **link = &proc->mmap_list;
    vm_area_t  *prev = NULL;
    while (*link && (*link)->start < vma->start) {
        prev = *link;
        link = &(*link)->next;
    }
    if ((prev && prev->end > vma->start) || (*link && vma->end > (*link)->start)) {
        spin_unlock(&proc->mmap_lock);
        return -EEXIST;
    }
    vma->next = *link;
    *link     = vma;
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
    if (!proc) return;
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

vm_area_t *process_mmap_replace(process_t *proc, vm_area_t *replacement)
{
    if (!proc) return NULL;
    spin_lock(&proc->mmap_lock);
    vm_area_t *old  = proc->mmap_list;
    proc->mmap_list = replacement;
    spin_unlock(&proc->mmap_lock);
    return old;
}

void process_mmap_destroy_detached(process_t *proc, vm_area_t *list)
{
    vm_area_free(list, proc && proc->task ? (uint32_t)proc->task->pid : 0);
}

static void process_fd_table_init(process_t *proc)
{
    proc->fd_lock.lock   = 0;
    proc->fd_lock.rflags = 0;
    memset(proc->fds, 0, sizeof(proc->fds));
    memset(proc->fd_flags, 0, sizeof(proc->fd_flags));
}

static void process_rlimit_init(process_t *proc)
{
    if (!proc) return;
    proc->rlimit_lock.lock   = 0;
    proc->rlimit_lock.rflags = 0;
    for (int i = 0; i < PROCESS_RLIMIT_COUNT; i++) {
        proc->rlimits[i].current = PROCESS_RLIM_INFINITY;
        proc->rlimits[i].maximum = PROCESS_RLIM_INFINITY;
    }

    /*
     * Limits backed by fixed kernel tables must report their real ceilings;
     * advertising infinity makes libc and applications derive invalid ABI
     * values (notably sysconf(_SC_OPEN_MAX)).
     */
    proc->rlimits[PROCESS_RLIMIT_NOFILE].current     = PROCESS_MAX_FD;
    proc->rlimits[PROCESS_RLIMIT_NOFILE].maximum     = PROCESS_MAX_FD;
    proc->rlimits[PROCESS_RLIMIT_NPROC].current      = 4096;
    proc->rlimits[PROCESS_RLIMIT_NPROC].maximum      = 4096;
    proc->rlimits[PROCESS_RLIMIT_STACK].current      = (uint64_t)PROCESS_STACK_SIZE;
    proc->rlimits[PROCESS_RLIMIT_STACK].maximum      = (uint64_t)PROCESS_STACK_SIZE;
    proc->rlimits[PROCESS_RLIMIT_MEMLOCK].current    = (uint64_t)64 * 1024;
    proc->rlimits[PROCESS_RLIMIT_MEMLOCK].maximum    = (uint64_t)64 * 1024;
    proc->rlimits[PROCESS_RLIMIT_SIGPENDING].current = 4096;
    proc->rlimits[PROCESS_RLIMIT_SIGPENDING].maximum = 4096;
    proc->rlimits[PROCESS_RLIMIT_MSGQUEUE].current   = 819200;
    proc->rlimits[PROCESS_RLIMIT_MSGQUEUE].maximum   = 819200;
    proc->rlimits[PROCESS_RLIMIT_NICE].current       = 0;
    proc->rlimits[PROCESS_RLIMIT_NICE].maximum       = 0;
    proc->rlimits[PROCESS_RLIMIT_RTPRIO].current     = 0;
    proc->rlimits[PROCESS_RLIMIT_RTPRIO].maximum     = 0;
}

uint32_t process_fd_limit(process_t *proc)
{
    if (!proc) return 0;
    spin_lock(&proc->rlimit_lock);
    uint64_t limit = proc->rlimits[PROCESS_RLIMIT_NOFILE].current;
    spin_unlock(&proc->rlimit_lock);
    if (limit > PROCESS_MAX_FD) limit = PROCESS_MAX_FD;
    return (uint32_t)limit;
}

void process_file_get(process_file_t *file)
{
    if (!file) return;

    /*
     * Match Linux's file reference model: transient fd users take an atomic
     * reference while the descriptor-table lock guarantees that the file is
     * still published.  Serialising every read/write against file->lock made
     * tiny stream I/O pay for a lock unrelated to its actual data path.
     */
    uint32_t refs = __atomic_load_n(&file->refcount, __ATOMIC_RELAXED);
    while (refs && !__atomic_compare_exchange_n(&file->refcount, &refs, refs + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {}
}

static void process_file_fd_get(process_file_t *file)
{
    if (!file) return;
    spin_lock(&file->lock);
    if (__atomic_load_n(&file->refcount, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_add(&file->refcount, 1, __ATOMIC_RELAXED);
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
        if (file->file_opened) vfs_file_descriptor_close(file->node, file->private_data);
        vfs_poll_source_close(&file->close_source, UINT32_MAX);
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

    uint32_t refs = __atomic_load_n(&file->refcount, __ATOMIC_RELAXED);
    do {
        if (!refs) return;
    } while (!__atomic_compare_exchange_n(&file->refcount, &refs, refs - 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (refs != 1) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    inotify_notify(file->node, (file->flags & O_ACCMODE) == O_RDONLY ? IN_CLOSE_NOWRITE : IN_CLOSE_WRITE);

    /* Release per-open-instance private_data. */
    if (file->file_opened) callbackof(file->node, file_release)(file->node, file->private_data);

    vfs_close(file->node);
    free(file);
}

static void process_fd_table_close(process_t *proc)
{
    if (!proc) return;

    /*
     * Detach the descriptor table atomically, but never run final-release
     * callbacks while holding proc->fd_lock.  Dropping the last descriptor
     * can notify poll/epoll subscribers and close sockets or pipes; those
     * callbacks are allowed to inspect descriptor state and may take locks
     * below the fd-table layer.  Running them under fd_lock turns an ordinary
     * process exit into a self-deadlock (notably when an OpenRC service runner
     * exits with inherited CLOEXEC/poll descriptors).
     *
     * This is the last thread when called from process_exit(), and callers of
     * process_free() no longer publish the process, so no new descriptors can
     * be installed after the table has been detached.
     */
    process_file_t *files[PROCESS_MAX_FD];
    spin_lock(&proc->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        files[i]          = proc->fds[i];
        proc->fds[i]      = NULL;
        proc->fd_flags[i] = 0;
    }
    spin_unlock(&proc->fd_lock);

    for (int i = 0; i < PROCESS_MAX_FD; i++) process_file_fd_put(files[i]);
}

static void process_fd_table_copy(process_t *child, process_t *parent)
{
    process_fd_table_init(child);

    spin_lock(&parent->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        child->fds[i]      = parent->fds[i];
        child->fd_flags[i] = parent->fd_flags[i];
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

/*
 * Linux's fget_light borrows the descriptor-table reference when the table is
 * private to the current thread.  In that case no other execution context can
 * remove this process's slot before the syscall returns, so neither the fd
 * table spinlock nor a transient file reference is needed.
 */
static process_file_t *process_fd_get_light(process_t *proc, int fd, bool *borrowed)
{
    *borrowed = false;
    if (!proc || fd < 0 || fd >= PROCESS_MAX_FD) return NULL;

    if (proc == process_current() && __atomic_load_n(&proc->thread_count, __ATOMIC_ACQUIRE) == 1) {
        process_file_t *file = __atomic_load_n(&proc->fds[fd], __ATOMIC_ACQUIRE);
        if (file) *borrowed = true;
        return file;
    }
    return process_fd_get(proc, fd);
}

static void process_file_put_light(process_file_t *file, bool borrowed)
{
    if (!borrowed) process_file_put(file);
}

process_file_t *process_fd_get_for_transfer(process_t *proc, int fd)
{
    if (!proc || fd < 0 || fd >= PROCESS_MAX_FD) return NULL;

    /*
     * Keep fd_refcount non-zero while SCM_RIGHTS is in flight.  Otherwise the
     * sender closing its original fd would publish POLLHUP and run the
     * descriptor-close transition before the receiver installs its copy.
     */
    spin_lock(&proc->fd_lock);
    process_file_t *file = proc->fds[fd];
    process_file_fd_get(file);
    spin_unlock(&proc->fd_lock);
    return file;
}

void process_file_put_transfer(process_file_t *file)
{
    process_file_fd_put(file);
}

int process_fd_install(process_t *proc, vfs_node_t node, uint64_t flags)
{
    if (!proc || !node) return -EINVAL;

    process_file_t *file = calloc(1, sizeof(process_file_t));
    if (!file) return -ENOMEM;

    file->node = node;
    /*
     * O_CLOEXEC is a descriptor creation flag.  It must not be shared by
     * dup(2) or forked open-file descriptions through file->flags.
     */
    file->flags       = flags & ~(uint64_t)O_CLOEXEC;
    file->refcount    = 1;
    file->fd_refcount = 1;
    file->lock.lock   = 0;
    file->lock.rflags = 0;
    wait_queue_init(&file->io_wait);
    vfs_poll_source_init(&file->close_source);
    if (flags & O_APPEND) file->offset = node->size;

    /*
     * O_PATH descriptors name a VFS object but do not open the object for
     * I/O.  In particular, do not call a filesystem's file_open callback:
     * sysfs/procfs quite correctly reject ordinary opens of directories,
     * while Linux permits an O_PATH descriptor for those same directories.
     * fstat(2), *at(2), fchdir(2), dup(2) and close(2) operate on the retained
     * vnode directly.
     */
    if (!(flags & O_PATH)) {
        void *priv = NULL;
        int   ret  = callbackof(node, file_open)(node, file->flags, &priv);
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
    uint32_t limit = process_fd_limit(proc);
    for (uint32_t i = 0; i < limit; i++) {
        if (!proc->fds[i]) {
            proc->fds[i]      = file;
            proc->fd_flags[i] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
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

int process_fd_install_file(process_t *proc, process_file_t *file, uint64_t flags)
{
    if (!proc || !file) return -EINVAL;

    spin_lock(&proc->fd_lock);
    uint32_t limit = process_fd_limit(proc);
    for (uint32_t i = 0; i < limit; i++) {
        if (!proc->fds[i]) {
            /*
             * SCM_RIGHTS and dup(2) share the same open-file description:
             * file offset, status flags and private driver state all remain
             * shared.  Only FD_CLOEXEC belongs to the new descriptor.
             */
            process_file_fd_get(file);
            proc->fds[i]      = file;
            proc->fd_flags[i] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            spin_unlock(&proc->fd_lock);
            return (int)i;
        }
    }
    spin_unlock(&proc->fd_lock);
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
    proc->fds[fd]      = NULL;
    proc->fd_flags[fd] = 0;
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

    uint32_t limit = process_fd_limit(proc);
    for (uint32_t i = 0; i < limit; i++) {
        if (!proc->fds[i]) {
            process_file_fd_get(file);
            proc->fds[i] = file;
            /* dup(2) always clears close-on-exec on the new descriptor. */
            proc->fd_flags[i] = 0;
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
    if ((uint32_t)newfd >= process_fd_limit(proc)) return -EBADF;
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
    /* dup2(2) always clears close-on-exec on the replacement descriptor. */
    proc->fd_flags[newfd] = 0;
    spin_unlock(&proc->fd_lock);

    process_file_fd_put(old);
    return newfd;
}

int64_t process_fd_read(process_t *proc, int fd, void *buf, size_t size)
{
    bool            borrowed;
    process_file_t *file = process_fd_get_light(proc, fd, &borrowed);
    if (!file) return -EBADF;

    /*
     * Pipes, like Linux FMODE_STREAM files, have no shared file position.
     * Their ring lock provides the required serialization, so taking the
     * open-file f_pos lock on every small transfer is both redundant and
     * expensive.
     */
    bool positionless = (file->node->type & (file_stream | file_pipe)) != 0;
    if (!positionless) process_file_io_lock(file);

    uint64_t flags;
    size_t   offset;
    if (positionless) {
        flags  = __atomic_load_n(&file->flags, __ATOMIC_RELAXED);
        offset = 0;
    } else {
        spin_lock(&file->lock);
        flags  = file->flags;
        offset = file->offset;
        spin_unlock(&file->lock);
    }

    if (flags & O_PATH) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EBADF;
    }

    if ((flags & O_ACCMODE) == O_WRONLY) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EBADF;
    }

    int64_t ret = vfs_file_read_process(file->node, file->private_data, flags, buf, offset, size, proc);
    if (!positionless) {
        if (ret >= 0) {
            spin_lock(&file->lock);
            file->offset = offset + (size_t)ret;
            spin_unlock(&file->lock);
        }
        process_file_io_unlock(file);
    }

    process_file_put_light(file, borrowed);
    return ret;
}

int64_t process_fd_write(process_t *proc, int fd, const void *buf, size_t size)
{
    bool            borrowed;
    process_file_t *file = process_fd_get_light(proc, fd, &borrowed);
    if (!file) return -EBADF;

    bool positionless = (file->node->type & (file_stream | file_pipe)) != 0;
    if (!positionless) process_file_io_lock(file);

    uint64_t flags;
    size_t   offset;
    if (positionless) {
        flags  = __atomic_load_n(&file->flags, __ATOMIC_RELAXED);
        offset = 0;
    } else {
        spin_lock(&file->lock);
        flags  = file->flags;
        offset = file->offset;
        if (flags & O_APPEND) offset = file->node->size;
        spin_unlock(&file->lock);
    }

    if (flags & O_PATH) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EBADF;
    }

    if ((flags & O_ACCMODE) == O_RDONLY) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EBADF;
    }

    if (vfs_mount_is_readonly(file->node)) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EROFS;
    }

    int64_t ret = vfs_file_write_process(file->node, file->private_data, flags, buf, offset, size, proc);
    if (!positionless) {
        if (ret >= 0) {
            spin_lock(&file->lock);
            file->offset = offset + (size_t)ret;
            spin_unlock(&file->lock);
        }
        process_file_io_unlock(file);
    }

    process_file_put_light(file, borrowed);
    return ret;
}

int64_t process_fd_read_user(process_t *proc, int fd, void *buf, size_t size)
{
    bool            borrowed;
    process_file_t *file = process_fd_get_light(proc, fd, &borrowed);
    if (!file) return -EBADF;

    bool positionless = (file->node->type & (file_stream | file_pipe)) != 0;
    if (!positionless) process_file_io_lock(file);

    uint64_t flags;
    size_t   offset;
    if (positionless) {
        flags  = __atomic_load_n(&file->flags, __ATOMIC_RELAXED);
        offset = 0;
    } else {
        spin_lock(&file->lock);
        flags  = file->flags;
        offset = file->offset;
        spin_unlock(&file->lock);
    }

    if ((flags & O_PATH) || (flags & O_ACCMODE) == O_WRONLY) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EBADF;
    }

    int64_t ret = vfs_file_read_user_process(file->node, file->private_data, flags, buf, offset, size, proc);
    if (!positionless) {
        if (ret >= 0) {
            spin_lock(&file->lock);
            file->offset = offset + (size_t)ret;
            spin_unlock(&file->lock);
        }
        process_file_io_unlock(file);
    }

    process_file_put_light(file, borrowed);
    return ret;
}

int64_t process_fd_write_user(process_t *proc, int fd, const void *buf, size_t size)
{
    bool            borrowed;
    process_file_t *file = process_fd_get_light(proc, fd, &borrowed);
    if (!file) return -EBADF;

    bool positionless = (file->node->type & (file_stream | file_pipe)) != 0;
    if (!positionless) process_file_io_lock(file);

    uint64_t flags;
    size_t   offset;
    if (positionless) {
        flags  = __atomic_load_n(&file->flags, __ATOMIC_RELAXED);
        offset = 0;
    } else {
        spin_lock(&file->lock);
        flags  = file->flags;
        offset = (flags & O_APPEND) ? file->node->size : file->offset;
        spin_unlock(&file->lock);
    }

    if ((flags & O_PATH) || (flags & O_ACCMODE) == O_RDONLY) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EBADF;
    }
    if (vfs_mount_is_readonly(file->node)) {
        if (!positionless) process_file_io_unlock(file);
        process_file_put_light(file, borrowed);
        return -EROFS;
    }

    int64_t ret = vfs_file_write_user_process(file->node, file->private_data, flags, buf, offset, size, proc);
    if (!positionless) {
        if (ret >= 0) {
            spin_lock(&file->lock);
            file->offset = offset + (size_t)ret;
            spin_unlock(&file->lock);
        }
        process_file_io_unlock(file);
    }

    process_file_put_light(file, borrowed);
    return ret;
}

int64_t process_fd_seek(process_t *proc, int fd, int64_t offset, int whence)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    if (file->flags & O_PATH) {
        process_file_put(file);
        return -EBADF;
    }

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
    if (file->flags & O_PATH) {
        process_file_put(file);
        return -EBADF;
    }
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
        /*
         * The original pathname was non-empty and consisted solely of one or
         * more separators.  Preserve the process root instead of turning a
         * valid "/" lookup into the empty-path ENOENT case.
         */
        if (!path[0]) {
            size_t root_len = strlen(root);
            if (root_len + 1 > size) return -ENAMETOOLONG;
            memcpy(resolved, root, root_len + 1);
            ret = EOK;
        } else {
            ret = vfs_resolve_path(root, path, resolved, size);
        }
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
    if (root_len != 1 && (strncmp(resolved, root, root_len) != 0 || (resolved[root_len] && resolved[root_len] != '/'))) return -EPERM;
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
    stat->nlink = file->node->nlink;
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
    if (!proc) {
        plogk("process: Process '%s' creation failed (control block OOM)\n", name ? name : "?");
        return NULL;
    }

    task_t *task = task_alloc(name);
    if (!task) {
        plogk("process: '%s' task allocation failed.\n", name ? name : "?");
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
        plogk("process: '%s' kernel stack allocation failed (%d bytes)\n", name ? name : "?", PROCESS_KERNEL_STACK);
        task_free(task);
        free(proc);
        return NULL;
    }

    if (setup_process_page_dir(proc)) {
        plogk("process: '%s' page directory setup failed.\n", name ? name : "?");
        free(proc->kernel_stack);
        task_free(task);
        free(proc);
        return NULL;
    }

    proc->task->state = TASK_RUNNING;
    proc->uid         = 1000;
    proc->gid         = 1000;
    proc->fsuid       = 1000;
    proc->fsgid       = 1000;
    proc->umask       = 022;
    proc->pgid        = 0;
    proc->sid         = 0;
    proc->start_brk   = PROCESS_HEAP_START;
    proc->heap_brk    = PROCESS_HEAP_START;
    proc->stack_brk   = PROCESS_STACK_BASE - (long)PROCESS_STACK_SIZE;
    proc->parent      = init_process;
    proc->exit_code   = 0;
    slist_init(&proc->children);
    wait_queue_init(&proc->child_wait);
    wait_queue_init(&proc->vfork_wait);
    proc->vfork_done       = true;
    proc->mmap_lock.lock   = 0;
    proc->mmap_lock.rflags = 0;
    proc->brk_lock.lock    = 0;
    proc->brk_lock.rflags  = 0;
    process_fd_table_init(proc);
    process_rlimit_init(proc);
    signal_state_init(&proc->signal);
    task_name_copy(task, name);

    strncpy(proc->name, name ? name : "user", PROCESS_NAME_LEN - 1);
    proc->name[PROCESS_NAME_LEN - 1] = '\0';

    pid_set(proc->task->pid, proc);

    if (proc->parent && proc->parent != proc) { slist_insert_tail(&proc->parent->children, proc); }

    return proc;
}

process_t *process_create_kernel(const char *name, void (*entry)(void *), void *arg)
{
    process_t *proc = calloc(1, sizeof(process_t));
    if (!proc) {
        plogk("process: Kernel process '%s' creation failed (control block OOM)\n", name ? name : "?");
        return NULL;
    }

    proc->kernel_stack = malloc(PROCESS_KERNEL_STACK);
    if (!proc->kernel_stack) {
        plogk("process: Kernel process '%s' kernel stack allocation failed (%d bytes)\n", name ? name : "?", PROCESS_KERNEL_STACK);
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
        plogk("process: Kernel process '%s' task allocation failed.\n", name ? name : "?");
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
    proc->fsuid       = 0;
    proc->fsgid       = 0;
    proc->umask       = 022;
    proc->pgid        = 0;
    proc->sid         = 0;
    proc->start_brk   = 0;
    proc->heap_brk    = 0;
    proc->stack_brk   = 0;
    proc->parent      = init_process;
    proc->exit_code   = 0;
    slist_init(&proc->children);
    wait_queue_init(&proc->child_wait);
    wait_queue_init(&proc->vfork_wait);
    proc->vfork_done       = true;
    proc->mmap_lock.lock   = 0;
    proc->mmap_lock.rflags = 0;
    proc->brk_lock.lock    = 0;
    proc->brk_lock.rflags  = 0;
    process_fd_table_init(proc);
    process_rlimit_init(proc);
    signal_state_init(&proc->signal);
    task_name_copy(task, name);
    strncpy(proc->name, name ? name : "kthread", PROCESS_NAME_LEN - 1);
    proc->name[PROCESS_NAME_LEN - 1] = '\0';

    pid_set(proc->task->pid, proc);

    if (proc->parent && proc->parent != proc) { slist_insert_tail(&proc->parent->children, proc); }

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
        if (child && child != proc) {
            child->parent = init_process;
            slist_insert_tail(&init_process->children, child);
        }
        node = next;
    }

    spin_unlock(&process_table_lock);
    spin_unlock(&scheduler.lock);

    /*
     * Notify parent via SIGCHLD (outside the locks, because signal_notify_child_exit
     * calls task_wakeup which acquires scheduler.lock, and we have just released it).
     */
    if (parent) {
        signal_notify_child_exit(parent, (pid_t)current->tgid, exit_code, 0);
        spin_lock(&parent->child_wait.lock);
        wait_queue_wake_all(&parent->child_wait);
        spin_unlock(&parent->child_wait.lock);
        process_put(parent);
    }

    /* Notify set_tid_address with 0 (futex wake on clear_child_tid) */
    if (current->clear_child_tid) {
        uint32_t zero = 0;
        copy_to_user((void *)current->clear_child_tid, &zero, sizeof(zero));
        sys_futex((uint32_t *)current->clear_child_tid, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, NULL, 0);
    }

    process_fd_table_close(proc);

    /*
     * A vfork parent may not resume until the child has either installed a
     * replacement image or fully released inherited descriptors on exit.
     */
    process_vfork_complete(proc);

    ptrace_exit_notify(exit_code);

    proc->task->state = TASK_ZOMBIE;
    task_exit();
}

static bool process_wait_selector_matches(process_t *parent, process_t *child, pid_t selector)
{
    if (!parent || !child || child->parent != parent || !child->task) return false;
    if (selector > 0) return (pid_t)child->task->tgid == selector;
    if (selector == -1) return true;
    if (selector == 0) return child->pgid == parent->pgid;
    if (selector == INT64_MIN) return false;
    return child->pgid == -selector;
}

static bool process_group_is_zombie(process_t *child)
{
    if (!child || !child->task || child->task->state != TASK_ZOMBIE) return false;
    for (ilist_node_t *node = child->threads.next; node != &child->threads; node = node->next) {
        task_t *member = rb_entry(node, task_t, thread_node);
        if (member->state != TASK_ZOMBIE) return false;
    }
    return ptrace_tracer_pid(child->task) == 0;
}

static bool process_group_is_on_cpu(process_t *child)
{
    if (!child) return false;
    for (ilist_node_t *node = child->threads.next; node != &child->threads; node = node->next) {
        task_t *member = rb_entry(node, task_t, thread_node);
        if (__atomic_load_n(&member->on_cpu, __ATOMIC_ACQUIRE)) return true;
    }
    return false;
}

static int process_wait_status(const process_t *child)
{
    if (!child) return 0;
    if (child->exit_code < 0) return (-child->exit_code) & 0x7f;
    return (child->exit_code & 0xff) << 8;
}

static void process_child_wait_event(process_t *child, int stop_signal, bool continued)
{
    if (!child) return;

    spin_lock(&process_table_lock);
    process_t *parent = child->parent;
    if (parent) process_get_locked(parent);
    spin_unlock(&process_table_lock);
    if (!parent) return;

    bool published = false;
    spin_lock(&parent->child_wait.lock);
    spin_lock(&process_table_lock);
    if (child->parent == parent) {
        if (continued) {
            child->wait_continue_pending = true;
        } else {
            child->wait_stop_signal  = stop_signal;
            child->wait_stop_pending = true;
        }
        published = true;
    }
    spin_unlock(&process_table_lock);
    if (published) wait_queue_wake_all(&parent->child_wait);
    spin_unlock(&parent->child_wait.lock);

    if (published)
        signal_notify_child_status(parent, (pid_t)child->task->tgid, continued ? SIGCONT : stop_signal, continued ? CLD_CONTINUED : CLD_STOPPED);
    process_put(parent);
}

void process_child_stopped(process_t *child, int signal)
{
    process_child_wait_event(child, signal, false);
}

void process_child_continued(process_t *child)
{
    process_child_wait_event(child, 0, true);
}

int process_wait_select(pid_t selector, int *wait_status, uint32_t options, pid_t *waited_pid)
{
    if (waited_pid) *waited_pid = 0;
    if (!init_process) return -ECHILD;
    if (selector == INT64_MIN) return -ESRCH;
    if (options & ~(PROCESS_WAIT_NOHANG | PROCESS_WAIT_STOPPED | PROCESS_WAIT_CONTINUED)) return -EINVAL;

    process_t *parent = process_current();
    if (!parent) return -ECHILD;

    for (;;) {
        process_t *zombie             = NULL;
        process_t *event              = NULL;
        int        event_status       = 0;
        bool       has_matching_child = false;

        /*
         * Serialize the condition check with child-exit notification.  The
         * queue lock closes the classic exit-between-check-and-sleep race.
         */
        spin_lock(&parent->child_wait.lock);
        spin_lock(&process_table_lock);
        for (slist_node_t *node = parent->children.head; node; node = node->next) {
            process_t *child = node->data;
            if (!process_wait_selector_matches(parent, child, selector)) continue;
            has_matching_child = true;
            if (process_group_is_zombie(child)) {
                zombie = child;
                break;
            }
            if ((options & PROCESS_WAIT_STOPPED) && child->wait_stop_pending) {
                event                    = child;
                event_status             = ((child->wait_stop_signal & 0xff) << 8) | 0x7f;
                child->wait_stop_pending = false;
                break;
            }
            if ((options & PROCESS_WAIT_CONTINUED) && child->wait_continue_pending) {
                event                        = child;
                event_status                 = 0xffff;
                child->wait_continue_pending = false;
                break;
            }
        }

        if (event) {
            pid_t event_pid = (pid_t)event->task->tgid;
            spin_unlock(&process_table_lock);
            spin_unlock(&parent->child_wait.lock);
            if (wait_status) *wait_status = event_status;
            if (waited_pid) *waited_pid = event_pid;
            return EOK;
        }

        if (zombie) {
            pid_t reaped_pid = (pid_t)zombie->task->tgid;
            int   status     = process_wait_status(zombie);
            pid_set_locked(reaped_pid, NULL);
            slist_remove(&parent->children, zombie);
            zombie->parent = NULL;
            spin_unlock(&process_table_lock);
            spin_unlock(&parent->child_wait.lock);

            /*
             * TASK_ZOMBIE is published before the exiting CPU has switched
             * off the task's kernel stack.  Delay physical destruction until
             * every thread has completed that switch; the PID/status have
             * already been consumed atomically above.
             */
            while (process_group_is_on_cpu(zombie)) task_sleep_ticks(1);
            process_put(zombie);

            if (wait_status) *wait_status = status;
            if (waited_pid) *waited_pid = reaped_pid;
            return EOK;
        }

        spin_unlock(&process_table_lock);
        if (!has_matching_child) {
            spin_unlock(&parent->child_wait.lock);
            return -ECHILD;
        }
        if (options & PROCESS_WAIT_NOHANG) {
            spin_unlock(&parent->child_wait.lock);
            return EOK;
        }

        wait_queue_prepare(&parent->child_wait);
        spin_unlock(&parent->child_wait.lock);

        /*
         * Once prepared, signal delivery either observes the waiter in the
         * queue or is visible here.  Default-ignored SIGCHLD does not
         * interrupt wait4; a caught/terminating signal does.
         */
        spin_lock(&parent->signal.lock);
        bool interrupted = signal_has_interrupting_pending(&parent->signal);
        spin_unlock(&parent->signal.lock);
        if (interrupted) {
            task_wakeup(current_task());
            return -ERESTARTSYS;
        }
        wait_queue_sleep();
    }
}

int process_wait(pid_t pid, int *exit_code)
{
    pid_t waited_pid = 0;
    int   status     = 0;
    int   result     = process_wait_select(pid, &status, 0, &waited_pid);
    if (result != EOK || !waited_pid) return 1;
    if (exit_code) {
        if ((status & 0x7f) != 0)
            *exit_code = -(status & 0x7f);
        else
            *exit_code = (status >> 8) & 0xff;
    }
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

process_t *process_fork_status_event_mode(int *error, uint32_t ptrace_event, bool vfork)
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
        plogk("process: Fork of '%s' failed (control block OOM)\n", parent->name);
        if (error) *error = -ENOMEM;
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    int     task_error = EOK;
    task_t *child_task = task_alloc_status(parent->task->name, &task_error);
    if (!child_task) {
        plogk("process: Fork of '%s' failed (task allocation, errno %d)\n", parent->name, task_error);
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
    child->fsuid       = parent->fsuid;
    child->fsgid       = parent->fsgid;
    child->umask       = parent->umask;
    child->pgid        = parent->pgid;
    child->sid         = parent->sid;
    child->parent      = parent;
    child->exit_code   = 0;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    child->name[sizeof(child->name) - 1] = '\0';
    child->start_brk                     = parent->start_brk;
    child->heap_brk                      = parent->heap_brk;
    child->stack_brk                     = parent->stack_brk;
    memcpy(child->root, parent->root, sizeof(child->root));
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
    child->kernel_stack = malloc(PROCESS_KERNEL_STACK);
    if (!child->kernel_stack) {
        plogk("process: Fork of '%s' failed (kernel stack OOM)\n", parent->name);
        if (error) *error = -ENOMEM;
        task_free(child_task);
        free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }
    child->mmap_lock.lock   = 0;
    child->mmap_lock.rflags = 0;
    child->brk_lock.lock    = 0;
    child->brk_lock.rflags  = 0;
    strncpy(child->name, parent->name, PROCESS_NAME_LEN - 1);
    child->name[PROCESS_NAME_LEN - 1] = '\0';
    process_fd_table_copy(child, parent);
    child->rlimit_lock.lock   = 0;
    child->rlimit_lock.rflags = 0;
    spin_lock(&parent->rlimit_lock);
    memcpy(child->rlimits, parent->rlimits, sizeof(child->rlimits));
    spin_unlock(&parent->rlimit_lock);
    signal_state_copy(&child->signal, &parent->signal);
    process_ctty_inherit(child, parent);
    slist_init(&child->children);
    wait_queue_init(&child->child_wait);
    wait_queue_init(&child->vfork_wait);
    child->vfork_done = !vfork;

    if (setup_process_page_dir(child)) {
        plogk("process: Fork of '%s' failed (page directory setup)\n", parent->name);
        if (error) *error = -ENOMEM;
        process_free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    if (page_clone_user_cow(child->user_page_dir, parent->user_page_dir)) {
        plogk("process: Fork of '%s' failed (user pages COW clone)\n", parent->name);
        if (error) *error = -ENOMEM;
        process_free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    for (vm_area_t *vma = parent->mmap_list; vma; vma = vma->next) {
        vm_area_t *copy = vm_area_alloc(vma->start, vma->end, vma->flags);
        if (!copy) {
            plogk("process: Fork of '%s' failed (VMA copy OOM)\n", parent->name);
            if (error) *error = -ENOMEM;
            process_free(child);
            spin_unlock(&parent->mmap_lock);
            spin_unlock(&scheduler.lock);
            return NULL;
        }
        copy->type            = vma->type;
        copy->vm_file         = vma->vm_file ? vfs_node_retain(vma->vm_file) : NULL;
        copy->vm_pgoff        = vma->vm_pgoff;
        copy->vm_private_data = vma->vm_private_data;
        copy->vm_pagecache    = vma->vm_pagecache;

        if (vma->vm_file && !copy->vm_file) {
            free(copy);
            if (error) *error = -ENOENT;
            process_free(child);
            spin_unlock(&parent->mmap_lock);
            spin_unlock(&scheduler.lock);
            return NULL;
        }
        if (copy->vm_file && copy->vm_pagecache) (void)vfs_cache_mapping_pin(copy->vm_file);
        if (copy->vm_file) memfd_vma_retain(copy->vm_file, copy->flags);
        if (copy->type == VM_REGION_SHM && sysv_shm_vma_get(copy->vm_private_data, (uint32_t)child->task->pid)) {
            plogk("process: Fork of '%s' failed (SHM VMA lookup)\n", parent->name);
            if (copy->vm_file) {
                if (copy->vm_pagecache) vfs_cache_mapping_unpin(copy->vm_file);
                memfd_vma_release(copy->vm_file, copy->flags);
                vfs_close(copy->vm_file);
            }
            free(copy);
            if (error) *error = -ENOMEM;
            process_free(child);
            spin_unlock(&parent->mmap_lock);
            spin_unlock(&scheduler.lock);
            return NULL;
        }

        if (vm_area_insert(child, copy)) {
            plogk("process: Fork of '%s' failed (VMA insert)\n", parent->name);
            if (copy->vm_file) {
                if (copy->vm_pagecache) vfs_cache_mapping_unpin(copy->vm_file);
                memfd_vma_release(copy->vm_file, copy->flags);
                vfs_close(copy->vm_file);
            }
            free(copy);
            if (error) *error = -ENOMEM;
            process_free(child);
            spin_unlock(&parent->mmap_lock);
            spin_unlock(&scheduler.lock);
            return NULL;
        }
    }

    memcpy(&child_task->context, &current->context, sizeof(task_context_t));
    child_task->thread.fs_base = current->thread.fs_base;
    child_task->thread.gs_base = current->thread.gs_base;
    fpu_task_clone(current, child_task);

    child_task->cpu_id = current->cpu_id;

    pid_set(child->task->pid, child);

    slist_insert_tail(&parent->children, child);

    (void)ptrace_fork_child(current, child_task, ptrace_event);

    spin_unlock(&parent->mmap_lock);
    spin_unlock(&scheduler.lock);
    flush_tlb_all();

    /* plogk("process: Forked process %llu from parent %llu\n", child->task->pid, parent->task->pid); it is very noisy */
    return child;
}

void process_fork_publish(process_t *child)
{
    if (!child || !child->task || child->task->state != TASK_READY) return;
    spin_lock(&scheduler.lock);
    enqueue_task_initial(child->task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(child->task);
}

process_t *process_fork_status_event(int *error, uint32_t ptrace_event)
{
    return process_fork_status_event_mode(error, ptrace_event, false);
}

void process_vfork_wait(process_t *child)
{
    if (!child) return;

    spin_lock(&child->vfork_wait.lock);
    while (!child->vfork_done) {
        wait_queue_prepare(&child->vfork_wait);
        spin_unlock(&child->vfork_wait.lock);
        wait_queue_sleep();
        spin_lock(&child->vfork_wait.lock);
    }
    spin_unlock(&child->vfork_wait.lock);
}

void process_vfork_complete(process_t *proc)
{
    if (!proc) return;

    spin_lock(&proc->vfork_wait.lock);
    if (!proc->vfork_done) {
        proc->vfork_done = true;
        wait_queue_wake_all(&proc->vfork_wait);
    }
    spin_unlock(&proc->vfork_wait.lock);
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
    process_fork_publish(child);
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
    if (!user_access_ok((void *)(child_stack - 1), 1, 1) || (parent_tid && !user_access_ok((void *)parent_tid, sizeof(uint32_t), 1))
        || (child_set_tid && !user_access_ok((void *)child_set_tid, sizeof(uint32_t), 1))
        || (child_clear_tid && !user_access_ok((void *)child_clear_tid, sizeof(uint32_t), 1))) {
        if (error) *error = -EFAULT;
        return NULL;
    }

    int     task_error = EOK;
    task_t *child      = task_alloc_status(current->name, &task_error);
    if (!child) {
        plogk("process: Thread clone of '%s' failed (task allocation, errno %d)\n", current->name, task_error);
        if (error) *error = task_error;
        return NULL;
    }
    child->kernel_stack = malloc(TASK_KERNEL_STACK);
    if (!child->kernel_stack) {
        plogk("process: Thread clone of '%s' failed (kernel stack OOM)\n", current->name);
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
    if (!proc || !proc->user_page_dir || !length || (addr & (PAGE_4K_SIZE - 1))) return 1;
    if (length > UINT64_MAX - (PAGE_4K_SIZE - 1)) return 1;
    size_t bytes = ALIGN_UP(length, PAGE_4K_SIZE);
    if (addr > UINT64_MAX - bytes || addr + bytes > PROCESS_USER_STACK_TOP) return 1;
    size_t pages = bytes / PAGE_4K_SIZE;
    if (pages > SIZE_MAX / sizeof(uint64_t)) return 1;

    vm_area_t *vma = vm_area_alloc(addr, addr + bytes, flags);
    if (!vma) return 1;

    if (flags & VM_LAZY) {
        spin_lock(&proc->mmap_lock);
        vm_area_t *previous = NULL;
        vm_area_t *cursor   = proc->mmap_list;
        while (cursor && cursor->end <= addr) {
            previous = cursor;
            cursor   = cursor->next;
        }
        if ((previous && addr < previous->end) || (cursor && addr + bytes > cursor->start)) {
            spin_unlock(&proc->mmap_lock);
            free(vma);
            return 1;
        }
        vma->type = VM_REGION_MMAP;
        if (previous) {
            vma->next      = previous->next;
            previous->next = vma;
        } else {
            vma->next       = proc->mmap_list;
            proc->mmap_list = vma;
        }
        spin_unlock(&proc->mmap_lock);
        return 0;
    }

    uint64_t *frames = calloc(pages, sizeof(*frames));
    if (!frames) {
        plogk("process: %s: mmap frame list allocation failed (%lu pages at %#lx)\n", proc->name, (unsigned long)pages, (unsigned long)addr);
        free(vma);
        return 1;
    }

    /*
     * Anonymous memory is required to read as zero on first use.  Allocate
     * the complete transaction before publishing either PTEs or the VMA.
     */
    size_t allocated = 0;
    for (; allocated < pages; allocated++) {
        frames[allocated] = alloc_frames(1);
        if (!frames[allocated]) {
            plogk("process: %s: mmap frame allocation failed (%lu/%lu pages at %#lx)\n", proc->name, (unsigned long)allocated,
                  (unsigned long)pages, (unsigned long)addr);
            goto rollback_frames;
        }
        memset(phys_to_virt(frames[allocated]), 0, PAGE_4K_SIZE);
    }

    uint64_t pte_flags = PTE_USER | PTE_PRESENT;
    if (flags & VM_WRITE) pte_flags |= PTE_WRITEABLE;
    if (flags & VM_SHARED) pte_flags |= PTE_SHARED;
    if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;

    spin_lock(&proc->mmap_lock);
    vm_area_t *previous = NULL;
    vm_area_t *cursor   = proc->mmap_list;
    while (cursor && cursor->end <= addr) {
        previous = cursor;
        cursor   = cursor->next;
    }
    if ((previous && addr < previous->end) || (cursor && addr + bytes > cursor->start)) {
        spin_unlock(&proc->mmap_lock);
        goto rollback_frames;
    }

    size_t mapped = 0;
    for (; mapped < pages; mapped++)
        if (page_map_new_to(proc->user_page_dir, addr + mapped * PAGE_4K_SIZE, frames[mapped], pte_flags) < 0) break;
    if (mapped != pages) {
        plogk("process: %s: mmap page map failed at %#lx (%lu/%lu pages)\n", proc->name, (unsigned long)addr, (unsigned long)mapped,
              (unsigned long)pages);
        for (size_t i = 0; i < mapped; i++) (void)page_unmap_release(proc->user_page_dir, addr + i * PAGE_4K_SIZE);
        spin_unlock(&proc->mmap_lock);
        allocated = pages;
        /* Mapped frames were released by page_unmap_release(). */
        for (size_t i = 0; i < mapped; i++) frames[i] = 0;
        goto rollback_frames;
    }

    vma->type = VM_REGION_MMAP;
    if (previous) {
        vma->next      = previous->next;
        previous->next = vma;
    } else {
        vma->next       = proc->mmap_list;
        proc->mmap_list = vma;
    }
    spin_unlock(&proc->mmap_lock);
    free(frames);
    return 0;

rollback_frames:
    for (size_t i = 0; i < allocated; i++)
        if (frames[i]) (void)frame_release_range(frames[i], 1);
    free(frames);
    free(vma);
    return 1;
}

int process_demand_fault(process_t *proc, uintptr_t addr, int write, int exec)
{
    if (!proc || !proc->user_page_dir || !proc->user_page_dir->table) return -1;
    uintptr_t page = ALIGN_DOWN(addr, PAGE_4K_SIZE);

    spin_lock(&proc->mmap_lock);
    vm_area_t *vma = proc->mmap_list;
    while (vma && vma->end <= page) vma = vma->next;
    if (!vma || vma->start > page || page >= vma->end) {
        spin_unlock(&proc->mmap_lock);
        return -1;
    }
    vm_flags_t flags     = vma->flags;
    vfs_node_t vm_file   = vma->vm_file;
    uint64_t   pgoff     = vma->vm_pgoff;
    bool       pagecache = vma->vm_pagecache;
    uintptr_t  vma_start = vma->start;
    if (vm_file) vm_file = vfs_node_retain(vm_file);
    spin_unlock(&proc->mmap_lock);

    if (exec && !(flags & VM_EXEC)) goto fail;
    if (write && !(flags & VM_WRITE)) goto fail;
    if (!(flags & VM_READ)) goto fail;

    uint64_t frame = 0;
    size_t   index = page - vma_start;
    if (pagecache && vm_file) {
        int dirty = (flags & (VM_SHARED | VM_WRITE)) == (VM_SHARED | VM_WRITE);
        if (vfs_cache_map_page(vm_file, pgoff + index / PAGE_4K_SIZE, dirty, &frame)) goto fail;
    } else if (vm_file) {
        frame = alloc_frames(1);
        if (!frame) goto fail;
        void *virt = phys_to_virt(frame);
        memset(virt, 0, PAGE_4K_SIZE);
        size_t read_offset = pgoff * PAGE_4K_SIZE + index;
        size_t to_read     = PAGE_4K_SIZE;
        if (read_offset < vm_file->size) {
            if (read_offset + to_read > vm_file->size) to_read = vm_file->size - read_offset;
            vfs_read(vm_file, virt, read_offset, to_read);
        }
    } else {
        frame = alloc_frames(1);
        if (!frame) goto fail;
        memset(phys_to_virt(frame), 0, PAGE_4K_SIZE);
    }

    spin_lock(&proc->mmap_lock);
    vma = proc->mmap_list;
    while (vma && vma->end <= page) vma = vma->next;
    if (!vma || vma->start > page || page >= vma->end || vma->flags != flags || vma->vm_file != vm_file || vma->vm_pagecache != pagecache
        || (vm_file && vma->vm_pgoff + (page - vma->start) / PAGE_4K_SIZE != pgoff + index / PAGE_4K_SIZE)) {
        spin_unlock(&proc->mmap_lock);
        (void)frame_release_range(frame, 1);
        goto fail;
    }
    flags = vma->flags;
    if (exec && !(flags & VM_EXEC)) goto fail_frame_locked;
    if (write && !(flags & VM_WRITE)) goto fail_frame_locked;
    if (!(flags & VM_READ)) goto fail_frame_locked;

    uint64_t pte_flags = PTE_USER | PTE_PRESENT;
    if (flags & VM_WRITE) pte_flags |= PTE_WRITEABLE;
    if (flags & VM_SHARED) pte_flags |= PTE_SHARED;
    if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;
    if (vm_file && !(flags & VM_SHARED) && (flags & VM_WRITE)) pte_flags = (pte_flags & ~PTE_WRITEABLE) | PTE_COW;

    if (page_map_new_to(proc->user_page_dir, page, frame, pte_flags) < 0) {
        (void)frame_release_range(frame, 1);
        /*
         * Another thread may have satisfied the same fault while this page
         * was being allocated or read.  Accept its mapping if it permits the
         * original access.
         */
        if (!page_user_accessible(proc->user_page_dir, page, write, exec)) {
            spin_unlock(&proc->mmap_lock);
            goto fail;
        }
    }
    spin_unlock(&proc->mmap_lock);
    if (vm_file) vfs_close(vm_file);
    return 0;

fail_frame_locked:
    spin_unlock(&proc->mmap_lock);
    (void)frame_release_range(frame, 1);

fail:
    if (vm_file) vfs_close(vm_file);
    return -1;
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

    for (uintptr_t va = addr; va < end; va += PAGE_4K_SIZE)
        if (page_unmap_release(proc->user_page_dir, va) < 0) return -ENOMEM;

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
