/*
 *
 *      elf_loader.c
 *      ELF executable file loader.
 *
 *      2026/7/21 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <elf_loader.h>
#include <frame.h>
#include <hhdm.h>
#include <page.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#define ELF_LOADER_MAGIC 0x464c457f
#define ELF_LOADER_PT_LOAD 0x1
#define ELF_LOADER_PF_X    (0x1 << 0)
#define ELF_LOADER_PF_W    (0x1 << 1)
#define ELF_LOADER_PF_R    (0x1 << 2)

elf_loader_ehdr_t *elf_loader_validate(const uint8_t *data, size_t size)
{
    elf_loader_ehdr_t *ehdr = (elf_loader_ehdr_t *)data;

    if (size < sizeof(elf_loader_ehdr_t)) return NULL;
    if (*(const uint32_t *)ehdr->ident != ELF_LOADER_MAGIC) return NULL;
    if (ehdr->ident[4] != 2 || ehdr->ident[5] != 1 || ehdr->ident[6] != 1) return NULL;
    if (ehdr->machine != 0x3e) return NULL;
    if (ehdr->phoff + ehdr->phnum * ehdr->phentsize > size) return NULL;
    return ehdr;
}

int elf_loader_load_segments(process_t *proc, const elf_loader_ehdr_t *ehdr, const uint8_t *data)
{
    const elf_loader_phdr_t *phdr        = (const elf_loader_phdr_t *)(data + ehdr->phoff);
    uintptr_t                highest_end = 0;

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_LOADER_PT_LOAD) continue;
        uintptr_t seg_end = ALIGN_UP(phdr[i].vaddr + phdr[i].memsz, PAGE_4K_SIZE);
        if (seg_end > highest_end) highest_end = seg_end;
    }

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != ELF_LOADER_PT_LOAD) continue;

        uint64_t pte_flags = PTE_USER | PTE_PRESENT;
        if (phdr[i].flags & ELF_LOADER_PF_W) pte_flags |= PTE_WRITEABLE;
        if (!(phdr[i].flags & ELF_LOADER_PF_X)) pte_flags |= PTE_NO_EXECUTE;

        uintptr_t seg_start = ALIGN_DOWN(phdr[i].vaddr, PAGE_4K_SIZE);
        uintptr_t seg_end   = ALIGN_UP(phdr[i].vaddr + phdr[i].memsz, PAGE_4K_SIZE);

        for (uintptr_t va = seg_start; va < seg_end; va += PAGE_4K_SIZE) {
            uint64_t frame = alloc_frames(1);
            if (!frame) return 1;

            uint8_t *page = phys_to_virt(frame);
            memset(page, 0, PAGE_4K_SIZE);

            uintptr_t file_start = MAX(va, phdr[i].vaddr);
            uintptr_t file_end   = MIN(va + PAGE_4K_SIZE, phdr[i].vaddr + phdr[i].filesz);
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

void *elf_loader_user_ptr(process_t *proc, uintptr_t addr)
{
    page_table_t *l4  = proc->user_page_dir->table;
    uint64_t      l4e = l4->entries[(addr >> 39) & 0x1ff].value;
    if (!(l4e & PTE_PRESENT)) return NULL;
    page_table_t *l3  = phys_to_virt(l4e & PAGE_4K_MASK);
    uint64_t      l3e = l3->entries[(addr >> 30) & 0x1ff].value;
    if (!(l3e & PTE_PRESENT) || (l3e & PTE_HUGE)) return NULL;
    page_table_t *l2  = phys_to_virt(l3e & PAGE_4K_MASK);
    uint64_t      l2e = l2->entries[(addr >> 21) & 0x1ff].value;
    if (!(l2e & PTE_PRESENT) || (l2e & PTE_HUGE)) return NULL;
    page_table_t *l1  = phys_to_virt(l2e & PAGE_4K_MASK);
    uint64_t      l1e = l1->entries[(addr >> 12) & 0x1ff].value;
    if (!(l1e & PTE_PRESENT)) return NULL;
    return (uint8_t *)phys_to_virt(l1e & PAGE_4K_MASK) + (addr & (PAGE_4K_SIZE - 1));
}

uintptr_t elf_loader_setup_user_stack(process_t *proc, const elf_loader_ehdr_t *ehdr)
{
    uintptr_t  top         = PROCESS_USER_STACK_TOP;
    uintptr_t  random_addr = top - 32;
    uintptr_t  string_addr = top - 64;
    uintptr_t  rsp         = top - 512;
    size_t name_len = strlen(proc->name) + 1;

    char *name_dst = elf_loader_user_ptr(proc, string_addr);
    if (!name_dst) return 0;
    memcpy(name_dst, proc->name, name_len);

    uint8_t *random = elf_loader_user_ptr(proc, random_addr);
    if (!random) return 0;
    for (int i = 0; i < 16; i++) random[i] = (uint8_t)(0xa5 + i);

    uint64_t *stack = elf_loader_user_ptr(proc, rsp);
    if (!stack) return 0;

    size_t n   = 0;
    stack[n++] = 1;                                                                          /* argc */
    stack[n++] = string_addr;                                                                /* argv[0] */
    stack[n++] = 0;                                                                          /* argv[1] */
    stack[n++] = 0;                                                                          /* envp[0] */
    stack[n++] = 3;
    stack[n++] = ehdr->phoff ? PROCESS_USER_CODE_MIN + ehdr->phoff : 0;                      /* AT_PHDR */
    stack[n++] = 4;
    stack[n++] = ehdr->phentsize;                                                            /* AT_PHENT */
    stack[n++] = 5;
    stack[n++] = ehdr->phnum;                                                                /* AT_PHNUM */
    stack[n++] = 6;
    stack[n++] = PAGE_4K_SIZE;                                                               /* AT_PAGESZ */
    stack[n++] = 9;
    stack[n++] = ehdr->entry;                                                                /* AT_ENTRY */
    stack[n++] = 11;
    stack[n++] = proc->uid;                                                                  /* AT_UID */
    stack[n++] = 12;
    stack[n++] = proc->uid;                                                                  /* AT_EUID */
    stack[n++] = 13;
    stack[n++] = proc->gid;                                                                  /* AT_GID */
    stack[n++] = 14;
    stack[n++] = proc->gid;                                                                  /* AT_EGID */
    stack[n++] = 23;
    stack[n++] = 0;                                                                          /* AT_SECURE */
    stack[n++] = 25;
    stack[n++] = random_addr;                                                                /* AT_RANDOM */
    stack[n++] = 0;
    stack[n++] = 0;                                                                          /* AT_NULL */
    return rsp;
}
