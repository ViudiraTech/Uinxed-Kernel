/*
 *
 *      numa.c
 *      Task and anonymous-VMA NUMA memory policies
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <kernel/errno.h>
#include <libs/std/stdlib.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/page_walker.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/numa.h>

#define MPOL_F_NODE         1
#define MPOL_F_ADDR         2
#define MPOL_F_MEMS_ALLOWED 4

/* Linux's input maxnode is one greater than the number of significant bits. */
static int copy_nodemask(uint64_t user, uint64_t maxnode, nodemask_t *nodes)
{
    *nodes = 0;
    if (!user) return 0;
    if (!maxnode || maxnode > 32769) return -EINVAL;
    uint64_t bits = maxnode - 1;
    for (uint64_t at = 0; at < bits; at += 64) {
        uint64_t word = 0;
        if (user > UINT64_MAX - (at / 8) || copy_from_user(&word, (const void *)(user + (at / 8)), sizeof(word))) return -EFAULT;
        if (bits - at < 64) word &= (1ULL << (bits - at)) - 1;
        if (!at)
            *nodes = word;
        else if (word)
            return -EINVAL;
    }
    return 0;
}

static int make_policy(uint64_t mode, uint64_t user, uint64_t maxnode, numa_policy_t *policy)
{
    if (mode > NUMA_POLICY_LOCAL) return -EINVAL;
    policy->mode      = (uint32_t)mode;
    policy->preferred = 0;
    int result        = copy_nodemask(user, maxnode, &policy->nodes);
    if (result) return result;
    if (mode == NUMA_POLICY_PREFERRED && !policy->nodes) policy->mode = NUMA_POLICY_LOCAL;
    if (policy->nodes) policy->preferred = (uint16_t)__builtin_ctzll(policy->nodes);
    task_t *task = current_task();
    if (!task) return -ESRCH;
    return numa_policy_validate(policy, __atomic_load_n(&task->mems_allowed, __ATOMIC_ACQUIRE) & frame_memory_nodes());
}

int64_t sys_set_mempolicy(uint64_t mode, uint64_t nodes, uint64_t maxnode, uint64_t unused3, uint64_t unused4, uint64_t unused5)
{
    (void)unused3;
    (void)unused4;
    (void)unused5;
    numa_policy_t policy;
    int           result = make_policy(mode, nodes, maxnode, &policy);
    if (result) return result;
    uint64_t flags = spin_lock_irqsave(&scheduler.lock);
    task_t  *task  = current_task();
    result         = numa_policy_validate(&policy, task->mems_allowed & frame_memory_nodes());
    if (!result) {
        task->mempolicy            = policy;
        task->numa_interleave_next = 0;
    }
    spin_unlock_irqrestore(&scheduler.lock, flags);
    return result;
}

int64_t sys_get_mempolicy(uint64_t user_mode, uint64_t user_nodes, uint64_t maxnode, uint64_t address, uint64_t flags, uint64_t unused5)
{
    (void)unused5;
    if (flags & ~(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED)) return -EINVAL;
    if ((flags & MPOL_F_MEMS_ALLOWED) && flags != MPOL_F_MEMS_ALLOWED) return -EINVAL;
    if (address && !(flags & MPOL_F_ADDR)) return -EINVAL;
    task_t       *task   = current_task();
    numa_policy_t policy = task->mempolicy;
    if (flags & MPOL_F_ADDR) {
        process_t *proc = process_current();
        if (!proc) return -ESRCH;
        spin_lock(&proc->mmap_lock);
        vm_area_t *vma = proc->mmap_list;
        while (vma && vma->end <= address) vma = vma->next;
        if (!vma || address < vma->start) {
            spin_unlock(&proc->mmap_lock);
            return -EFAULT;
        }
        if (vma->mempolicy.mode != NUMA_POLICY_DEFAULT) policy = vma->mempolicy;
        spin_unlock(&proc->mmap_lock);
    }
    int32_t mode = (int32_t)policy.mode;
    if (flags & MPOL_F_NODE) {
        if (flags & MPOL_F_ADDR) {
            process_t *proc = process_current();
            uint8_t    byte;
            if (!proc || !proc->user_page_dir) return -ESRCH;
            if (copy_from_user(&byte, (const void *)address, 1)) return -EFAULT;
            spin_lock(&proc->user_page_dir->lock);
            uint64_t physical = walk_page_tables(proc->user_page_dir, address);
            mode              = physical ? numa_phys_node(physical) : -1;
            spin_unlock(&proc->user_page_dir->lock);
            if (mode < 0) return -EFAULT;
        } else {
            nodemask_t allowed = policy.nodes & task->mems_allowed & frame_memory_nodes();
            if (policy.mode != NUMA_POLICY_INTERLEAVE || !allowed) return -EINVAL;
            uint64_t ordinal = task->numa_interleave_next % popcount64(allowed);
            while (ordinal--) allowed &= allowed - 1;
            mode = __builtin_ctzll(allowed);
        }
    }
    nodemask_t nodes = flags & MPOL_F_MEMS_ALLOWED ? __atomic_load_n(&task->mems_allowed, __ATOMIC_ACQUIRE) : policy.nodes;
    if (user_nodes) {
        if (maxnode < numa_topology.nr_nodes || maxnode > 32768) return -EINVAL;
        size_t bytes = (size_t)((maxnode - 1 + 63) / 64) * sizeof(uint64_t);
        for (size_t at = 0; at < bytes; at += sizeof(uint64_t)) {
            uint64_t word = at ? 0 : nodes;
            if (user_nodes > UINT64_MAX - at || copy_to_user((void *)(user_nodes + at), &word, sizeof(word))) return -EFAULT;
        }
    }
    return user_mode && copy_to_user((void *)user_mode, &mode, sizeof(mode)) ? -EFAULT : 0;
}

/* Two preallocated boundary pieces make the VMA edit all-or-nothing. */
int64_t sys_mbind(uint64_t address, uint64_t length, uint64_t mode, uint64_t nodes, uint64_t maxnode, uint64_t flags)
{
    if (flags) return -EOPNOTSUPP; // no false success for strict checking or migration
    if ((address & (PAGE_4K_SIZE - 1)) || length > UINT64_MAX - (PAGE_4K_SIZE - 1)) return -EINVAL;
    length = ALIGN_UP(length, PAGE_4K_SIZE);
    if (address >= PROCESS_USER_STACK_TOP || length > PROCESS_USER_STACK_TOP - address) return -EINVAL;
    numa_policy_t policy;
    int           result = make_policy(mode, nodes, maxnode, &policy);
    if (result || !length) return result;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    vm_area_t *lower = malloc(sizeof(*lower)), *upper = malloc(sizeof(*upper));
    if (!lower || !upper) {
        free(lower);
        free(upper);
        return -ENOMEM;
    }
    uint64_t end = address + length;
    spin_lock(&proc->mmap_lock);
    vm_area_t **link = &proc->mmap_list;
    while (*link && (*link)->end <= address) link = &(*link)->next;
    vm_area_t *first  = *link;
    uint64_t   cursor = address;
    for (vm_area_t *vma = first; cursor < end; vma = vma->next) {
        if (!vma || vma->start > cursor) {
            result = -EFAULT;
            goto out;
        }
        if (vma->vm_file || vma->vm_private_data || (vma->flags & VM_SHARED)) {
            result = -EOPNOTSUPP;
            goto out;
        }
        cursor = vma->end;
    }
    if (!first) {
        result = -EFAULT;
        goto out;
    }
    if (first->start < address) {
        *lower       = *first;
        lower->end   = address;
        lower->next  = first;
        *link        = lower;
        lower        = NULL;
        first->start = address;
    }
    for (vm_area_t *vma = first; vma && vma->start < end; vma = vma->next) {
        if (vma->end > end) {
            *upper       = *vma;
            upper->start = end;
            vma->end     = end;
            vma->next    = upper;
            upper        = NULL;
        }
        vma->mempolicy = policy;
    }
out:
    spin_unlock(&proc->mmap_lock);
    free(lower);
    free(upper);
    return result;
}
