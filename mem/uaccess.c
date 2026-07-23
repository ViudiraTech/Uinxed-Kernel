/*
 *
 *      uaccess.c
 *      User memory access helpers
 *
 *      2026/7/20 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
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

int user_access_ok(const void *uaddr, size_t size, int write)
{
    uintptr_t cur = (uintptr_t)uaddr;
    size_t    remaining;

    if (!user_ptr_range_ok(cur, size)) return 0;
    remaining = size;

    while (remaining) {
        void  *kaddr;
        size_t page_left;
        if (!user_translate(cur, write, &kaddr, &page_left)) return 0;
        (void)kaddr;

        size_t step = remaining < page_left ? remaining : page_left;
        cur += step;
        remaining -= step;
    }
    return 1;
}

static int copy_user_bytes(void *dst, const void *src, size_t size, int to_user)
{
    uintptr_t user = (uintptr_t)(to_user ? dst : src);
    uintptr_t kern = (uintptr_t)(to_user ? src : dst);
    size_t    remaining;

    if (!user_ptr_range_ok(user, size)) return -EFAULT;
    remaining = size;

    while (remaining) {
        void  *kaddr;
        size_t page_left;
        if (!user_translate(user, to_user, &kaddr, &page_left)) return -EFAULT;

        size_t step = remaining < page_left ? remaining : page_left;
        if (to_user) {
            memcpy(kaddr, (const void *)kern, step);
        } else {
            memcpy((void *)kern, kaddr, step);
        }
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
