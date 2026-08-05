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
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/uaccess.h>

static int user_ptr_range_ok(uintptr_t addr, size_t size)
{
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

static int user_translate(uintptr_t uaddr, int write, void **kaddr, size_t *page_left)
{
    process_t *proc = process_current();
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
static int user_translate_writable(uintptr_t uaddr, void **kaddr, size_t *page_left)
{
    if (user_translate(uaddr, 1, kaddr, page_left)) return 1;

    process_t *proc = process_current();
    if (!proc || !proc->user_page_dir) return 0;
    if (page_resolve_cow_fault(proc, uaddr) < 0) return 0;

    return user_translate(uaddr, 1, kaddr, page_left);
}

static int user_translate_access(uintptr_t uaddr, int write, void **kaddr, size_t *page_left)
{
    if (write) {
        if (user_translate_writable(uaddr, kaddr, page_left)) return 1;
        process_t *proc = process_current();
        if (proc && process_demand_fault(proc, uaddr, 1, 0) == 0 && user_translate_writable(uaddr, kaddr, page_left)) return 1;
        return 0;
    }
    if (user_translate(uaddr, 0, kaddr, page_left)) return 1;
    process_t *proc = process_current();
    if (proc && process_demand_fault(proc, uaddr, 0, 0) == 0 && user_translate(uaddr, 0, kaddr, page_left)) return 1;
    return 0;
}

int user_access_ok(const void *uaddr, size_t size, int write)
{
    uintptr_t cur = (uintptr_t)uaddr;
    size_t    remaining;

    if (!user_ptr_range_ok(cur, size)) return 0;
    remaining = size;

    while (remaining) {
        void  *kaddr;
        size_t page_left;
        if (!user_translate_access(cur, write, &kaddr, &page_left)) return 0;
        (void)kaddr;

        size_t step = remaining < page_left ? remaining : page_left;
        cur += step;
        remaining -= step;
    }
    return 1;
}

static int copy_user_bytes(void *dst, const void *src, size_t size, int to_user)
{
    uintptr_t  user = (uintptr_t)(to_user ? dst : src);
    uintptr_t  kern = (uintptr_t)(to_user ? src : dst);
    process_t *proc = process_current();
    size_t     remaining;

    if (!proc || !proc->user_page_dir || !user_ptr_range_ok(user, size)) return -EFAULT;
    remaining = size;

    while (remaining) {
        void  *kaddr;
        size_t page_left;
        if (!user_translate_access(user, to_user, &kaddr, &page_left)) return -EFAULT;

        /* Keep the leaf mapping alive until the direct-map copy completes.
         * A concurrent COW or munmap may otherwise release and reuse the
         * translated frame between the page-table walk and memcpy(). */
        spin_lock(&proc->user_page_dir->lock);
        if (!user_translate(user, to_user, &kaddr, &page_left)) {
            spin_unlock(&proc->user_page_dir->lock);
            continue;
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

int copy_from_user(void *dst, const void *src, size_t size)
{
    return copy_user_bytes(dst, src, size, 0);
}

int copy_to_user(void *dst, const void *src, size_t size)
{
    return copy_user_bytes(dst, src, size, 1);
}

int strncpy_from_user(char *dst, const char *src, size_t max_size)
{
    if (!dst || !src || !max_size) return -EFAULT;

    for (size_t i = 0; i < max_size; i++) {
        int ret = copy_from_user(dst + i, src + i, 1);
        if (ret) return ret;
        if (!dst[i]) return (int)i;
    }
    dst[max_size - 1] = '\0';
    return -ENAMETOOLONG;
}
