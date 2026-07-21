/*
 *
 *      process.c
 *      Process management
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <debug.h>
#include <errno.h>
#include <frame.h>
#include <heap.h>
#include <hhdm.h>
#include <page.h>
#include <printk.h>
#include <process.h>
#include <sched.h>
#include <smp.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <vfs.h>

#define PROCESS_TABLE_SIZE 4096

__attribute__((naked)) static void user_process_enter(void)
{
    __asm__ volatile("movq %rdi, %rax\n\t"
                     "movq %rdi, %rdx\n\t"
                     "shrq $32, %rdx\n\t"
                     "movl $0xC0000100, %ecx\n\t"
                     "wrmsr\n\t"
                     "iretq\n\t");
}

static process_t *process_table[PROCESS_TABLE_SIZE];
static spinlock_t process_table_lock;
process_t        *init_process;

void init_thread(void *arg);

typedef struct {
        uint8_t  ident[16];
        uint16_t type;
        uint16_t machine;
        uint32_t version;
        uint64_t entry;
        uint64_t phoff;
        uint64_t shoff;
        uint32_t flags;
        uint16_t ehsize;
        uint16_t phentsize;
        uint16_t phnum;
        uint16_t shentsize;
        uint16_t shnum;
        uint16_t shstrndx;
} elf64_ehdr_t;

typedef struct {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t vaddr;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t align;
} elf64_phdr_t;

#define ELF_MAGIC 0x464c457f
#define PT_LOAD   0x1
#define PF_X      (0x1 << 0)
#define PF_W      (0x1 << 1)
#define PF_R      (0x1 << 2)

static void *process_user_ptr(process_t *proc, uintptr_t addr)
{
    page_table_t *l4 = proc->user_page_dir->table;
    uint64_t l4e = l4->entries[(addr >> 39) & 0x1ff].value;
    if (!(l4e & PTE_PRESENT)) return NULL;
    page_table_t *l3 = phys_to_virt(l4e & PAGE_4K_MASK);
    uint64_t l3e = l3->entries[(addr >> 30) & 0x1ff].value;
    if (!(l3e & PTE_PRESENT) || (l3e & PTE_HUGE)) return NULL;
    page_table_t *l2 = phys_to_virt(l3e & PAGE_4K_MASK);
    uint64_t l2e = l2->entries[(addr >> 21) & 0x1ff].value;
    if (!(l2e & PTE_PRESENT) || (l2e & PTE_HUGE)) return NULL;
    page_table_t *l1 = phys_to_virt(l2e & PAGE_4K_MASK);
    uint64_t l1e = l1->entries[(addr >> 12) & 0x1ff].value;
    if (!(l1e & PTE_PRESENT)) return NULL;
    return (uint8_t *)phys_to_virt(l1e & PAGE_4K_MASK) + (addr & (PAGE_4K_SIZE - 1));
}

static uintptr_t process_setup_user_stack(process_t *proc, const elf64_ehdr_t *ehdr)
{
    uintptr_t top         = PROCESS_USER_STACK_TOP;
    uintptr_t random_addr = top - 32;
    uintptr_t string_addr = top - 64;
    uintptr_t rsp         = top - 512;
    const char name[] = "init";

    char *name_dst = process_user_ptr(proc, string_addr);
    if (!name_dst) return 0;
    memcpy(name_dst, name, sizeof(name));

    uint8_t *random = process_user_ptr(proc, random_addr);
    if (!random) return 0;
    for (int i = 0; i < 16; i++) random[i] = (uint8_t)(0xa5 + i);

    uint64_t *stack = process_user_ptr(proc, rsp);
    if (!stack) return 0;

    size_t n = 0;
    stack[n++] = 1;                  /* argc */
    stack[n++] = string_addr;        /* argv[0] */
    stack[n++] = 0;                  /* argv[1] */
    stack[n++] = 0;                  /* envp[0] */
    stack[n++] = 3;  stack[n++] = ehdr->phoff ? PROCESS_USER_CODE_MIN + ehdr->phoff : 0; /* AT_PHDR */
    stack[n++] = 4;  stack[n++] = ehdr->phentsize;                                      /* AT_PHENT */
    stack[n++] = 5;  stack[n++] = ehdr->phnum;                                          /* AT_PHNUM */
    stack[n++] = 6;  stack[n++] = PAGE_4K_SIZE;                                         /* AT_PAGESZ */
    stack[n++] = 9;  stack[n++] = ehdr->entry;                                          /* AT_ENTRY */
    stack[n++] = 11; stack[n++] = proc->uid;                                            /* AT_UID */
    stack[n++] = 12; stack[n++] = proc->uid;                                            /* AT_EUID */
    stack[n++] = 13; stack[n++] = proc->gid;                                            /* AT_GID */
    stack[n++] = 14; stack[n++] = proc->gid;                                            /* AT_EGID */
    stack[n++] = 23; stack[n++] = 0;                                                    /* AT_SECURE */
    stack[n++] = 25; stack[n++] = random_addr;                                          /* AT_RANDOM */
    stack[n++] = 0;  stack[n++] = 0;                                                    /* AT_NULL */
    return rsp;
}

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

static elf64_ehdr_t *validate_elf(const uint8_t *data, size_t size)
{
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)data;

    if (size < sizeof(elf64_ehdr_t)) return NULL;
    if (*(const uint32_t *)ehdr->ident != ELF_MAGIC) return NULL;
    if (ehdr->ident[4] != 2 || ehdr->ident[5] != 1 || ehdr->ident[6] != 1) return NULL;
    if (ehdr->machine != 0x3e) return NULL;
    if (ehdr->phoff + ehdr->phnum * ehdr->phentsize > size) return NULL;
    return ehdr;
}

static int load_elf_segments(process_t *proc, const elf64_ehdr_t *ehdr, const uint8_t *data)
{
    const elf64_phdr_t *phdr        = (const elf64_phdr_t *)(data + ehdr->phoff);
    uintptr_t           highest_end = 0;

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;
        uintptr_t seg_end = ALIGN_UP(phdr[i].vaddr + phdr[i].memsz, PAGE_4K_SIZE);
        if (seg_end > highest_end) highest_end = seg_end;
    }

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;

        uint64_t pte_flags = PTE_USER | PTE_PRESENT;
        if (phdr[i].flags & PF_W) pte_flags |= PTE_WRITEABLE;
        if (!(phdr[i].flags & PF_X)) pte_flags |= PTE_NO_EXECUTE;

        uintptr_t seg_start = ALIGN_DOWN(phdr[i].vaddr, PAGE_4K_SIZE);
        uintptr_t seg_end = ALIGN_UP(phdr[i].vaddr + phdr[i].memsz, PAGE_4K_SIZE);

        for (uintptr_t va = seg_start; va < seg_end; va += PAGE_4K_SIZE) {
            uint64_t frame = alloc_frames(1);
            if (!frame) return 1;

            uint8_t *page = phys_to_virt(frame);
            memset(page, 0, PAGE_4K_SIZE);

            uintptr_t file_start = MAX(va, phdr[i].vaddr);
            uintptr_t file_end = MIN(va + PAGE_4K_SIZE,
                                     phdr[i].vaddr + phdr[i].filesz);
            if (file_start < file_end) {
                size_t page_offset = file_start - va;
                size_t file_offset = phdr[i].offset + file_start - phdr[i].vaddr;
                memcpy(page + page_offset, data + file_offset, file_end - file_start);
            }
            page_map_to(proc->user_page_dir, va, frame, pte_flags);
        }
    }

    if (highest_end > PROCESS_HEAP_START) proc->heap_brk = highest_end;
    return 0;
}

static int setup_process_page_dir(process_t *proc)
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

static vm_area_t *vm_area_alloc(uintptr_t start, uintptr_t end, vm_flags_t flags)
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

static int vm_area_insert(process_t *proc, vm_area_t *vma)
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

static void process_fd_table_init(process_t *proc)
{
    proc->fd_lock.lock   = 0;
    proc->fd_lock.rflags = 0;
}

static void process_file_get(process_file_t *file)
{
    if (!file) return;

    spin_lock(&file->lock);
    file->refcount++;
    spin_unlock(&file->lock);
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
    spin_unlock(&file->lock);

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

static process_file_t *process_fd_get(process_t *proc, int fd)
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
    if (flags & O_APPEND) file->offset = node->size;

    spin_lock(&proc->fd_lock);
    for (int i = 0; i < PROCESS_MAX_FD; i++) {
        if (!proc->fds[i]) {
            proc->fds[i] = file;
            spin_unlock(&proc->fd_lock);
            return i;
        }
    }
    spin_unlock(&proc->fd_lock);

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
    if ((file->flags & O_ACCMODE) == O_WRONLY) {
        process_file_put(file);
        return -EBADF;
    }

    spin_lock(&file->lock);
    size_t ret = vfs_read(file->node, buf, file->offset, size);
    if (ret != (size_t)-1) file->offset += ret;
    spin_unlock(&file->lock);

    process_file_put(file);
    return ret == (size_t)-1 ? -EIO : (int64_t)ret;
}

int64_t process_fd_write(process_t *proc, int fd, const void *buf, size_t size)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;
    if ((file->flags & O_ACCMODE) == O_RDONLY) {
        process_file_put(file);
        return -EBADF;
    }

    spin_lock(&file->lock);
    if (file->flags & O_APPEND) file->offset = file->node->size;
    size_t ret = vfs_write(file->node, (void *)buf, file->offset, size);
    if (ret != (size_t)-1) file->offset += ret;
    spin_unlock(&file->lock);

    process_file_put(file);
    return ret == (size_t)-1 ? -EIO : (int64_t)ret;
}

int64_t process_fd_seek(process_t *proc, int fd, int64_t offset, int whence)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

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
        process_file_put(file);
        return -EINVAL;
    }

    int64_t next = base + offset;
    if (next < 0) {
        spin_unlock(&file->lock);
        process_file_put(file);
        return -EINVAL;
    }
    file->offset = (size_t)next;
    spin_unlock(&file->lock);

    process_file_put(file);
    return next;
}

int process_fd_ioctl(process_t *proc, int fd, size_t req, void *arg)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;
    int ret = vfs_ioctl(file->node, req, arg);
    process_file_put(file);
    return ret;
}

int process_fd_poll(process_t *proc, int fd, size_t events)
{
    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;
    int ret = vfs_poll(file->node, events);
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
}

void process_init(void)
{
    process_table_lock.lock   = 0;
    process_table_lock.rflags = 0;

    signal_init();

    process_t *init = process_create_kernel("init", init_thread, NULL);
    if (!init) panic("process: Failed to create init process.");
    init_process      = init;
    init->task->state = TASK_BLOCKED;

    for (uint32_t i = 0; i < sched_cpu_count(); i++) {
        if (cpu_rqs[i].idle) { cpu_rqs[i].idle->process = init; }
    }

    plogk("process: Process subsystem initialized. init pid=%llu\n", init->task->pid);
}

process_t *process_create(const uint8_t *elf_data, size_t elf_size, const char *name)
{
    elf64_ehdr_t *ehdr = validate_elf(elf_data, elf_size);
    if (!ehdr) {
        plogk("process: Invalid ELF binary.\n");
        return NULL;
    }

    process_t *proc = calloc(1, sizeof(process_t));
    if (!proc) return NULL;

    task_t *task = task_alloc(name);
    if (!task) {
        free(proc);
        return NULL;
    }

    proc->task            = task;
    task->process         = proc;
    proc->kernel_page_dir = get_kernel_pagedir();
    proc->kernel_stack    = malloc(PROCESS_KERNEL_STACK);
    if (!proc->kernel_stack) {
        free(task);
        free(proc);
        return NULL;
    }

    if (setup_process_page_dir(proc)) {
        free(proc->kernel_stack);
        free(task);
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

    if (load_elf_segments(proc, ehdr, elf_data)) {
        plogk("process: Failed to load ELF segments for %s.\n", name);
        process_free(proc);
        return NULL;
    }

    if (process_mmap(proc, proc->stack_brk, PROCESS_STACK_SIZE, VM_READ | VM_WRITE)) {
        plogk("process: Failed to allocate user stack for %s.\n", name);
        process_free(proc);
        return NULL;
    }

    vfs_node_t console = vfs_open("/dev/console");
    if (console) {
        int std_fd = process_fd_install(proc, console, O_RDWR);
        if (std_fd == 0) {
            process_fd_dup2(proc, 0, 1);
            process_fd_dup2(proc, 0, 2);
        }
    } else {
        plogk("process: warning - /dev/console not found for %s.\n", name);
    }

    uint64_t tls_frame = alloc_frames(1);
    if (!tls_frame) {
        plogk("process: Failed to allocate TLS frame for %s.\n", name);
        process_free(proc);
        return NULL;
    }
    memset(phys_to_virt(tls_frame), 0, PAGE_4K_SIZE);
    uintptr_t tls_user_addr = 0x700000000000ULL;
    page_map_to(proc->user_page_dir, tls_user_addr, tls_frame, PTE_USER | PTE_PRESENT | PTE_WRITEABLE);

    uintptr_t user_rsp = process_setup_user_stack(proc, ehdr);
    if (!user_rsp) {
        plogk("process: Failed to initialize user stack for %s.\n", name);
        process_free(proc);
        return NULL;
    }

    proc->task->context.rbx    = 0;
    proc->task->context.rbp    = 0;
    proc->task->context.r12    = 0;
    proc->task->context.r13    = 0;
    proc->task->context.r14    = 0;
    proc->task->context.r15    = 0;
    proc->task->context.rflags = 0x202;
    proc->task->context.rdi    = tls_user_addr;

    uint64_t  kstack_top = (uint64_t)(proc->kernel_stack + PROCESS_KERNEL_STACK);
    uint64_t *kstack     = (uint64_t *)ALIGN_DOWN(kstack_top, 16ULL);

    *(--kstack) = 0x23;
    *(--kstack) = user_rsp;
    *(--kstack) = 0x202;
    *(--kstack) = 0x1B;
    *(--kstack) = ehdr->entry;
    *(--kstack) = (uint64_t)user_process_enter;

    proc->task->context.rsp = (uint64_t)kstack;

    pid_set(proc->task->pid, proc);
    proc->task->state = TASK_READY;

    if (proc->parent) { slist_insert_tail(&proc->parent->children, proc); }

    spin_lock(&scheduler.lock);
    enqueue_task(proc->task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(proc->task);

    plogk("process: Created process %llu (%s), entry=%p\n", proc->task->pid, proc->task->name, (void *)ehdr->entry);
    return proc;
}

process_t *process_create_kernel(const char *name, void (*entry)(void *), void *arg)
{
    process_t *proc = calloc(1, sizeof(process_t));
    if (!proc) return NULL;

    task_t *task;
    if (entry) {
        task = kthread_create(name, (kthread_entry_t)entry, arg);
    } else {
        task = task_alloc(name);
        if (!task) {
            free(proc);
            return NULL;
        }
    }
    if (!task) {
        free(proc);
        return NULL;
    }

    proc->task            = task;
    task->process         = proc;
    proc->kernel_page_dir = get_kernel_pagedir();
    proc->user_page_dir   = NULL;
    proc->kernel_stack    = malloc(PROCESS_KERNEL_STACK);
    if (!proc->kernel_stack) {
        free(task);
        free(proc);
        return NULL;
    }

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
    disable_intr();
    spin_lock(&scheduler.lock);
    spin_lock(&process_table_lock);

    proc->task->state = TASK_ZOMBIE;
    proc->exit_code   = exit_code;

    /* Notify parent via SIGCHLD */
    if (proc->parent) {
        signal_notify_child_exit(proc->parent, (pid_t)proc->task->pid, exit_code, 0);
    }

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

    process_fd_table_close(proc);

    plogk("process: Process %llu (%s) exited with code %d.\n", proc->task->pid, proc->task->name, exit_code);
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
        if (child->task->state == TASK_ZOMBIE) break;
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

    process_free(saved_child);
    return 0;
}

int process_kill(pid_t pid)
{
    process_t *proc = pid_to_process(pid);
    if (!proc || proc->task->state == TASK_ZOMBIE) return 1;
    proc->exit_code   = -9;
    proc->task->state = TASK_ZOMBIE;
    process_fd_table_close(proc);
    return 0;
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

process_t *process_fork(void)
{
    task_t    *current = current_task();
    process_t *parent  = current ? current->process : NULL;
    if (!parent || parent->task->state == TASK_ZOMBIE) return NULL;

    disable_intr();
    spin_lock(&scheduler.lock);
    spin_lock(&parent->mmap_lock);

    process_t *child = calloc(1, sizeof(process_t));
    if (!child) {
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    task_t *child_task = task_alloc(parent->task->name);
    if (!child_task) {
        free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    child->task         = child_task;
    child_task->process = child;
    child->task->state  = TASK_READY;
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->pgid         = parent->pgid;
    child->sid          = parent->sid;
    child->parent       = parent;
    child->exit_code    = 0;
    child->heap_brk     = parent->heap_brk;
    child->stack_brk    = parent->stack_brk;
    child->kernel_stack = malloc(PROCESS_KERNEL_STACK);
    if (!child->kernel_stack) {
        free(child_task);
        free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }
    child->mmap_lock.lock   = 0;
    child->mmap_lock.rflags = 0;
    process_fd_table_copy(child, parent);
    signal_state_copy(&child->signal, &parent->signal);
    slist_init(&child->children);

    if (setup_process_page_dir(child)) {
        free(child->kernel_stack);
        free(child_task);
        free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    if (clone_parent_mappings(child, parent)) {
        process_free(child);
        spin_unlock(&parent->mmap_lock);
        spin_unlock(&scheduler.lock);
        return NULL;
    }

    for (vm_area_t *vma = parent->mmap_list; vma; vma = vma->next) {
        vm_area_t *copy = vm_area_alloc(vma->start, vma->end, vma->flags);
        if (!copy) {
            process_free(child);
            spin_unlock(&parent->mmap_lock);
            spin_unlock(&scheduler.lock);
            return NULL;
        }
        copy->type = vma->type;
        vm_area_insert(child, copy);
    }

    memcpy(&child_task->context, &current->context, sizeof(task_context_t));

    child_task->cpu_id = current->cpu_id;

    pid_set(child->task->pid, child);

    slist_insert_tail(&parent->children, child);

    enqueue_task(child_task);

    spin_unlock(&parent->mmap_lock);
    spin_unlock(&scheduler.lock);
    request_task_cpu(child_task);

    plogk("process: Forked process %llu from parent %llu.\n", child->task->pid, parent->task->pid);
    return child;
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
    return scheduler.next_pid;
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
    (void)length;
    if (!proc) return 1;
    spin_lock(&proc->mmap_lock);
    vm_area_t **prev = &proc->mmap_list;
    while (*prev) {
        vm_area_t *vma = *prev;
        if (vma->start == addr) {
            *prev = vma->next;
            spin_unlock(&proc->mmap_lock);
            free(vma);
            return 0;
        }
        prev = &vma->next;
    }
    spin_unlock(&proc->mmap_lock);
    return 1;
}
