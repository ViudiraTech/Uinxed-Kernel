/*
 *
 *      page.c
 *      Memory pages
 *
 *      2025/2/16 By XIAOYI12
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/cpuid.h>
#include <arch/smp.h>
#include <kernel/debug/debug.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/swap.h>
#include <process/process.h>
#include <process/sched.h>
#include <sync/signal.h>
#include <syscall/syscall.h>

page_directory_t  kernel_page_dir;
page_directory_t *current_directory = 0;
static void       page_enable_global_tlb(void);

/*
 * GCC/Clang interrupt functions save only the registers selected by their
 * optimiser.  Their slots therefore cannot be addressed by fixed offsets
 * from interrupt_frame_t.  Signals need to replace RIP/RSP and several GPRs,
 * so #PF uses a small assembly entry that defines one stable full frame.
 */
typedef struct page_fault_frame {
        uint64_t r15;
        uint64_t r14;
        uint64_t r13;
        uint64_t r12;
        uint64_t r11;
        uint64_t r10;
        uint64_t r9;
        uint64_t r8;
        uint64_t rbp;
        uint64_t rdi;
        uint64_t rsi;
        uint64_t rdx;
        uint64_t rcx;
        uint64_t rbx;
        uint64_t rax;
        uint64_t error_code;
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
} page_fault_frame_t;

_Static_assert(offsetof(page_fault_frame_t, error_code) == 15 * sizeof(uint64_t), "bad #PF frame layout");
_Static_assert(offsetof(page_fault_frame_t, rip) == 16 * sizeof(uint64_t), "bad #PF iret layout");

void page_fault_handle_frame(page_fault_frame_t *frame) __attribute__((used, noinline));

__asm__(".text\n"
        ".global page_fault_entry\n"
        ".type page_fault_entry, @function\n"
        "page_fault_entry:\n"
        "cld\n"
        "testb $3, 16(%rsp)\n" // frame->cs & 3 (error_code, rip, cs at 0/8/16)
        "jz 1f\n"
        "swapgs\n" // user #PF: switch %gs to the per-CPU base
        "1:\n"
        "pushq %rax\n"
        "pushq %rbx\n"
        "pushq %rcx\n"
        "pushq %rdx\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        "pushq %rbp\n"
        "pushq %r8\n"
        "pushq %r9\n"
        "pushq %r10\n"
        "pushq %r11\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "movq %rsp, %r12\n"
        "movq %r12, %rdi\n"
        "andq $-16, %rsp\n"
        "call page_fault_handle_frame\n"
        "movq %r12, %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %r11\n"
        "popq %r10\n"
        "popq %r9\n"
        "popq %r8\n"
        "popq %rbp\n"
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %rcx\n"
        "popq %rbx\n"
        "popq %rax\n"
        "addq $8, %rsp\n"
        "testb $3, 8(%rsp)\n" // frame->cs & 3 (rip, cs at 0/8)
        "jz 2f\n"
        "cli\n"
        "swapgs\n" // restore the user GS before returning
        "2:\n"
        "iretq\n"
        ".size page_fault_entry, .-page_fault_entry\n");

/* Page fault handling */
void page_fault_handle_frame(page_fault_frame_t *frame)
{
    disable_intr();

    uint64_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));

    uint64_t    error_code = frame->error_code;
    uint64_t    present    = error_code & 0x1;  // Page exists, access violated protection
    uint64_t    rw         = error_code & 0x2;  // Write access
    uint64_t    us         = error_code & 0x4;  // Fault from user mode
    uint64_t    reserved   = error_code & 0x8;  // Reserved bits were set
    uint64_t    id         = error_code & 0x10; // Instruction fetch
    const char *pf_msg     = present ? "Protection" : "NotPresent";

    if (reserved)
        pf_msg = "Reserved";
    else if (id)
        pf_msg = "InstructionFetch";
    else if (rw && present)
        pf_msg = "ReadOnly";

    carry_error_code = 1; // carry error code

    /*
     * copy_{to,from}_user() deliberately accesses the user virtual address
     * through the current hardware page table.  Resolve ordinary demand/COW
     * faults just like a ring-3 access.  A nofault copy, or an invalid range,
     * returns -EFAULT through the assembly fixup instead of panicking the
     * kernel.
     */
    task_t *fault_task = current_task();
    if (!us && !reserved && !id && fault_task && fault_task->uaccess_fault_resume && faulting_address && faulting_address < PROCESS_USER_STACK_TOP) {
        process_t *proc = fault_task->process;
        if (!fault_task->uaccess_fault_nofault && proc && proc->user_page_dir) {
            if (present && rw && page_resolve_write_fault(proc, faulting_address) == 0) return;
            if (!present && swap_fault(proc->user_page_dir, faulting_address) == 0) return;
            if (!present && process_demand_fault(proc, faulting_address, rw, 0) == 0) return;
        }
        frame->rax = (uint64_t)(int64_t)-EFAULT;
        frame->rip = fault_task->uaccess_fault_resume;
        return;
    }

    if (us) {
        process_t *proc = process_current();
        if (proc) {
            if (present && rw && !reserved && proc->user_page_dir && page_resolve_write_fault(proc, faulting_address) == 0) return;
            if (!present && !reserved && proc->user_page_dir && swap_fault(proc->user_page_dir, faulting_address) == 0) return;
            if (!present && !reserved && proc->user_page_dir && process_demand_fault(proc, faulting_address, rw, id) == 0) return;

            siginfo_t info = {0};
            info.si_signo  = SIGSEGV;
            info.si_code   = present ? SEGV_ACCERR : SEGV_MAPERR;
            info.si_addr   = (void *)faulting_address;

            plogk("#PF (pid=%llu task=%s): addr=0x%016llx rip=0x%016llx rsp=0x%016llx cs=0x%llx err=0x%llx\n", fault_task->pid, fault_task->name, faulting_address, frame->rip, frame->rsp, frame->cs,
                  error_code);
            /*
             * A synchronous fault cannot be deferred.  If SIGSEGV is
             * blocked (normally because its handler faulted recursively) or
             * ignored, terminate the process instead of retrying the same
             * faulting instruction forever.
             */
            if (signal_is_blocked_or_ignored(proc, SIGSEGV)) process_exit_group(-SIGSEGV);

            signal_send_thread(fault_task, SIGSEGV, &info);

            syscall_frame_t sigframe = {0};
            sigframe.rax             = frame->rax;
            sigframe.rbx             = frame->rbx;
            sigframe.rcx             = frame->rcx;
            sigframe.rdx             = frame->rdx;
            sigframe.rsi             = frame->rsi;
            sigframe.rdi             = frame->rdi;
            sigframe.rbp             = frame->rbp;
            sigframe.r8              = frame->r8;
            sigframe.r9              = frame->r9;
            sigframe.r10             = frame->r10;
            sigframe.r11             = frame->r11;
            sigframe.r12             = frame->r12;
            sigframe.r13             = frame->r13;
            sigframe.r14             = frame->r14;
            sigframe.r15             = frame->r15;
            sigframe.rip             = frame->rip;
            sigframe.cs              = frame->cs;
            sigframe.rflags          = frame->rflags;
            sigframe.rsp             = frame->rsp;
            sigframe.ss              = frame->ss;

            int ret = signal_deliver_if_pending(&sigframe);
            if (ret == 1) task_exit();

            frame->rax    = sigframe.rax;
            frame->rbx    = sigframe.rbx;
            frame->rcx    = sigframe.rcx;
            frame->rdx    = sigframe.rdx;
            frame->rsi    = sigframe.rsi;
            frame->rdi    = sigframe.rdi;
            frame->rbp    = sigframe.rbp;
            frame->r8     = sigframe.r8;
            frame->r9     = sigframe.r9;
            frame->r10    = sigframe.r10;
            frame->r11    = sigframe.r11;
            frame->r12    = sigframe.r12;
            frame->r13    = sigframe.r13;
            frame->r14    = sigframe.r14;
            frame->r15    = sigframe.r15;
            frame->rip    = sigframe.rip;
            frame->rflags = sigframe.rflags;
            frame->rsp    = sigframe.rsp;
        } else {
            plogk("#PF: User-mode fault at 0x%016llx (err 0x%llx) with no process context.\n", faulting_address, error_code);
        }
        return;
    }

    panic("PAGE_FAULT-%s-Address: 0x%016llx RIP: 0x%016llx RSP: 0x%016llx error: 0x%llx", pf_msg, faulting_address, frame->rip, frame->rsp, error_code);
}

/* Determine whether the page table entry maps a huge page */
int is_huge_page(page_table_entry_t *entry)
{
    return (((uint64_t)entry->value) & PTE_HUGE) != 0;
}

/* Enable paging with a phys page directory address */
void enable_paging(uintptr_t page_directory_phys)
{
    uint64_t cr3_val = page_directory_phys;
    __asm__ volatile("mfence\n\t"
                     "mov %0, %%cr3\n\t"
                     "mov %%cr0, %%rax\n\t"
                     /*
                      * PG + WP: supervisor writes to a read-only user PTE
                      * must fault so copy_to_user() cannot bypass COW.
                      */
                     "orl $0x80010000, %%eax\n\t"
                     "mov %%rax, %%cr0\n\t"
                     "jmp 1f\n\t"
                     "1:\n\t"
                     "mov %%cr3, %%rax\n\t"
                     "mov %%rax, %%cr3\n\t"
                     :
                     : "r"(cr3_val)
                     : "rax", "memory");
    page_enable_global_tlb();
}

/* Clear all entries in a memory page table */
void page_table_clear(page_table_t *table)
{
    for (int i = 0; i < 512; i++) table->entries[i].value = 0;
}

/* Create a memory page table */
page_table_t *page_table_create(page_table_entry_t *entry)
{
    if (!entry->value) {
        uint64_t frame = alloc_frames(1);
        if (!frame) return NULL;
        entry->value        = frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
        page_table_t *table = (page_table_t *)phys_to_virt(entry->value & PAGE_4K_MASK);
        page_table_clear(table);
        return table;
    }
    page_table_t *table = (page_table_t *)phys_to_virt(entry->value & PAGE_4K_MASK);
    return table;
}

/* Returns the kernel's page directory */
page_directory_t *get_kernel_pagedir(void)
{
    return &kernel_page_dir;
}

/* Returns the page directory of the current process */
page_directory_t *get_current_directory(void)
{
    return current_directory;
}

/* Return the address mask for a leaf mapping at the given level. */
static uint64_t leaf_address_mask(int level)
{
    if (level == 3) return PAGE_1G_MASK;
    if (level == 2) return PAGE_2M_MASK;
    return PAGE_4K_MASK;
}

/* Return the number of 4 KiB frames backing a leaf at the given level. */
static size_t leaf_frame_count(int level)
{
    if (level == 3) return PAGE_1G_SIZE / PAGE_4K_SIZE;
    if (level == 2) return PAGE_2M_SIZE / PAGE_4K_SIZE;
    return 1;
}

/* Convert a writable leaf to COW unless it is explicitly shared. */
static uint64_t cow_leaf_value(uint64_t value)
{
    if ((value & PTE_WRITEABLE) && !(value & PTE_SHARED)) return (value & ~PTE_WRITEABLE) | PTE_COW;
    return value;
}

/* Release every frame referenced by a page table subtree. */
static void destroy_table(page_table_t *table, int level)
{
    for (int i = 0; i < 512; i++) {
        uint64_t value = table->entries[i].value;
        if (!(value & PTE_PRESENT)) {
            if (level == 1 && swap_entry_is_swap(value)) (void)swap_entry_release_pte(value);
            table->entries[i].value = 0;
            continue;
        }

        if (level == 1 || (value & PTE_HUGE)) {
            uint64_t mask = leaf_address_mask(level);
            (void)frame_release_range(value & mask, leaf_frame_count(level));
        } else {
            page_table_t *next = phys_to_virt(value & PAGE_4K_MASK);
            destroy_table(next, level - 1);
        }
        table->entries[i].value = 0;
    }

    (void)frame_release_range((uint64_t)virt_to_phys((uint64_t)table) & PAGE_4K_MASK, 1);
}

/* Release the lower half (user space) of a page directory. */
static void destroy_user_entries(page_directory_t *directory)
{
    page_table_t *pml4 = directory->table;
    if (!pml4) return;

    for (int i = 0; i < 256; i++) {
        uint64_t value = pml4->entries[i].value;
        if (!(value & PTE_PRESENT)) {
            pml4->entries[i].value = 0;
            continue;
        }
        if (!(value & PTE_HUGE)) {
            page_table_t *pdpt = phys_to_virt(value & PAGE_4K_MASK);
            destroy_table(pdpt, 3);
        }
        pml4->entries[i].value = 0;
    }
}

/* Copy a table subtree for fork, retaining frames and COW-marking leaves. */
static int clone_table_cow(page_table_t *destination, const page_table_t *source, int level)
{
    for (int i = 0; i < 512; i++) {
        uint64_t value = __atomic_load_n(&source->entries[i].value, __ATOMIC_ACQUIRE);
        if (!(value & PTE_PRESENT)) {
            if (level == 1 && swap_entry_is_swap(value)) {
                if (swap_entry_retain_pte(value)) return -1;
                destination->entries[i].value = cow_leaf_value(value);
            }
            continue;
        }

        if (level == 1 || (value & PTE_HUGE)) {
            uint64_t mask  = leaf_address_mask(level);
            size_t   count = leaf_frame_count(level);
            if (frame_retain_range(value & mask, count)) return -1;
            destination->entries[i].value = cow_leaf_value(value);
            continue;
        }

        uint64_t table_frame = alloc_frames(1);
        if (!table_frame) return -1;
        page_table_t *next = phys_to_virt(table_frame);
        page_table_clear(next);
        destination->entries[i].value = table_frame | (value & ~PAGE_4K_MASK);

        if (clone_table_cow(next, phys_to_virt(value & PAGE_4K_MASK), level - 1)) return -1;
    }
    return 0;
}

/* COW-mark every writable leaf in the parent's table subtree. */
static void mark_parent_table_cow(page_table_t *table, int level)
{
    for (int i = 0; i < 512; i++) {
        page_table_entry_t *entry = &table->entries[i];
        uint64_t            value = __atomic_load_n(&entry->value, __ATOMIC_ACQUIRE);
        if (!(value & PTE_PRESENT)) {
            if (level == 1 && swap_entry_is_swap(value)) __atomic_store_n(&entry->value, cow_leaf_value(value), __ATOMIC_RELEASE);
            continue;
        }
        if (level == 1 || (value & PTE_HUGE)) {
            __atomic_store_n(&entry->value, cow_leaf_value(value), __ATOMIC_RELEASE);
        } else {
            mark_parent_table_cow(phys_to_virt(value & PAGE_4K_MASK), level - 1);
        }
    }
}

/* Copy the parent's user space into a fresh child directory under COW. */
int page_clone_user_cow(page_directory_t *child, page_directory_t *parent)
{
    if (!child || !child->table || !parent || !parent->table || child == parent) return -1;

    spin_lock(&parent->lock);
    spin_lock(&child->lock);

    for (int i = 0; i < 256; i++) {
        if (child->table->entries[i].value) {
            spin_unlock(&child->lock);
            spin_unlock(&parent->lock);
            return -1;
        }
    }

    for (int i = 0; i < 256; i++) {
        uint64_t value = __atomic_load_n(&parent->table->entries[i].value, __ATOMIC_ACQUIRE);
        if (!(value & PTE_PRESENT)) continue;
        if (value & PTE_HUGE) goto rollback;

        uint64_t table_frame = alloc_frames(1);
        if (!table_frame) goto rollback;
        page_table_t *pdpt = phys_to_virt(table_frame);
        page_table_clear(pdpt);
        child->table->entries[i].value = table_frame | (value & ~PAGE_4K_MASK);
        if (clone_table_cow(pdpt, phys_to_virt(value & PAGE_4K_MASK), 3)) goto rollback;
    }

    /*
     * Publish write protection only after every child leaf owns a frame
     * reference.  The caller performs one synchronized TLB shootdown before
     * the child can run, so fork remains cheap without stale writable TLBs.
     */
    for (int i = 0; i < 256; i++) {
        uint64_t value = parent->table->entries[i].value;
        if ((value & PTE_PRESENT) && !(value & PTE_HUGE)) mark_parent_table_cow(phys_to_virt(value & PAGE_4K_MASK), 3);
    }

    spin_unlock(&child->lock);
    spin_unlock(&parent->lock);
    return 0;
rollback:
    destroy_user_entries(child);
    spin_unlock(&child->lock);
    spin_unlock(&parent->lock);
    return -1;
}

typedef struct {
        page_table_entry_t *entry;
        uint64_t            value;
        uint64_t            mask;
        size_t              size;
        size_t              frame_count;
        uintptr_t           base;
} cow_fault_leaf_t;

/* Locate the leaf mapping covering addr, filling in its frame and size. */
static int find_cow_leaf(page_directory_t *directory, uintptr_t addr, cow_fault_leaf_t *leaf)
{
    if (((addr >> 39) & 0x1ff) >= 256) return -1;

    page_table_t *table = directory->table;
    uint64_t      value = table->entries[(addr >> 39) & 0x1ff].value;
    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return -1;
    table = phys_to_virt(value & PAGE_4K_MASK);

    leaf->entry = &table->entries[(addr >> 30) & 0x1ff];
    leaf->value = __atomic_load_n(&leaf->entry->value, __ATOMIC_ACQUIRE);
    if (!(leaf->value & PTE_PRESENT)) return -1;
    if (leaf->value & PTE_HUGE) {
        leaf->mask        = PAGE_1G_MASK;
        leaf->size        = PAGE_1G_SIZE;
        leaf->frame_count = PAGE_1G_SIZE / PAGE_4K_SIZE;
        leaf->base        = ALIGN_DOWN(addr, PAGE_1G_SIZE);
        return 0;
    }
    table = phys_to_virt(leaf->value & PAGE_4K_MASK);

    leaf->entry = &table->entries[(addr >> 21) & 0x1ff];
    leaf->value = __atomic_load_n(&leaf->entry->value, __ATOMIC_ACQUIRE);
    if (!(leaf->value & PTE_PRESENT)) return -1;
    if (leaf->value & PTE_HUGE) {
        leaf->mask        = PAGE_2M_MASK;
        leaf->size        = PAGE_2M_SIZE;
        leaf->frame_count = PAGE_2M_SIZE / PAGE_4K_SIZE;
        leaf->base        = ALIGN_DOWN(addr, PAGE_2M_SIZE);
        return 0;
    }
    table = phys_to_virt(leaf->value & PAGE_4K_MASK);

    leaf->entry       = &table->entries[(addr >> 12) & 0x1ff];
    leaf->value       = __atomic_load_n(&leaf->entry->value, __ATOMIC_ACQUIRE);
    leaf->mask        = PAGE_4K_MASK;
    leaf->size        = PAGE_4K_SIZE;
    leaf->frame_count = 1;
    leaf->base        = ALIGN_DOWN(addr, PAGE_4K_SIZE);
    return (leaf->value & PTE_PRESENT) ? 0 : -1;
}

/* Return the 4 KiB PTE for addr, or NULL if any upper level is huge/absent. */
static page_table_entry_t *find_4k_pte(page_directory_t *directory, uintptr_t addr)
{
    if (!directory || !directory->table || ((addr >> 39) & 0x1ff) >= 256) return NULL;
    page_table_t *table = directory->table;
    uint64_t      value = table->entries[(addr >> 39) & 0x1ff].value;
    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    value = table->entries[(addr >> 30) & 0x1ff].value;
    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    value = table->entries[(addr >> 21) & 0x1ff].value;
    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    return &table->entries[(addr >> 12) & 0x1ff];
}

/* Return the writable VMA covering addr (mmap_lock held). */
static vm_area_t *process_writable_vma_locked(process_t *proc, uintptr_t addr)
{
    vm_area_t *vma = proc->mmap_list;
    while (vma && vma->end <= addr) vma = vma->next;
    if (!vma || vma->start > addr || addr >= vma->end || !(vma->flags & VM_WRITE)) return NULL;
    return vma;
}

/*
 * Reconcile a write-protection fault with its VMA.
 *
 * A present write fault is not necessarily a new COW operation: another CPU
 * may already have made the leaf writable while this CPU still has the old
 * read-only translation cached.  The VMA is the permission authority.  Once
 * it confirms VM_WRITE, retry writable leaves after a local invalidation,
 * unshare private referenced frames, and repair read-only leaves whose
 * software COW marker was lost during a concurrent permission update.
 */
int page_resolve_write_fault(process_t *proc, uintptr_t addr)
{
    page_directory_t *directory = proc ? proc->user_page_dir : NULL;
    if (!directory || !directory->table) return -1;

    for (;;) {
        spin_lock(&proc->mmap_lock);
        vm_area_t *vma = process_writable_vma_locked(proc, addr);
        if (!vma) {
            spin_unlock(&proc->mmap_lock);
            return -1;
        }
        spin_lock(&directory->lock);
        cow_fault_leaf_t leaf;
        if (find_cow_leaf(directory, addr, &leaf) || !(leaf.value & PTE_USER)) {
            spin_unlock(&directory->lock);
            spin_unlock(&proc->mmap_lock);
            return -1;
        }

        /* The PTE won the race; invalidate this CPU's stale read-only TLB. */
        if (leaf.value & PTE_WRITEABLE) {
            flush_tlb(leaf.base);
            spin_unlock(&directory->lock);
            spin_unlock(&proc->mmap_lock);
            return 0;
        }

        uint64_t old_frame = leaf.value & leaf.mask;
        bool     shared    = (vma->flags & VM_SHARED) || (leaf.value & PTE_SHARED);
        int      sole      = 1;
        for (size_t i = 0; i < leaf.frame_count; i++) {
            if (frame_refcount(old_frame + i * PAGE_4K_SIZE) != 1) {
                sole = 0;
                break;
            }
        }

        uint64_t replacement_flags = (leaf.value & ~leaf.mask & ~PTE_COW) | PTE_WRITEABLE;
        if (shared) replacement_flags |= PTE_SHARED;
        if (shared || sole) {
            __atomic_store_n(&leaf.entry->value, old_frame | replacement_flags, __ATOMIC_RELEASE);
            flush_tlb(leaf.base);
            spin_unlock(&directory->lock);
            spin_unlock(&proc->mmap_lock);
            flush_tlb_all();
            return 0;
        }
        if (frame_retain_range(old_frame, leaf.frame_count)) {
            spin_unlock(&directory->lock);
            spin_unlock(&proc->mmap_lock);
            return -1;
        }
        spin_unlock(&directory->lock);
        spin_unlock(&proc->mmap_lock);

        frame_reclaim_if_needed(leaf.frame_count);
        uint64_t new_frame;
        if (leaf.size == PAGE_1G_SIZE)
            new_frame = alloc_frames_1G(1);
        else if (leaf.size == PAGE_2M_SIZE)
            new_frame = alloc_frames_2M(1);
        else
            new_frame = alloc_frames(1);

        if (!new_frame) {
            (void)frame_release_range(old_frame, leaf.frame_count);
            return -1;
        }
        memcpy(phys_to_virt(new_frame), phys_to_virt(old_frame), leaf.size);

        spin_lock(&proc->mmap_lock);
        if (!process_writable_vma_locked(proc, addr)) {
            spin_unlock(&proc->mmap_lock);
            (void)frame_release_range(new_frame, leaf.frame_count);
            (void)frame_release_range(old_frame, leaf.frame_count);
            return -1;
        }
        spin_lock(&directory->lock);
        cow_fault_leaf_t current;
        int              current_result = find_cow_leaf(directory, addr, &current);
        if (!current_result && current.entry == leaf.entry && current.value == leaf.value) {
            __atomic_exchange_n(&current.entry->value, (new_frame & leaf.mask) | replacement_flags, __ATOMIC_ACQ_REL);
            flush_tlb(leaf.base);
            spin_unlock(&directory->lock);
            spin_unlock(&proc->mmap_lock);
            flush_tlb_all();
            /* Drop both the replaced mapping and the temporary copy retain. */
            (void)frame_release_range(old_frame, leaf.frame_count);
            (void)frame_release_range(old_frame, leaf.frame_count);
            return 0;
        }

        int already_resolved = !current_result && (current.value & PTE_WRITEABLE);
        int retry            = !current_result && !(current.value & PTE_WRITEABLE);
        spin_unlock(&directory->lock);
        spin_unlock(&proc->mmap_lock);
        (void)frame_release_range(new_frame, leaf.frame_count);
        (void)frame_release_range(old_frame, leaf.frame_count);
        if (already_resolved) {
            flush_tlb(current.base);
            return 0;
        }
        if (!retry) return -1;
    }
}

/* Keep the historical API for existing kernel write helpers. */
int page_resolve_cow_fault(process_t *proc, uintptr_t addr)
{
    return page_resolve_write_fault(proc, addr);
}

/* Tear down the user half of a page directory and release its frames. */
void page_destroy_user_space(page_directory_t *directory)
{
    if (!directory || !directory->table) return;

    spin_lock(&directory->lock);
    page_table_t *root = directory->table;
    destroy_user_entries(directory);
    directory->table = NULL;
    (void)frame_release_range((uint64_t)virt_to_phys((uint64_t)root) & PAGE_4K_MASK, 1);
    spin_unlock(&directory->lock);
}

/* Recursively copy memory page tables */
void copy_page_table_recursive(page_table_t *source_table, page_table_t *new_table, int level)
{
    if (!level) {
        for (int i = 0; i < 512; i++) new_table->entries[i].value = source_table->entries[i].value;
        return;
    }
    for (int i = 0; i < 512; i++) {
        if (!source_table->entries[i].value) {
            new_table->entries[i].value = 0;
            continue;
        }
        page_table_t *source_next_level = (page_table_t *)phys_to_virt(source_table->entries[i].value & PAGE_4K_MASK);
        page_table_t *new_next_level    = page_table_create(&(new_table->entries[i]));
        new_table->entries[i].value     = (uint64_t)new_next_level | (source_table->entries[i].value & 0xfff);
        copy_page_table_recursive(source_next_level, new_next_level, level - 1);
    }
}

/* Recursively free memory page tables */
void free_page_table_recursive(page_table_t *table, int level)
{
    uint64_t virtual_address  = (uint64_t)table;
    uint64_t physical_address = (uint64_t)virt_to_phys(virtual_address);

    if (!level) {
        free_frame(physical_address & PAGE_4K_MASK);
        return;
    }
    for (int i = 0; i < 512; i++) {
        page_table_entry_t *entry = &table->entries[i];
        if (entry->value == 0 || is_huge_page(entry)) continue;
        if (level == 1) {
            if (entry->value & PTE_PRESENT && entry->value & PTE_WRITEABLE && entry->value & PTE_USER) free_frame(entry->value & PAGE_4K_MASK);
        } else {
            free_page_table_recursive(phys_to_virt(entry->value & PAGE_4K_MASK), level - 1);
        }
    }
    free_frame(physical_address & PAGE_4K_MASK);
}

/* Clone a page directory */
page_directory_t *clone_directory(page_directory_t *src)
{
    if (!src || !src->table) return NULL;

    page_directory_t *new_directory = malloc(sizeof(page_directory_t));
    if (!new_directory) return NULL;

    uint64_t frame = alloc_frames(1);
    if (frame == 0) {
        free(new_directory);
        return 0;
    }
    new_directory->table       = (page_table_t *)phys_to_virt(frame);
    new_directory->lock.lock   = 0;
    new_directory->lock.rflags = 0;
    page_table_clear(new_directory->table);
    for (int i = 256; i < 512; i++) new_directory->table->entries[i] = src->table->entries[i];

    if (page_clone_user_cow(new_directory, src)) {
        page_destroy_user_space(new_directory);
        free(new_directory);
        return NULL;
    }
    flush_tlb_all();
    return new_directory;
}

/* Free a page directory */
void free_directory(page_directory_t *dir)
{
    if (!dir) return;
    page_destroy_user_space(dir);
    free(dir);
}

/* Map addr to frame, creating upper levels as needed; rollback on failure. */
static int page_map_to_status(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags, int require_empty)
{
    if (!directory || !directory->table || !frame) return -1;
    if (((addr >> 39) & 0x1ff) >= 256) flags |= PTE_GLOBAL;
    spin_lock(&directory->lock);

    uint64_t l4_index = (((addr >> 39)) & 0x1ff);
    uint64_t l3_index = (((addr >> 30)) & 0x1ff);
    uint64_t l2_index = (((addr >> 21)) & 0x1ff);
    uint64_t l1_index = (((addr >> 12)) & 0x1ff);

    page_table_entry_t *created_entries[3] = {0};
    uint64_t            created_frames[3]  = {0};
    size_t              created_count      = 0;
    page_table_t       *l4_table           = directory->table;
    page_table_entry_t *l4_entry           = &l4_table->entries[l4_index];
    if (l4_entry->value & PTE_HUGE) goto rollback;
    if (!(l4_entry->value & PTE_PRESENT)) {
        uint64_t table_frame = alloc_frames(1);
        if (!table_frame) goto rollback;
        page_table_clear(phys_to_virt(table_frame));
        l4_entry->value                 = table_frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
        created_entries[created_count]  = l4_entry;
        created_frames[created_count++] = table_frame;
    }
    page_table_t *l3_table = phys_to_virt(l4_entry->value & PAGE_4K_MASK);

    page_table_entry_t *l3_entry = &l3_table->entries[l3_index];
    if (l3_entry->value & PTE_HUGE) goto rollback;
    if (!(l3_entry->value & PTE_PRESENT)) {
        uint64_t table_frame = alloc_frames(1);
        if (!table_frame) goto rollback;
        page_table_clear(phys_to_virt(table_frame));
        l3_entry->value                 = table_frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
        created_entries[created_count]  = l3_entry;
        created_frames[created_count++] = table_frame;
    }
    page_table_t *l2_table = phys_to_virt(l3_entry->value & PAGE_4K_MASK);

    page_table_entry_t *l2_entry = &l2_table->entries[l2_index];
    if (l2_entry->value & PTE_HUGE) goto rollback;
    if (!(l2_entry->value & PTE_PRESENT)) {
        uint64_t table_frame = alloc_frames(1);
        if (!table_frame) goto rollback;
        page_table_clear(phys_to_virt(table_frame));
        l2_entry->value                 = table_frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
        created_entries[created_count]  = l2_entry;
        created_frames[created_count++] = table_frame;
    }
    page_table_t *l1_table = phys_to_virt(l2_entry->value & PAGE_4K_MASK);

    uint64_t old_value = l1_table->entries[l1_index].value;
    if (old_value & PTE_PRESENT) {
        if (require_empty || (old_value & PAGE_4K_MASK) != (frame & PAGE_4K_MASK)) goto rollback;
        if (old_value & PTE_SHARED) flags |= PTE_SHARED;
        if ((old_value & PTE_COW) && (flags & PTE_WRITEABLE) && !(flags & PTE_SHARED)) flags = (flags & ~PTE_WRITEABLE) | PTE_COW;
        if ((flags & PTE_WRITEABLE) && !(flags & PTE_SHARED) && frame_refcount(frame & PAGE_4K_MASK) > 1) flags = (flags & ~PTE_WRITEABLE) | PTE_COW;
    }
    l1_table->entries[l1_index].value = (frame & PAGE_4K_MASK) | flags;
    flush_tlb(addr);
    spin_unlock(&directory->lock);
    return 0;
rollback:
    while (created_count) {
        created_count--;
        created_entries[created_count]->value = 0;
        (void)frame_release_range(created_frames[created_count], 1);
    }
    spin_unlock(&directory->lock);
    return -1;
}

/* Maps a virtual address to a physical frame using 4KB pages */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
    (void)page_map_to_status(directory, addr, frame, flags, 0);
}

/* Map addr to frame, failing if the leaf is already mapped. */
int page_map_new_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
    return page_map_to_status(directory, addr, frame, flags, 1);
}

/* Check that addr is user-accessible with the given permissions. */
int page_user_accessible(page_directory_t *directory, uintptr_t addr, int write, int exec)
{
    if (!directory || !directory->table) return 0;
    spin_lock(&directory->lock);
    cow_fault_leaf_t leaf;
    int accessible = find_cow_leaf(directory, addr, &leaf) == 0 && (leaf.value & PTE_USER) && (!write || (leaf.value & (PTE_WRITEABLE | PTE_COW))) && (!exec || !(leaf.value & PTE_NO_EXECUTE));
    spin_unlock(&directory->lock);
    return accessible;
}

/* Advance to the next span-aligned boundary within the mapped range. */
static uintptr_t page_range_next(uintptr_t address, uintptr_t end, uintptr_t span)
{
    uintptr_t next = (address | (span - 1)) + 1;
    if (next <= address || next > end) return end;
    return next;
}

/* Count mapped user pages without taking the page-directory lock per 4 KiB. */
uint64_t page_count_present_range(page_directory_t *directory, uintptr_t start, uintptr_t end)
{
    if (!directory || !directory->table || start >= end) return 0;
    start = ALIGN_DOWN(start, PAGE_4K_SIZE);
    end   = ALIGN_UP(end, PAGE_4K_SIZE);

    uint64_t  pages  = 0;
    uintptr_t cursor = start;
    spin_lock(&directory->lock);
    while (cursor < end) {
        page_table_t *l4  = directory->table;
        uint64_t      l4e = l4->entries[(cursor >> 39) & 0x1ff].value;
        if (!(l4e & PTE_PRESENT) || (l4e & PTE_HUGE)) {
            cursor = page_range_next(cursor, end, 1ULL << 39);
            continue;
        }

        page_table_t *l3  = phys_to_virt(l4e & PAGE_4K_MASK);
        uint64_t      l3e = l3->entries[(cursor >> 30) & 0x1ff].value;
        if (!(l3e & PTE_PRESENT)) {
            cursor = page_range_next(cursor, end, PAGE_1G_SIZE);
            continue;
        }
        if (l3e & PTE_HUGE) {
            uintptr_t next = page_range_next(cursor, end, PAGE_1G_SIZE);
            if (l3e & PTE_USER) pages += (next - cursor) / PAGE_4K_SIZE;
            cursor = next;
            continue;
        }

        page_table_t *l2  = phys_to_virt(l3e & PAGE_4K_MASK);
        uint64_t      l2e = l2->entries[(cursor >> 21) & 0x1ff].value;
        if (!(l2e & PTE_PRESENT)) {
            cursor = page_range_next(cursor, end, PAGE_2M_SIZE);
            continue;
        }
        if (l2e & PTE_HUGE) {
            uintptr_t next = page_range_next(cursor, end, PAGE_2M_SIZE);
            if (l2e & PTE_USER) pages += (next - cursor) / PAGE_4K_SIZE;
            cursor = next;
            continue;
        }

        page_table_t *l1  = phys_to_virt(l2e & PAGE_4K_MASK);
        uint64_t      l1e = l1->entries[(cursor >> 12) & 0x1ff].value;
        if ((l1e & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER)) pages++;
        cursor += PAGE_4K_SIZE;
    }
    spin_unlock(&directory->lock);
    return pages;
}

/* Remove the 4 KiB mapping at addr and return its frame, or 0 if unmapped. */
uint64_t page_unmap(page_directory_t *directory, uint64_t addr)
{
    if (!directory || !directory->table) return 0;
    spin_lock(&directory->lock);

    uint64_t l4_index = (addr >> 39) & 0x1ff;
    uint64_t l3_index = (addr >> 30) & 0x1ff;
    uint64_t l2_index = (addr >> 21) & 0x1ff;
    uint64_t l1_index = (addr >> 12) & 0x1ff;

    page_table_t *l4  = directory->table;
    uint64_t      l4e = l4->entries[l4_index].value;
    if (!(l4e & PTE_PRESENT) || (l4e & PTE_HUGE)) goto not_mapped;
    page_table_t *l3  = phys_to_virt(l4e & PAGE_4K_MASK);
    uint64_t      l3e = l3->entries[l3_index].value;
    if (!(l3e & PTE_PRESENT) || (l3e & PTE_HUGE)) goto not_mapped;
    page_table_t *l2  = phys_to_virt(l3e & PAGE_4K_MASK);
    uint64_t      l2e = l2->entries[l2_index].value;
    if (!(l2e & PTE_PRESENT) || (l2e & PTE_HUGE)) goto not_mapped;
    page_table_t *l1  = phys_to_virt(l2e & PAGE_4K_MASK);
    uint64_t      l1e = l1->entries[l1_index].value;
    if (!(l1e & PTE_PRESENT)) goto not_mapped;

    l1->entries[l1_index].value = 0;
    flush_tlb(addr);
    spin_unlock(&directory->lock);
    return l1e & PAGE_4K_MASK;
not_mapped:
    spin_unlock(&directory->lock);
    return 0;
}

/* Unmap addr and release its backing frame (splitting huge pages as needed). */
int page_unmap_release(page_directory_t *directory, uint64_t addr)
{
    if (!directory || !directory->table || ((addr >> 39) & 0x1ff) >= 256) return -1;
retry_swap:
    spin_lock(&directory->lock);
    page_table_entry_t *swap_pte   = find_4k_pte(directory, addr);
    uint64_t            swap_value = swap_pte ? __atomic_load_n(&swap_pte->value, __ATOMIC_ACQUIRE) : 0;
    if (swap_entry_is_swap(swap_value)) {
        if (swap_value & PTE_SWAP_BUSY) {
            spin_unlock(&directory->lock);
            __asm__ volatile("pause");
            goto retry_swap;
        }
        __atomic_store_n(&swap_pte->value, 0, __ATOMIC_RELEASE);
        flush_tlb(addr);
        spin_unlock(&directory->lock);
        return swap_entry_release_pte(swap_value);
    }
    cow_fault_leaf_t leaf;
    if (find_cow_leaf(directory, addr, &leaf)) {
        spin_unlock(&directory->lock);
        return 1;
    }

    if (leaf.size != PAGE_4K_SIZE) {
        uint64_t first_table_frame  = alloc_frames(1);
        uint64_t second_table_frame = 0;
        if (!first_table_frame) {
            spin_unlock(&directory->lock);
            return -1;
        }
        if (leaf.size == PAGE_1G_SIZE) {
            second_table_frame = alloc_frames(1);
            if (!second_table_frame) {
                (void)frame_release_range(first_table_frame, 1);
                spin_unlock(&directory->lock);
                return -1;
            }
        }

        page_table_t *first_table = phys_to_virt(first_table_frame);
        page_table_clear(first_table);
        uint64_t old_frame   = leaf.value & leaf.mask;
        uint64_t leaf_flags  = leaf.value & ~leaf.mask;
        uint64_t table_flags = PTE_PRESENT | PTE_WRITEABLE;
        table_flags |= leaf.value & (PTE_USER | PTE_PWT | PTE_PCD);

        if (leaf.size == PAGE_1G_SIZE) {
            for (size_t i = 0; i < 512; i++) first_table->entries[i].value = (old_frame + i * PAGE_2M_SIZE) | leaf_flags;

            size_t        target_2m    = (addr >> 21) & 0x1ff;
            page_table_t *second_table = phys_to_virt(second_table_frame);
            page_table_clear(second_table);
            uint64_t       target_frame = old_frame + target_2m * PAGE_2M_SIZE;
            const uint64_t huge_pat     = 1ULL << 12;
            int            pat          = (leaf_flags & huge_pat) != 0;
            uint64_t       pte_flags    = leaf_flags & ~(PTE_HUGE | huge_pat);
            if (pat) pte_flags |= PTE_HUGE;
            for (size_t i = 0; i < 512; i++) second_table->entries[i].value = (target_frame + i * PAGE_4K_SIZE) | pte_flags;
            first_table->entries[target_2m].value = second_table_frame | table_flags;
        } else {
            const uint64_t huge_pat = 1ULL << 12;
            int            pat      = (leaf_flags & huge_pat) != 0;
            leaf_flags &= ~(PTE_HUGE | huge_pat);
            if (pat) leaf_flags |= PTE_HUGE; // Bit 7 is PAT in a 4 KiB PTE.
            for (size_t i = 0; i < 512; i++) first_table->entries[i].value = (old_frame + i * PAGE_4K_SIZE) | leaf_flags;
        }

        __atomic_exchange_n(&leaf.entry->value, first_table_frame | table_flags, __ATOMIC_ACQ_REL);
        flush_tlb(leaf.base);
        if (find_cow_leaf(directory, addr, &leaf) || leaf.size != PAGE_4K_SIZE) {
            spin_unlock(&directory->lock);
            return -1;
        }
    }

    __atomic_store_n(&leaf.entry->value, 0, __ATOMIC_RELEASE);
    flush_tlb(leaf.base);
    spin_unlock(&directory->lock);
    flush_tlb_all();
    int result = frame_release_range(leaf.value & leaf.mask, leaf.frame_count);
    return result;
}

/* Maps a virtual address to a physical frame using 2MB huge pages */
void page_map_to_2M(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
    if (!directory || !directory->table || !frame) return;
    if (((addr >> 39) & 0x1ff) >= 256) flags |= PTE_GLOBAL;
    spin_lock(&directory->lock);

    uint64_t l4_index = (addr >> 39) & 0x1FF;
    uint64_t l3_index = (addr >> 30) & 0x1FF;
    uint64_t l2_index = (addr >> 21) & 0x1FF;

    page_table_t *l4_table = directory->table;
    page_table_t *l3_table = page_table_create(&l4_table->entries[l4_index]);
    if (!l3_table) goto out;
    page_table_t *l2_table = page_table_create(&l3_table->entries[l3_index]);
    if (!l2_table) goto out;

    l2_table->entries[l2_index].value = (frame & PAGE_2M_MASK) | flags | PTE_HUGE;
    flush_tlb(addr);
out:
    spin_unlock(&directory->lock);
}

/* Maps a virtual address to a physical frame using 1GB huge pages */
void page_map_to_1G(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
{
    if (!directory || !directory->table || !frame) return;
    if (((addr >> 39) & 0x1ff) >= 256) flags |= PTE_GLOBAL;
    spin_lock(&directory->lock);

    uint64_t l4_index = (addr >> 39) & 0x1FF;
    uint64_t l3_index = (addr >> 30) & 0x1FF;

    page_table_t *l4_table = directory->table;
    page_table_t *l3_table = page_table_create(&l4_table->entries[l4_index]);
    if (!l3_table) goto out;

    l3_table->entries[l3_index].value = (frame & PAGE_1G_MASK) | flags | PTE_HUGE;
    flush_tlb(addr);
out:
    spin_unlock(&directory->lock);
}

/* Switch the page directory of the current process */
void switch_page_directory(page_directory_t *dir)
{
    current_directory            = dir;
    page_table_t *physical_table = virt_to_phys((uint64_t)dir->table);
    __asm__ volatile("mov %0, %%cr3" ::"r"(physical_table));
}

/* Maps a contiguous physical memory range to the specified virtual address range */
void page_map_range(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t length, uint64_t flags)
{
    for (uint64_t i = 0; i < length; i += 0x1000) page_map_to(directory, (uint64_t)addr + i, frame + i, flags);
}

/* Maps a contiguous physical memory range to virtual memory */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags)
{
    for (uint64_t i = 0; i < length; i += 0x1000) page_map_to(directory, (uint64_t)phys_to_virt(frame + i), frame + i, flags);
}

/* Maps random non-contiguous physical pages to the virtual address range using 4K page */
void page_map_range_to_random_4K(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    uint64_t frame = 0;
    for (uint64_t i = 0; i < length; i += PAGE_4K_SIZE) {
        frame = alloc_frames(1);
        if (frame) page_map_to(directory, addr + i, frame, flags);
    }
}

/* Maps random non-contiguous physical pages to the virtual address range using 2M page */
void page_map_range_to_random_2M(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    /* Check align */
    uint64_t aligned_addr   = ALIGN_DOWN(addr, PAGE_2M_SIZE);
    uint64_t end_addr       = ALIGN_UP(addr + length, PAGE_2M_SIZE);
    uint64_t aligned_length = end_addr - aligned_addr;
    uint64_t blocks         = aligned_length / PAGE_2M_SIZE;

    for (uint64_t i = 0; i < blocks; i++) {
        uint64_t block_addr = aligned_addr + i * PAGE_2M_SIZE;

        /* Try 2M */
        uint64_t frame_2m = alloc_frames_2M(1);
        if (frame_2m) {
            page_map_to_2M(directory, block_addr, frame_2m, flags);
        } else {
            /* Fallback to 4K */
            page_map_range_to_random_4K(directory, block_addr, PAGE_2M_SIZE, flags);
        }
    }
}

/* Maps random non-contiguous physical pages to the virtual address range using 1G page */
void page_map_range_to_random_1G(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    /* Check align */
    uint64_t aligned_addr   = ALIGN_DOWN(addr, PAGE_1G_SIZE);
    uint64_t end_addr       = ALIGN_UP(addr + length, PAGE_1G_SIZE);
    uint64_t aligned_length = end_addr - aligned_addr;
    uint64_t blocks         = aligned_length / PAGE_1G_SIZE;

    for (uint64_t i = 0; i < blocks; i++) {
        uint64_t block_addr = aligned_addr + i * PAGE_1G_SIZE;

        /* Try 1G */
        uint64_t frame_1g = alloc_frames_1G(1);
        if (frame_1g) {
            page_map_to_1G(directory, block_addr, frame_1g, flags);
        } else {
            /* Fallback to 2M */
            page_map_range_to_random_2M(directory, block_addr, PAGE_1G_SIZE, flags);
        }
    }
}

/* Helper function to map unaligned regions using 2M and 4K pages */
static void map_unaligned_region(page_directory_t *directory, uint64_t start_addr, uint64_t end_addr, uint64_t flags)
{
    /* Try 2M pages for aligned sub-regions */
    const uint64_t aligned_2m_start = ALIGN_UP(start_addr, PAGE_2M_SIZE);
    const uint64_t aligned_2m_end   = ALIGN_DOWN(end_addr, PAGE_2M_SIZE);

    /* Map aligned middle region with 2M pages */
    if (aligned_2m_start < aligned_2m_end) page_map_range_to_random_2M(directory, aligned_2m_start, aligned_2m_end - aligned_2m_start, flags);

    /* Map leading unaligned region with 4K pages */
    if (start_addr < aligned_2m_start) {
        const uint64_t lead_end = MIN(aligned_2m_start, end_addr);
        page_map_range_to_random_4K(directory, start_addr, lead_end - start_addr, flags);
    }

    /* Map trailing unaligned region with 4K pages */
    if (aligned_2m_end < end_addr) page_map_range_to_random_4K(directory, aligned_2m_end, end_addr - aligned_2m_end, flags);
}

/* Intelligently maps random non-contiguous physical pages to the virtual address range */
void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags)
{
    if (!length) return;

    const uint64_t start_addr = addr;
    const uint64_t end_addr   = addr + length;

    /* Try to map 1G-aligned regions with 1G pages */
    const uint64_t aligned_1g_start = ALIGN_UP(start_addr, PAGE_1G_SIZE);
    const uint64_t aligned_1g_end   = ALIGN_DOWN(end_addr, PAGE_1G_SIZE);

    if (aligned_1g_start < aligned_1g_end) {
        /* We have a fully 1G-aligned region in the middle */
        page_map_range_to_random_1G(directory, aligned_1g_start, aligned_1g_end - aligned_1g_start, flags);
    }

    /* Process remaining regions with 2M and 4K pages */
    uint64_t current_addr = start_addr;

    /* Handle unaligned region before 1G section (if any) */
    if (current_addr < aligned_1g_start) {
        const uint64_t chunk_end = MIN(aligned_1g_start, end_addr);
        map_unaligned_region(directory, current_addr, chunk_end, flags);
        current_addr = chunk_end;
    }

    /* Handle unaligned region after 1G section (if any) */
    if (current_addr < end_addr) map_unaligned_region(directory, current_addr, end_addr, flags);
}

/* Get the PAT configuration */
pat_config_t get_pat_config(void)
{
    pat_config_t config       = {0};
    const char  *pat_types[8] = {"WB ", "WC ", "UC-", "UC ", "WB ", "WP ", "UC-", "WT "};
    uint64_t     pat_value    = rdmsr(MSR_IA32_PAT);
    int          pos          = 0;

    for (int i = 0; i < 8; i++) {
        uint8_t entry = (pat_value >> (i * 8)) & 0xff;
        uint8_t type  = entry & 0x7;
        if (type > 7) type = 0;

        config.entries[i] = entry;
        config.types[i]   = type;
        pos += sprintf(config.pat_str + pos, "%s ", pat_types[type]);
    }
    if (pos > 0) config.pat_str[pos - 1] = '\0';
    return config;
}

/* Set the G flag on every leaf in the subtree so it survives CR3 switches. */
static void page_mark_global_leaves(page_table_t *table, int level)
{
    if (!table || level < 1) return;
    for (size_t i = 0; i < 512; i++) {
        page_table_entry_t *entry = &table->entries[i];
        uint64_t            value = entry->value;
        if (!(value & PTE_PRESENT)) continue;
        if (level == 1 || (value & PTE_HUGE)) {
            entry->value = value | PTE_GLOBAL;
            continue;
        }
        page_mark_global_leaves(phys_to_virt(value & PAGE_4K_MASK), level - 1);
    }
}

/* Enable global TLB entries (CR4.PGE) if the CPU supports them. */
static void page_enable_global_tlb(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1U << 13))) return; // CPUID.01H:EDX.PGE
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 7); // CR4.PGE
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

/* Initialize memory page table */
void page_init(void)
{
    page_table_t *kernel_page_table = phys_to_virt(get_cr3());
    kernel_page_dir                 = (page_directory_t) {.table = kernel_page_table};
    current_directory               = &kernel_page_dir;
    /*
     * Limine built the boot mappings, so retrofit G onto every existing leaf
     * in the shared kernel half before enabling PGE.
     */
    for (size_t i = 256; i < 512; i++) {
        uint64_t entry = kernel_page_table->entries[i].value;
        if (!(entry & PTE_PRESENT)) continue;
        if (entry & PTE_HUGE)
            kernel_page_table->entries[i].value = entry | PTE_GLOBAL;
        else
            page_mark_global_leaves(phys_to_virt(entry & PAGE_4K_MASK), 3);
    }
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16); // CR0.WP, see direct uaccess fault handling above.
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    page_enable_global_tlb();
    cpu_enable_nx();
}
