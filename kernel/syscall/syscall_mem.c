/*
 *
 *      syscall_mem.c
 *      Memory mappings, protection and swap control
 *
 *      2026/11/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/std/errno.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <mem/mmap.h>
#include <mem/page.h>
#include <mem/swap.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <syscall/syscall.h>
#include <syscall/syscall_impl.h>

/* System call handler for `mmap`. */
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t pgoff)
{
    return sys_mmap_pgoff(addr, length, prot, flags, fd, pgoff);
}

/* System call handler for `munmap`. */
int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_munmap_full(addr, length);
}

/* brk syscall: adjust the program break */
int64_t sys_brk(uint64_t addr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return 0;

    spin_lock(&proc->brk_lock);
    uintptr_t old_brk = proc->heap_brk;

    if (!addr) {
        spin_unlock(&proc->brk_lock);
        return (int64_t)old_brk;
    }
    if (addr < proc->start_brk || addr > CONFIG_PROCESS_HEAP_MAX) {
        spin_unlock(&proc->brk_lock);
        return (int64_t)old_brk;
    }
    if (addr > old_brk) {
        uintptr_t start = ALIGN_UP(old_brk, PAGE_4K_SIZE);
        uintptr_t end   = ALIGN_UP(addr, PAGE_4K_SIZE);
        if (end > start && process_mmap(proc, start, end - start, VM_READ | VM_WRITE | VM_LAZY)) {
            spin_unlock(&proc->brk_lock);
            return (int64_t)old_brk;
        }
    } else if (addr < old_brk) {
        uintptr_t start = ALIGN_UP(addr, PAGE_4K_SIZE);
        uintptr_t end   = ALIGN_UP(old_brk, PAGE_4K_SIZE);
        if (end > start && sys_munmap_full(start, end - start) != EOK) {
            spin_unlock(&proc->brk_lock);
            return (int64_t)old_brk;
        }
    }

    proc->heap_brk = addr;
    spin_unlock(&proc->brk_lock);
    return (int64_t)addr;
}

/* System call handler for `mlock2_wrap`. */
int64_t sys_mlock2_wrap(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mlock(addr, length);
}

/* System call handler for `pkey_mprotect_wrap`. */
int64_t sys_pkey_mprotect_wrap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t pkey, uint64_t arg4, uint64_t arg5)
{
    (void)pkey;
    (void)arg4;
    (void)arg5;
    return sys_mprotect(addr, len, prot);
}

/* process_madvise syscall: apply advice to a process */
int64_t sys_process_madvise_impl(uint64_t pidfd, uint64_t iovec, uint64_t vlen, uint64_t advice, uint64_t flags, uint64_t arg5)
{
    (void)flags;
    (void)arg5;

    if (flags) return -EINVAL;
    if (pidfd >= CONFIG_PROCESS_MAX_FD) return -EBADF;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pid_file = process_fd_get(proc, (int)pidfd);
    if (!pid_file) return -EBADF;

    process_t *target = pidfd_get_target(pid_file->node);
    if (!target) {
        process_file_put(pid_file);
        return -EBADF;
    }

    /* Read iovec entries */
    struct {
            void  *iov_base;
            size_t iov_len;
    } iov[16];

    if (vlen > 16) vlen = 16;
    if (copy_from_user(iov, (const void *)iovec, vlen * sizeof(iov[0]))) {
        process_file_put(pid_file);
        return -EFAULT;
    }

    int result = 0;
    for (uint64_t i = 0; i < vlen; i++) {
        /* Validate advice per range. */
        switch (advice) {
            case 1 : // MADV_COLD
            case 2 : // MADV_PAGEOUT
                break;
            default :
                process_file_put(pid_file);
                return -EINVAL;
        }
        result++;
    }

    process_file_put(pid_file);
    return result;
}

/* mmap family wrappers (6-arg syscall -> actual function) */
int64_t sys_mprotect_wrap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mprotect(addr, length, prot);
}

/* System call handler for `mremap`. */
int64_t sys_mremap_wrap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr, uint64_t arg5)
{
    (void)arg5;
    return sys_mremap(old_addr, old_len, new_len, flags, new_addr);
}

/* System call handler for `msync`. */
int64_t sys_msync_wrap(uint64_t addr, uint64_t length, uint64_t flags, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_msync(addr, length, flags);
}

/* System call handler for `mincore`. */
int64_t sys_mincore_wrap(uint64_t addr, uint64_t length, uint64_t vec, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mincore(addr, length, vec);
}

/* System call handler for `madvise`. */
int64_t sys_madvise_wrap(uint64_t addr, uint64_t length, uint64_t advice, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_madvise(addr, length, advice);
}

/* System call handler for `mlock`. */
int64_t sys_mlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mlock(addr, length);
}

/* System call handler for `munlock`. */
int64_t sys_munlock_wrap(uint64_t addr, uint64_t length, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_munlock(addr, length);
}

/* System call handler for `mlockall`. */
int64_t sys_mlockall_wrap(uint64_t flags, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_mlockall(flags);
}

/* System call handler for `munlockall`. */
int64_t sys_munlockall_wrap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return sys_munlockall();
}

/* swapon syscall: enable a swap area */
int64_t sys_swapon(uint64_t path, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;
    if (!path) return -EFAULT;

    char name[CONFIG_VFS_PATH_MAX] = {0};
    if (strncpy_from_user(name, (const char *)path, sizeof(name)) < 0) return -EFAULT;
    return swap_activate_path(name, (uint32_t)flags);
}

/* swapoff syscall: disable a swap area */
int64_t sys_swapoff(uint64_t path, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (proc->uid != 0) return -EPERM;
    if (!path) return -EFAULT;

    char name[CONFIG_VFS_PATH_MAX] = {0};
    if (strncpy_from_user(name, (const char *)path, sizeof(name)) < 0) return -EFAULT;
    return swap_deactivate_path(name);
}

/* pkey_alloc syscall: unsupported */
int64_t sys_pkey_alloc_impl(uint64_t flags, uint64_t access_rights, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)flags;
    (void)access_rights;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS; // no PKU on this x86 config
}

/* pkey_free syscall: unsupported */
int64_t sys_pkey_free_impl(uint64_t pkey, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)pkey;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return -ENOSYS;
}
