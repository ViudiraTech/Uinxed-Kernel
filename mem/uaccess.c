/*
 *
 *      uaccess.c
 *      User memory access helpers
 *
 *      2026/7/20 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/uaccess.h>

/*
 * The active process page table already maps both the kernel and userspace.
 * Let the CPU and its TLB perform the translation instead of taking the page
 * table lock and walking four levels in software for every small read/write.
 * A kernel-mode #PF redirects to the fixup label below when the address cannot
 * be resolved.  rep movsb is restartable, so ordinary demand/COW faults resume
 * at the exact byte where they stopped.
 */
extern int  __uaccess_copy_direct(void *dst, const void *src, size_t size);
extern void __uaccess_copy_fault(void);

__asm__(".text\n"
        ".global __uaccess_copy_direct\n"
        ".type __uaccess_copy_direct, @function\n"
        "__uaccess_copy_direct:\n"
        "cld\n"
        "movq %rdx, %rcx\n"
        "rep movsb\n"
        "xorl %eax, %eax\n"
        "ret\n"
        ".size __uaccess_copy_direct, .-__uaccess_copy_direct\n"
        ".global __uaccess_copy_fault\n"
        ".type __uaccess_copy_fault, @function\n"
        "__uaccess_copy_fault:\n"
        "ret\n"
        ".size __uaccess_copy_fault, .-__uaccess_copy_fault\n");

static int copy_user_direct(void *dst, const void *src, size_t size, int nofault)
{
    task_t *task = current_task();
    if (!task) return -EFAULT;

    /*
     * Preserve an outer fixup in case an interrupt handler performs uaccess
     * while a task was interrupted inside another user copy.
     */
    uintptr_t old_resume        = task->uaccess_fault_resume;
    uint8_t   old_nofault       = task->uaccess_fault_nofault;
    task->uaccess_fault_nofault = nofault != 0;
    task->uaccess_fault_resume  = (uintptr_t)__uaccess_copy_fault;
    __asm__ volatile("" ::: "memory");
    int ret = __uaccess_copy_direct(dst, src, size);
    __asm__ volatile("" ::: "memory");
    task->uaccess_fault_resume  = old_resume;
    task->uaccess_fault_nofault = old_nofault;
    return ret;
}

int user_range_ok(const void *uaddr, size_t size)
{
    uintptr_t addr = (uintptr_t)uaddr;
    if (!size) return 1;
    if (!addr) return 0;
    if (addr >= PROCESS_USER_STACK_TOP) return 0;
    if (addr > PROCESS_USER_STACK_TOP - size) return 0;
    return 1;
}

static int check_entry(uint64_t entry, int write)
{
    if (!(entry & PTE_PRESENT)) return 0;
    if (!(entry & PTE_USER)) return 0;
    if (write && !(entry & PTE_WRITEABLE)) return 0;
    return 1;
}

static int user_translate(process_t *proc, uintptr_t uaddr, int write, void **kaddr, size_t *page_left)
{
    if (!proc || !proc->user_page_dir || !proc->user_page_dir->table) return 0;

    uint16_t l4i = (uaddr >> 39) & 0x1ff;
    uint16_t l3i = (uaddr >> 30) & 0x1ff;
    uint16_t l2i = (uaddr >> 21) & 0x1ff;
    uint16_t l1i = (uaddr >> 12) & 0x1ff;

    page_table_t *l4  = proc->user_page_dir->table;
    uint64_t      l4e = l4->entries[l4i].value;
    if (!check_entry(l4e, write)) return 0;

    page_table_t *l3  = phys_to_virt(l4e & PAGE_4K_MASK);
    uint64_t      l3e = l3->entries[l3i].value;
    if (!check_entry(l3e, write)) return 0;
    if (l3e & PTE_HUGE) {
        uintptr_t off = uaddr & (PAGE_1G_SIZE - 1);
        *kaddr        = phys_to_virt((l3e & PAGE_1G_MASK) + off);
        *page_left    = PAGE_1G_SIZE - off;
        return 1;
    }

    page_table_t *l2  = phys_to_virt(l3e & PAGE_4K_MASK);
    uint64_t      l2e = l2->entries[l2i].value;
    if (!check_entry(l2e, write)) return 0;
    if (l2e & PTE_HUGE) {
        uintptr_t off = uaddr & (PAGE_2M_SIZE - 1);
        *kaddr        = phys_to_virt((l2e & PAGE_2M_MASK) + off);
        *page_left    = PAGE_2M_SIZE - off;
        return 1;
    }

    page_table_t *l1  = phys_to_virt(l2e & PAGE_4K_MASK);
    uint64_t      l1e = l1->entries[l1i].value;
    if (!check_entry(l1e, write)) return 0;

    uintptr_t off = uaddr & (PAGE_4K_SIZE - 1);
    *kaddr        = phys_to_virt((l1e & PAGE_4K_MASK) + off);
    *page_left    = PAGE_4K_SIZE - off;
    return 1;
}

/*
 * A private writable mapping is deliberately made read-only when an address
 * space is cloned.  Kernel writes through copy_to_user() must take the same
 * COW fault path as a user-mode store; writing through the direct-map alias
 * without resolving COW would corrupt the parent's page, while rejecting the
 * mapping would spuriously return EFAULT for perfectly valid user memory.
 *
 * Resolve only after the ordinary writable walk fails.  This preserves the
 * fast path for writable mappings and ensures that genuinely read-only or
 * unmapped memory remains inaccessible.  The second walk is required because
 * page_resolve_cow_fault() may replace the physical leaf, and also closes the
 * race with another thread resolving the same mapping concurrently.
 */
static int user_translate_writable(process_t *proc, uintptr_t uaddr, void **kaddr, size_t *page_left)
{
    if (user_translate(proc, uaddr, 1, kaddr, page_left)) return 1;

    if (!proc || !proc->user_page_dir) return 0;
    if (page_resolve_cow_fault(proc, uaddr) < 0) return 0;

    return user_translate(proc, uaddr, 1, kaddr, page_left);
}

static int user_translate_access(process_t *proc, uintptr_t uaddr, int write, void **kaddr, size_t *page_left)
{
    if (write) {
        if (user_translate_writable(proc, uaddr, kaddr, page_left)) return 1;
        if (proc && process_demand_fault(proc, uaddr, 1, 0) == 0 && user_translate_writable(proc, uaddr, kaddr, page_left)) return 1;
        return 0;
    }
    if (user_translate(proc, uaddr, 0, kaddr, page_left)) return 1;
    if (proc && process_demand_fault(proc, uaddr, 0, 0) == 0 && user_translate(proc, uaddr, 0, kaddr, page_left)) return 1;
    return 0;
}

int user_access_ok_process(process_t *proc, const void *uaddr, size_t size, int write)
{
    uintptr_t cur = (uintptr_t)uaddr;
    size_t    remaining;

    if (!proc || !user_range_ok(uaddr, size)) return 0;
    remaining = size;

    while (remaining) {
        void  *kaddr;
        size_t page_left;
        if (!user_translate_access(proc, cur, write, &kaddr, &page_left)) return 0;
        (void)kaddr;

        size_t step = remaining < page_left ? remaining : page_left;
        cur += step;
        remaining -= step;
    }
    return 1;
}

int user_access_ok(const void *uaddr, size_t size, int write)
{
    return user_access_ok_process(process_current(), uaddr, size, write);
}

static int copy_user_bytes(void *dst, const void *src, size_t size, int to_user)
{
    uintptr_t  user = (uintptr_t)(to_user ? dst : src);
    uintptr_t  kern = (uintptr_t)(to_user ? src : dst);
    process_t *proc = process_current();
    size_t     remaining;

    if (!proc || !proc->user_page_dir || !user_range_ok((const void *)user, size)) return -EFAULT;
    remaining = size;

    while (remaining) {
        void  *kaddr;
        size_t page_left;

        /*
         * Translate once while holding the page-table lock and keep the leaf
         * alive through the copy.  The old fast path walked all four page
         * table levels before taking this lock, then immediately walked them
         * again to close the COW/munmap race.
         */
        spin_lock(&proc->user_page_dir->lock);
        if (!user_translate(proc, user, to_user, &kaddr, &page_left)) {
            spin_unlock(&proc->user_page_dir->lock);
            /*
             * Fault handling may allocate, copy pages, or shoot down TLBs and
             * therefore must stay outside the page-table lock.
             */
            if (to_user && page_resolve_cow_fault(proc, user) == 0) continue;
            if (process_demand_fault(proc, user, to_user, 0) == 0) continue;
            plogk("uaccess: Copy %s fault at %p (size %zu, remaining %zu)\n", to_user ? "to_user" : "from_user", (void *)user, size, remaining);
            return -EFAULT;
        }

        size_t step = remaining < page_left ? remaining : page_left;
        if (to_user) {
            memcpy(kaddr, (const void *)kern, step);
        } else {
            memcpy((void *)kern, kaddr, step);
        }
        spin_unlock(&proc->user_page_dir->lock);
        user += step;
        kern += step;
        remaining -= step;
    }
    return 0;
}

/*
 * Copy through already-present user mappings without invoking the demand or
 * COW fault paths.  Callers that hold an object lock can use this for their
 * common path, drop that lock and fault the range with user_access_ok_process
 * only when this reports EFAULT, then retry without exposing half-committed
 * object state.
 */
static int copy_user_bytes_process_nofault(process_t *proc, void *dst, const void *src, size_t size, int to_user)
{
    uintptr_t user = (uintptr_t)(to_user ? dst : src);
    uintptr_t kern = (uintptr_t)(to_user ? src : dst);
    size_t    remaining;

    if (!proc || !proc->user_page_dir || !user_range_ok((const void *)user, size)) return -EFAULT;
    remaining = size;

    while (remaining) {
        void  *kaddr;
        size_t page_left;

        spin_lock(&proc->user_page_dir->lock);
        if (!user_translate(proc, user, to_user, &kaddr, &page_left)) {
            spin_unlock(&proc->user_page_dir->lock);
            return -EFAULT;
        }

        size_t step = remaining < page_left ? remaining : page_left;
        if (to_user)
            memcpy(kaddr, (const void *)kern, step);
        else
            memcpy((void *)kern, kaddr, step);
        spin_unlock(&proc->user_page_dir->lock);

        user += step;
        kern += step;
        remaining -= step;
    }
    return 0;
}

int copy_from_user(void *dst, const void *src, size_t size)
{
    return copy_user_bytes(dst, src, size, 0);
}

int copy_to_user(void *dst, const void *src, size_t size)
{
    return copy_user_bytes(dst, src, size, 1);
}

int copy_from_user_process_nofault(process_t *proc, void *dst, const void *src, size_t size)
{
    return copy_user_bytes_process_nofault(proc, dst, src, size, 0);
}

int copy_to_user_process_nofault(process_t *proc, void *dst, const void *src, size_t size)
{
    return copy_user_bytes_process_nofault(proc, dst, src, size, 1);
}

int strnlen_user(const char *src, size_t max_size)
{
    if (!src || !max_size) return -EFAULT;

    char   buffer[256];
    size_t copied = 0;
    while (copied < max_size) {
        size_t count = max_size - copied;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        size_t page_left = PAGE_4K_SIZE - (((uintptr_t)src + copied) & (PAGE_4K_SIZE - 1));
        if (count > page_left) count = page_left;
        int ret = copy_from_user(buffer, src + copied, count);
        if (ret) return ret;
        char *end = memchr(buffer, '\0', count);
        if (end) return (int)(copied + (size_t)(end - buffer) + 1);
        copied += count;
    }
    return -ENAMETOOLONG;
}

int strncpy_from_user(char *dst, const char *src, size_t max_size)
{
    if (!dst || !src || !max_size) return -EFAULT;

    char   buffer[256];
    size_t copied = 0;
    while (copied < max_size) {
        size_t count = max_size - copied;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        size_t page_left = PAGE_4K_SIZE - (((uintptr_t)src + copied) & (PAGE_4K_SIZE - 1));
        if (count > page_left) count = page_left;
        int ret = copy_from_user(buffer, src + copied, count);
        if (ret) return ret;

        char  *end    = memchr(buffer, '\0', count);
        size_t amount = end ? (size_t)(end - buffer) + 1 : count;
        memcpy(dst + copied, buffer, amount);
        if (end) return (int)(copied + amount - 1);
        copied += amount;
    }
    dst[max_size - 1] = '\0';
    return -ENAMETOOLONG;
}
