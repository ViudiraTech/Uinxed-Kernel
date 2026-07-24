/*
 *
 *      mmap.c
 *      Memory mapping subsystem implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/vfs.h>
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
#include <mem/page_walker.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/mmap.h>
#include <syscall/syscall.h>

#define MMAP_BASE_ADDR     0x00007f0000000000ULL
#define MMAP_END_ADDR      0x00007fffffffffffULL
#define MMAP_DEFAULT_ALIGN PAGE_4K_SIZE

/* Convert Linux mmap prot to internal vm_flags_t */
static vm_flags_t prot_to_vm_flags(uint64_t prot)
{
    vm_flags_t f = 0;
    if (prot & PROT_READ) f |= VM_READ;
    if (prot & PROT_WRITE) f |= VM_WRITE;
    if (prot & PROT_EXEC) f |= VM_EXEC;
    return f;
}

/* Convert vm_flags_t to PTE flags */
static uint64_t vm_flags_to_pte(vm_flags_t flags)
{
    uint64_t pte = PTE_USER | PTE_PRESENT;
    if (flags & VM_WRITE) pte |= PTE_WRITEABLE;
    if (!(flags & VM_EXEC)) pte |= PTE_NO_EXECUTE;
    return pte;
}

/* Find a free virtual address range for mmap */
static uintptr_t find_free_vma_range(process_t *proc, size_t length)
{
    uintptr_t addr  = MMAP_BASE_ADDR;
    size_t    pages = ALIGN_UP(length, PAGE_4K_SIZE);

    spin_lock(&proc->mmap_lock);

    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (addr + pages <= vma->start) {
            spin_unlock(&proc->mmap_lock);
            return addr;
        }
        if (vma->end > addr) addr = vma->end;
        addr = ALIGN_UP(addr, PAGE_4K_SIZE);
    }

    spin_unlock(&proc->mmap_lock);

    if (addr + pages <= MMAP_END_ADDR) return addr;
    return 0;
}

/* Check if a VMA range overlaps with any existing VMA */
static int vma_range_overlaps(process_t *proc, uintptr_t start, uintptr_t end)
{
    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (start < vma->end && end > vma->start) {
            spin_unlock(&proc->mmap_lock);
            return 1;
        }
    }
    spin_unlock(&proc->mmap_lock);
    return 0;
}

/* Remove and free VMA entries that overlap with the given range.
 * Returns the number of VMAs removed. */
static int vma_remove_range(process_t *proc, uintptr_t start, uintptr_t end)
{
    int removed = 0;

    spin_lock(&proc->mmap_lock);
    vm_area_t **prev = &proc->mmap_list;
    while (*prev) {
        vm_area_t *vma = *prev;
        if (vma->start >= start && vma->end <= end) {
            *prev = vma->next;
            free(vma);
            removed++;
            continue;
        }
        prev = &vma->next;
    }
    spin_unlock(&proc->mmap_lock);

    return removed;
}

/* Unmap physical pages in a range from the page directory */
static void unmap_physical_pages(process_t *proc, uintptr_t start, size_t length)
{
    uintptr_t end = ALIGN_UP(start + length, PAGE_4K_SIZE);
    for (uintptr_t va = start; va < end; va += PAGE_4K_SIZE) {
        uintptr_t phys = walk_page_tables(proc->user_page_dir, va);
        if (phys && phys != (uintptr_t)-1) { free_frame(phys); }
    }
}

/* ---------- Full mmap syscall implementation ---------- */

int64_t sys_mmap_pgoff(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t pgoff)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    if (!length) return -EINVAL;
    if (length > UINT64_MAX - PAGE_4K_SIZE) return -EINVAL;

    size_t    pages = ALIGN_UP(length, PAGE_4K_SIZE);
    uintptr_t mmap_addr;

    /* Determine target address */
    if (addr && !(flags & MAP_FIXED)) {
        /* Hint address - use it if possible */
        if (!vma_range_overlaps(proc, addr, addr + pages)) {
            mmap_addr = addr;
        } else {
            mmap_addr = find_free_vma_range(proc, pages);
            if (!mmap_addr) return -ENOMEM;
        }
    } else if (flags & MAP_FIXED) {
        if (!addr) return -EINVAL;
        if (addr > UINT64_MAX - pages) return -EINVAL;
        if (addr + pages > PROCESS_USER_STACK_TOP) return -EINVAL;
        mmap_addr = addr;
        unmap_physical_pages(proc, mmap_addr, pages);
        vma_remove_range(proc, mmap_addr, mmap_addr + pages);
    } else {
        mmap_addr = find_free_vma_range(proc, pages);
        if (!mmap_addr) return -ENOMEM;
    }

    vm_flags_t vm_flags = prot_to_vm_flags(prot);
    if (flags & MAP_SHARED) vm_flags |= VM_SHARED;

    /* File-backed mapping */
    if (!(flags & MAP_ANONYMOUS) && (int64_t)fd >= 0) {
        process_file_t *file = NULL;
        spin_lock(&proc->fd_lock);
        if ((int64_t)fd < PROCESS_MAX_FD) {
            file = proc->fds[(int)fd];
            if (file) {
                spin_lock(&file->lock);
                file->refcount++;
                spin_unlock(&file->lock);
            }
        }
        spin_unlock(&proc->fd_lock);

        if (!file) return -EBADF;

        if ((size_t)pgoff > SIZE_MAX / PAGE_4K_SIZE) {
            process_file_put(file);
            return -EINVAL;
        }
        size_t file_offset = (size_t)pgoff * PAGE_4K_SIZE;

        /* Check if the filesystem has a per-open mmap callback
         * (e.g., DRM GEM mmap). If so, use it to map device
         * backing pages directly instead of reading file content. */
        if (callbackof(file->node, file_mmap)) {
            vm_area_t vma;
            memset(&vma, 0, sizeof(vma));
            vma.start    = mmap_addr;
            vma.end      = mmap_addr + pages;
            vma.flags    = vm_flags;
            vma.type     = VM_REGION_MMAP;
            vma.vm_file  = file->node;
            vma.vm_pgoff = pgoff;
            vma.vm_file->refcount++; /* VMA holds a reference */

            void *result = callbackof(file->node, file_mmap)(file->node, file->private_data, file_offset, pages, vm_flags, &vma);
            if (!result) {
                vma.vm_file->refcount--;
                process_file_put(file);
                return -ENODEV;
            }

            vm_area_insert(proc, &vma);
            process_file_put(file);
            goto vma_done;
        }

        /* Fallback: read file content into new frames. */
        {
            uint64_t pte_flags = vm_flags_to_pte(vm_flags);

            for (size_t i = 0; i < pages; i += PAGE_4K_SIZE) {
                uint64_t frame = alloc_frames(1);
                if (!frame) {
                    process_file_put(file);
                    return -ENOMEM;
                }

                void *virt = phys_to_virt(frame);
                memset(virt, 0, PAGE_4K_SIZE);

                size_t read_offset = file_offset + i;
                size_t to_read     = PAGE_4K_SIZE;
                if (read_offset < file->node->size) {
                    if (read_offset + to_read > file->node->size) { to_read = file->node->size - read_offset; }
                    vfs_read(file->node, virt, read_offset, to_read);
                }

                page_map_to(proc->user_page_dir, mmap_addr + i, frame, pte_flags);
            }
        }

        /* Record the VMA */
        vm_area_t *vma = calloc(1, sizeof(vm_area_t));
        if (!vma) {
            process_file_put(file);
            return -ENOMEM;
        }
        vma->start = mmap_addr;
        vma->end   = mmap_addr + pages;
        vma->flags = vm_flags;
        vma->type  = VM_REGION_MMAP;
        vma->next  = NULL;

        spin_lock(&proc->mmap_lock);
        vma->next       = proc->mmap_list;
        proc->mmap_list = vma;
        spin_unlock(&proc->mmap_lock);

        process_file_put(file);
        return (int64_t)mmap_addr;
    }

vma_done:
    return (int64_t)mmap_addr;
}

int sys_munmap_full(uint64_t addr, uint64_t length)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!length || (addr & (PAGE_4K_SIZE - 1))) return -EINVAL;
    if (length > UINT64_MAX - PAGE_4K_SIZE) return -EINVAL;

    size_t pages = ALIGN_UP(length, PAGE_4K_SIZE);
    unmap_physical_pages(proc, (uintptr_t)addr, pages);
    vma_remove_range(proc, (uintptr_t)addr, (uintptr_t)addr + pages);

    return process_munmap(proc, (uintptr_t)addr, length);
}

int sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!length || (addr & (PAGE_4K_SIZE - 1))) return -EINVAL;

    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;

    vm_flags_t vm_flags  = prot_to_vm_flags(prot);
    uint64_t   pte_flags = vm_flags_to_pte(vm_flags);
    size_t     pages     = ALIGN_UP(length, PAGE_4K_SIZE);

    /* Update VMA flags */
    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (vma->start == (uintptr_t)addr) {
            vma->flags = vm_flags;
            break;
        }
    }
    spin_unlock(&proc->mmap_lock);

    /* Update page table entries */
    for (size_t i = 0; i < pages; i += PAGE_4K_SIZE) {
        uintptr_t va   = (uintptr_t)addr + i;
        uintptr_t phys = walk_page_tables(proc->user_page_dir, va);
        if (phys && phys != (uintptr_t)-1) { page_map_to(proc->user_page_dir, va, phys, pte_flags); }
    }

    return EOK;
}

int sys_msync(uint64_t addr, uint64_t length, uint64_t flags)
{
    (void)addr;
    (void)length;
    (void)flags;
    /* For a non-MMU-writeback kernel, msync is a no-op */
    return EOK;
}

int sys_madvise(uint64_t addr, uint64_t length, uint64_t advice)
{
    (void)addr;
    (void)length;
    (void)advice;
    /* madvise is advisory - accept all hints */
    return EOK;
}

int sys_mlock(uint64_t addr, uint64_t length)
{
    if (!addr || !length) return -EINVAL;
    /* mlock is accepted but pages are already pinned in this kernel */
    return EOK;
}

int sys_munlock(uint64_t addr, uint64_t length)
{
    if (!addr || !length) return -EINVAL;
    return EOK;
}

int sys_mlockall(uint64_t flags)
{
    (void)flags;
    return EOK;
}

int sys_munlockall(void)
{
    return EOK;
}

int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags, uint64_t new_addr)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!old_addr || !old_len) return -EINVAL;
    if (old_len > UINT64_MAX - PAGE_4K_SIZE || new_len > UINT64_MAX - PAGE_4K_SIZE) return -EINVAL;

    size_t old_pages = ALIGN_UP(old_len, PAGE_4K_SIZE);
    size_t new_pages = ALIGN_UP(new_len, PAGE_4K_SIZE);

    if (new_len <= old_len) {
        if (new_len < old_len) { unmap_physical_pages(proc, (uintptr_t)old_addr + new_pages, old_pages - new_pages); }
        return (int64_t)old_addr;
    }

    /* Expanding: try to extend in-place if possible */
    uintptr_t target = (uintptr_t)old_addr;
    if (flags & 0x1) { /* MREMAP_MAYMOVE */
        if (new_addr) {
            if (new_addr > UINT64_MAX - new_pages) return -EINVAL;
            if (new_addr + new_pages > PROCESS_USER_STACK_TOP) return -EINVAL;
            target = (uintptr_t)new_addr;
        } else if (vma_range_overlaps(proc, old_addr + old_pages, old_addr + new_pages)) {
            target = find_free_vma_range(proc, new_pages);
            if (!target) return -ENOMEM;
        }
    } else {
        if (vma_range_overlaps(proc, old_addr + old_pages, old_addr + new_pages)) { return -ENOMEM; }
    }

    /* Map additional pages */
    vm_flags_t vm_flags = VM_READ | VM_WRITE;
    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (vma->start == (uintptr_t)old_addr) {
            vm_flags = vma->flags;
            break;
        }
    }
    spin_unlock(&proc->mmap_lock);

    uint64_t pte_flags = vm_flags_to_pte(vm_flags);
    for (size_t i = old_pages; i < new_pages; i += PAGE_4K_SIZE) {
        uint64_t frame = alloc_frames(1);
        if (!frame) return -ENOMEM;
        memset(phys_to_virt(frame), 0, PAGE_4K_SIZE);
        page_map_to(proc->user_page_dir, target + i, frame, pte_flags);
    }

    /* Update VMA */
    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (vma->start == (uintptr_t)old_addr) {
            vma->end   = target + new_pages;
            vma->start = target;
            break;
        }
    }
    spin_unlock(&proc->mmap_lock);

    return (int64_t)target;
}

int sys_mincore(uint64_t addr, uint64_t length, uint64_t vec)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (!vec) return -EFAULT;

    size_t   pages = ALIGN_UP(length, PAGE_4K_SIZE) / PAGE_4K_SIZE;
    uint8_t *residency;

    residency = malloc(pages);
    if (!residency) return -ENOMEM;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t phys = walk_page_tables(proc->user_page_dir, (uintptr_t)addr + i * PAGE_4K_SIZE);
        residency[i]   = (phys && phys != (uintptr_t)-1) ? 1 : 0;
    }

    int ret = copy_to_user((void *)vec, residency, pages) ? -EFAULT : EOK;
    free(residency);
    return ret;
}

void mmap_init(void)
{
}