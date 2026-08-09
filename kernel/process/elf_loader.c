/*
 *
 *      elf_loader.c
 *      ELF64 process-image loader
 *
 *      2026/7/21 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/tty/tty/tty.h>
#include <fs/core/vfs.h>
#include <kernel/elf.h>
#include <kernel/elf_loader.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/uaccess.h>
#include <syscall/fcntl.h>
#include <syscall/syscall.h>

#define INTERP_LOAD_BASE 0x7f0000000000ULL
#define INTERP_LOAD_END  0x7f0001000000ULL

static int validate_elf(const uint8_t *data, size_t size, Elf64_Ehdr **ehdr_out)
{
    if (size < sizeof(Elf64_Ehdr)) return -1;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;
    if (*(const uint32_t *)ehdr->e_ident != ELF_MAGIC) return -1;
    if (ehdr->e_ident[4] != 2 || ehdr->e_ident[5] != 1 || ehdr->e_ident[6] != 1) return -1;
    if (ehdr->e_machine != 0x3e) return -1;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return -1;
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) return -1;
    if (ehdr->e_phoff > size) return -1;
    if (ehdr->e_phnum > (size - ehdr->e_phoff) / sizeof(Elf64_Phdr)) return -1;
    *ehdr_out = ehdr;
    return 0;
}

int elf_loader_parse_elf_info(const uint8_t *data, size_t size, elf_load_info_t *info)
{
    memset(info, 0, sizeof(*info));

    Elf64_Ehdr *ehdr = NULL;
    if (validate_elf(data, size, &ehdr)) return -1;

    info->entry      = ehdr->e_entry;
    info->phdr       = (uintptr_t)(data + ehdr->e_phoff);
    info->phnum      = ehdr->e_phnum;
    info->phentsize  = ehdr->e_phentsize;
    info->is_dynamic = 0;
    info->has_interp = 0;

    Elf64_Phdr *phdr = (Elf64_Phdr *)(data + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        switch (phdr[i].type) {
            case PT_INTERP :
                if (info->has_interp) return -1;
                if (phdr[i].filesz > 1 && phdr[i].filesz < sizeof(info->interp_path)) {
                    if (phdr[i].offset + phdr[i].filesz > size) break;
                    const char *interp = (const char *)data + phdr[i].offset;
                    if (interp[phdr[i].filesz - 1] != '\0') return -1;
                    memcpy(info->interp_path, interp, phdr[i].filesz);
                    info->has_interp = 1;
                }
                break;
            case PT_DYNAMIC :
                info->is_dynamic       = 1;
                info->pt_dynamic_vaddr = phdr[i].vaddr;
                break;
            case PT_TLS :
                info->tls_vaddr = phdr[i].vaddr;
                info->tls_size  = phdr[i].memsz;
                info->tls_align = phdr[i].align;
                break;
            default :
                break;
        }
    }
    return 0;
}

/* Return the relocated, page-aligned memory range of a PT_LOAD segment. */
static int elf_segment_range(const Elf64_Phdr *segment, uintptr_t load_bias, uintptr_t *base_out, uintptr_t *start_out, uintptr_t *end_out)
{
    if (segment->memsz == 0) return 0;
    if (segment->vaddr > UINT64_MAX - load_bias) return -1;

    uintptr_t base = segment->vaddr + load_bias;
    if (base > UINT64_MAX - segment->memsz) return -1;

    uintptr_t mem_end = base + segment->memsz;
    if (mem_end > UINT64_MAX - (PAGE_4K_SIZE - 1)) return -1;
    uintptr_t end = ALIGN_UP(mem_end, PAGE_4K_SIZE);
    if (end < mem_end) return -1;

    if (base_out) *base_out = base;
    if (start_out) *start_out = ALIGN_DOWN(base, PAGE_4K_SIZE);
    if (end_out) *end_out = end;
    return 1;
}

/* Return the permissions for one page covered by one or more PT_LOADs. */
static int elf_page_attributes(const Elf64_Phdr *phdr, int phnum, uintptr_t load_bias, uintptr_t va, uint64_t *pte_flags_out,
                               vm_flags_t *vm_flags_out)
{
    int       covered    = 0;
    int       readable   = 0;
    int       writable   = 0;
    int       executable = 0;
    uintptr_t page_end   = va + PAGE_4K_SIZE;

    for (int i = 0; i < phnum; i++) {
        if (phdr[i].type != PT_LOAD || phdr[i].memsz == 0) continue;

        uintptr_t seg_start, seg_end;
        if (elf_segment_range(&phdr[i], load_bias, NULL, &seg_start, &seg_end) <= 0) continue;
        if (va >= seg_end || page_end <= seg_start) continue;

        covered = 1;
        if (phdr[i].flags & PF_R) readable = 1;
        if (phdr[i].flags & PF_W) writable = 1;
        if (phdr[i].flags & PF_X) executable = 1;
    }

    if (!covered) return 0;

    uint64_t pte_flags = PTE_USER | PTE_PRESENT;
    if (writable) pte_flags |= PTE_WRITEABLE;
    if (!executable) pte_flags |= PTE_NO_EXECUTE;

    vm_flags_t vm_flags = 0;
    if (readable) vm_flags |= VM_READ;
    if (writable) vm_flags |= VM_WRITE;
    if (executable) vm_flags |= VM_EXEC;

    *pte_flags_out = pte_flags;
    *vm_flags_out  = vm_flags;
    return 1;
}

static int insert_elf_vma(process_t *proc, uintptr_t start, uintptr_t end, vm_flags_t flags)
{
    if (start >= end) return 0;

    vm_area_t *vma = calloc(1, sizeof(*vma));
    if (!vma) {
        plogk("elf_loader: PT_LOAD VMA allocation failed at %p\n", (void *)start);
        return 1;
    }
    vma->start = start;
    vma->end   = end;
    vma->flags = flags;
    vma->type  = VM_REGION_MMAP;
    if (vm_area_insert(proc, vma)) {
        plogk("elf_loader: PT_LOAD VMA insert failed at %p\n", (void *)start);
        free(vma);
        return 1;
    }
    return 0;
}

static int load_elf_segments(process_t *proc, const Elf64_Ehdr *ehdr, const uint8_t *data, size_t elf_size, uintptr_t load_bias, int set_brk)
{
    const Elf64_Phdr *phdr         = (const Elf64_Phdr *)(data + ehdr->e_phoff);
    uintptr_t         lowest_start = UINT64_MAX;
    uintptr_t         highest_end  = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;
        if (phdr[i].filesz > phdr[i].memsz) return 1;
        if (phdr[i].offset > elf_size || phdr[i].filesz > elf_size - phdr[i].offset) return 1;
        if (phdr[i].align > 1
            && ((phdr[i].align & (phdr[i].align - 1)) || (phdr[i].vaddr & (phdr[i].align - 1)) != (phdr[i].offset & (phdr[i].align - 1))))
            return 1;

        uintptr_t seg_start, seg_end;
        int       range = elf_segment_range(&phdr[i], load_bias, NULL, &seg_start, &seg_end);
        if (range < 0) return 1;
        if (range == 0) continue;
        if (seg_start < PROCESS_HEAP_START || seg_end > PROCESS_USER_STACK_TOP) return 1;
        if (seg_start < lowest_start) lowest_start = seg_start;
        if (seg_end > highest_end) highest_end = seg_end;
    }

    if (lowest_start == UINT64_MAX) return 1;

    /*
     * Map each virtual page once, then copy all PT_LOAD portions into it.
     * ELF files commonly put the end of an R segment and the beginning of
     * the following RW segment in the same page (GNU_RELRO).  Loading one
     * segment at a time incorrectly treats that normal layout as a collision.
     */
    for (uintptr_t va = lowest_start; va < highest_end; va += PAGE_4K_SIZE) {
        uint64_t   pte_flags;
        vm_flags_t vm_flags;
        if (!elf_page_attributes(phdr, ehdr->e_phnum, load_bias, va, &pte_flags, &vm_flags)) continue;
        (void)vm_flags;

        uint64_t frame = alloc_frames(1);
        if (!frame) {
            plogk("elf_loader: PT_LOAD frame allocation failed at %p\n", (void *)va);
            return 1;
        }

        uint8_t *page = phys_to_virt(frame);
        memset(page, 0, PAGE_4K_SIZE);
        if (page_map_new_to(proc->user_page_dir, va, frame, pte_flags)) {
            (void)frame_release_range(frame, 1);
            plogk("elf_loader: PT_LOAD page collision at %p\n", (void *)va);
            return 1;
        }

        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].type != PT_LOAD || phdr[i].filesz == 0) continue;

            uintptr_t base;
            if (elf_segment_range(&phdr[i], load_bias, &base, NULL, NULL) <= 0) continue;
            uintptr_t file_start = MAX(va, base);
            uintptr_t file_end   = MIN(va + PAGE_4K_SIZE, base + phdr[i].filesz);
            if (file_start >= file_end) continue;

            uintptr_t file_delta = file_start - base;
            if (phdr[i].offset > elf_size || file_delta > elf_size - phdr[i].offset) return 1;
            uint64_t file_offset = phdr[i].offset + file_delta;
            size_t   copy_size   = file_end - file_start;
            if (copy_size > elf_size - file_offset) return 1;
            memcpy(page + (file_start - va), data + file_offset, copy_size);
        }
    }

    /* Build non-overlapping VMAs, merging pages with identical permissions. */
    uintptr_t  run_start = 0;
    uintptr_t  run_end   = 0;
    vm_flags_t run_flags = 0;
    for (uintptr_t va = lowest_start; va < highest_end; va += PAGE_4K_SIZE) {
        uint64_t   pte_flags;
        vm_flags_t vm_flags;
        if (!elf_page_attributes(phdr, ehdr->e_phnum, load_bias, va, &pte_flags, &vm_flags)) {
            if (run_start && insert_elf_vma(proc, run_start, run_end, run_flags)) return 1;
            run_start = 0;
            continue;
        }
        (void)pte_flags;

        if (!run_start) {
            run_start = va;
            run_end   = va + PAGE_4K_SIZE;
            run_flags = vm_flags;
        } else if (run_end != va || run_flags != vm_flags) {
            if (insert_elf_vma(proc, run_start, run_end, run_flags)) return 1;
            run_start = va;
            run_end   = va + PAGE_4K_SIZE;
            run_flags = vm_flags;
        } else {
            run_end += PAGE_4K_SIZE;
        }
    }
    if (run_start && insert_elf_vma(proc, run_start, run_end, run_flags)) return 1;

    if (set_brk && highest_end > PROCESS_HEAP_START) proc->start_brk = proc->heap_brk = highest_end;
    return 0;
}

static void *user_ptr(process_t *proc, uintptr_t addr)
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

static int write_user(process_t *proc, uintptr_t dst, const void *src, size_t size)
{
    const uint8_t *in = src;
    while (size) {
        void *out = user_ptr(proc, dst);
        if (!out && process_demand_fault(proc, dst, 1, 0) == 0) out = user_ptr(proc, dst);
        if (!out) return -1;
        size_t chunk = PAGE_4K_SIZE - (dst & (PAGE_4K_SIZE - 1));
        if (chunk > size) chunk = size;
        memcpy(out, in, chunk);
        dst += chunk;
        in += chunk;
        size -= chunk;
    }
    return 0;
}

static uintptr_t find_free_range(process_t *proc, uintptr_t start, uintptr_t end, size_t size)
{
    uintptr_t addr  = ALIGN_UP(start, PAGE_4K_SIZE);
    size_t    pages = ALIGN_UP(size, PAGE_4K_SIZE);

    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (addr + pages <= vma->start) {
            spin_unlock(&proc->mmap_lock);
            return addr;
        }
        if (vma->end > addr) addr = ALIGN_UP(vma->end, PAGE_4K_SIZE);
    }
    spin_unlock(&proc->mmap_lock);

    if (addr + pages <= end) return addr;
    return 0;
}

static uintptr_t compute_load_bias(const Elf64_Ehdr *ehdr, const uint8_t *data, uintptr_t chosen_base)
{
    if (ehdr->e_type == ET_DYN) {
        const Elf64_Phdr *phdr   = (const Elf64_Phdr *)(data + ehdr->e_phoff);
        uintptr_t         lowest = UINT64_MAX;
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].type == PT_LOAD) {
                uintptr_t start = ALIGN_DOWN(phdr[i].vaddr, PAGE_4K_SIZE);
                if (start < lowest) lowest = start;
            }
        }
        if (lowest != UINT64_MAX && chosen_base >= lowest) return ALIGN_DOWN(chosen_base - lowest, PAGE_4K_SIZE);
    }
    return 0;
}

int elf_loader_load_interpreter(struct process *proc, const char *interp_path, Elf64_Addr *base_out, Elf64_Addr *entry_out)
{
    char resolved_path[VFS_PATH_MAX];
    if (process_resolve_path_at(proc, PROCESS_AT_FDCWD, interp_path, resolved_path, sizeof(resolved_path)) != EOK) return -1;

    vfs_node_t node = vfs_open(resolved_path);
    if (!node) {
        plogk("elf_loader: interpreter not found: %s\n", interp_path);
        return -1;
    }

    if (node->size == 0 || node->size > 0x400000) {
        plogk("elf_loader: interpreter invalid size: %s\n", interp_path);
        vfs_close(node);
        return -1;
    }

    uint8_t *elf_data = malloc(node->size);
    if (!elf_data) {
        plogk("elf_loader: interpreter image allocation failed (%llu bytes): %s\n", (unsigned long long)node->size, interp_path);
        vfs_close(node);
        return -1;
    }

    size_t total = 0;
    while (total < node->size) {
        size_t remaining = node->size - total;
        size_t n         = vfs_read(node, elf_data + total, total, remaining);
        if (!n || n > remaining) break;
        total += n;
    }
    vfs_close(node);

    if (total < sizeof(Elf64_Ehdr)) {
        plogk("elf_loader: interpreter truncated (%lu bytes): %s\n", (unsigned long)total, interp_path);
        free(elf_data);
        return -1;
    }

    Elf64_Ehdr *iehdr = NULL;
    if (validate_elf(elf_data, total, &iehdr)) {
        plogk("elf_loader: invalid interpreter ELF: %s\n", interp_path);
        free(elf_data);
        return -1;
    }

    const Elf64_Phdr *iphdr       = (const Elf64_Phdr *)(elf_data + iehdr->e_phoff);
    uintptr_t         image_start = UINT64_MAX;
    uintptr_t         image_end   = 0;
    for (int i = 0; i < iehdr->e_phnum; i++) {
        if (iphdr[i].type != PT_LOAD) continue;
        uintptr_t start = ALIGN_DOWN(iphdr[i].vaddr, PAGE_4K_SIZE);
        uintptr_t end   = ALIGN_UP(iphdr[i].vaddr + iphdr[i].memsz, PAGE_4K_SIZE);
        if (start < image_start) image_start = start;
        if (end > image_end) image_end = end;
    }
    if (image_start == UINT64_MAX || image_end <= image_start) {
        plogk("elf_loader: interpreter has no PT_LOAD segment: %s\n", interp_path);
        free(elf_data);
        return -1;
    }

    uintptr_t interp_base = find_free_range(proc, INTERP_LOAD_BASE, INTERP_LOAD_END, image_end - image_start);
    if (!interp_base) {
        plogk("elf_loader: no free space for interpreter.\n");
        free(elf_data);
        return -1;
    }

    uintptr_t load_bias = compute_load_bias(iehdr, elf_data, interp_base);

    if (load_elf_segments(proc, iehdr, elf_data, total, load_bias, 0)) {
        plogk("elf_loader: failed to load interpreter segments.\n");
        free(elf_data);
        return -1;
    }

    int valid_entry = 0;
    for (int i = 0; i < iehdr->e_phnum; i++) {
        if (iphdr[i].type == PT_LOAD && (iphdr[i].flags & PF_X)) {
            uintptr_t entry = iehdr->e_entry + load_bias;
            uintptr_t start = iphdr[i].vaddr + load_bias;
            if (entry >= start && entry < start + iphdr[i].memsz) valid_entry = 1;
        }
        if (valid_entry) { break; }
    }
    if (!valid_entry) {
        plogk("elf_loader: interpreter entry %#lx outside any executable segment: %s\n", (unsigned long)(iehdr->e_entry + load_bias),
              interp_path);
        free(elf_data);
        return -1;
    }

    *base_out  = load_bias;
    *entry_out = iehdr->e_entry + load_bias;

    free(elf_data);
    return 0;
}

static int count_string_array(char *const arr[])
{
    if (!arr) return 0;
    int count = 0;
    for (int i = 0; arr[i]; i++) count++;
    return count;
}

static size_t string_array_size(char *const arr[])
{
    if (!arr) return 0;
    size_t total = 0;
    for (int i = 0; arr[i]; i++) total += strlen(arr[i]) + 1;
    return total;
}

static uintptr_t setup_user_stack(process_t *proc, uintptr_t phdr_addr, uint16_t phnum, uint16_t phentsize, uintptr_t interp_base,
                                  uintptr_t main_entry, char *const argv[], char *const envp[])
{
    int         argc      = count_string_array(argv);
    int         envc      = count_string_array(envp);
    size_t      argv_strs = string_array_size(argv);
    size_t      envp_strs = string_array_size(envp);
    const char *execfn    = argc > 0 ? argv[0] : proc->name;

    const size_t aux_pairs    = 16;
    size_t       vector_words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + aux_pairs * 2;
    size_t       strings_size = argv_strs + envp_strs + strlen(execfn) + 1 + sizeof("x86_64") + 16;
    size_t       total_needed = ALIGN_UP(vector_words * sizeof(uint64_t) + strings_size + 16, 16);
    if (total_needed > (size_t)PROCESS_STACK_SIZE) return 0;
    uintptr_t base_rsp    = ALIGN_DOWN(PROCESS_USER_STACK_TOP - total_needed, 16);
    uintptr_t string_area = base_rsp + vector_words * sizeof(uint64_t);
    uint64_t *vectors     = calloc(vector_words, sizeof(uint64_t));
    uint8_t  *strings     = calloc(1, strings_size);
    if (!vectors || !strings) {
        free(vectors);
        free(strings);
        return 0;
    }

    uint8_t *sp       = strings;
    size_t   name_len = strlen(execfn) + 1;
    memcpy(sp, execfn, name_len);
    uintptr_t execfn_addr = string_area;
    sp += name_len;

    size_t n     = 0;
    vectors[n++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        memcpy(sp, argv[i], len);
        vectors[n++] = string_area + (uintptr_t)(sp - strings);
        sp += len;
    }
    vectors[n++] = 0;

    for (int i = 0; i < envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        memcpy(sp, envp[i], len);
        vectors[n++] = string_area + (uintptr_t)(sp - strings);
        sp += len;
    }
    vectors[n++] = 0;

    const char platform_str[] = "x86_64";
    memcpy(sp, platform_str, sizeof(platform_str));
    uintptr_t platform_addr = string_area + (uintptr_t)(sp - strings);
    sp += sizeof(platform_str);

    uintptr_t random_addr  = string_area + (uintptr_t)(sp - strings);
    uint64_t  random_state = sched_ticks() ^ (uintptr_t)proc ^ phdr_addr;
    for (int i = 0; i < 16; i++) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 7;
        random_state ^= random_state << 17;
        sp[i] = (uint8_t)random_state;
    }

    vectors[n++] = AT_PHDR;
    vectors[n++] = phdr_addr;
    vectors[n++] = AT_PHENT;
    vectors[n++] = phentsize;
    vectors[n++] = AT_PHNUM;
    vectors[n++] = phnum;
    vectors[n++] = AT_PAGESZ;
    vectors[n++] = PAGE_4K_SIZE;
    vectors[n++] = AT_BASE;
    vectors[n++] = interp_base;
    vectors[n++] = AT_FLAGS;
    vectors[n++] = 0;
    vectors[n++] = AT_ENTRY;
    vectors[n++] = main_entry;
    vectors[n++] = AT_UID;
    vectors[n++] = proc->uid;
    vectors[n++] = AT_EUID;
    vectors[n++] = proc->uid;
    vectors[n++] = AT_GID;
    vectors[n++] = proc->gid;
    vectors[n++] = AT_EGID;
    vectors[n++] = proc->gid;
    vectors[n++] = AT_SECURE;
    vectors[n++] = 0;
    vectors[n++] = AT_PLATFORM;
    vectors[n++] = platform_addr;
    vectors[n++] = AT_RANDOM;
    vectors[n++] = random_addr;
    vectors[n++] = AT_EXECFN;
    vectors[n++] = execfn_addr;
    vectors[n++] = AT_NULL;
    vectors[n++] = 0;

    int failed = write_user(proc, base_rsp, vectors, vector_words * sizeof(uint64_t)) || write_user(proc, string_area, strings, strings_size);
    free(vectors);
    free(strings);
    if (failed) return 0;

    return base_rsp;
}

__attribute__((naked)) static void user_process_enter(void)
{
    __asm__ volatile("xorl %eax, %eax\n\t"
                     "xorl %ebx, %ebx\n\t"
                     "xorl %ecx, %ecx\n\t"
                     "xorl %edx, %edx\n\t"
                     "xorl %esi, %esi\n\t"
                     "xorl %edi, %edi\n\t"
                     "xorl %r8d, %r8d\n\t"
                     "xorl %r9d, %r9d\n\t"
                     "xorl %r10d, %r10d\n\t"
                     "xorl %r11d, %r11d\n\t"
                     "iretq\n\t");
}

int elf_loader_load_process_internal(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[],
                                     uintptr_t *entry_out, uintptr_t *rsp_out, bool acquire_console)
{
    Elf64_Ehdr *ehdr = NULL;
    if (validate_elf(elf_data, elf_size, &ehdr)) {
        plogk("elf_loader: Invalid ELF binary.\n");
        return 1;
    }

    elf_load_info_t info;
    if (elf_loader_parse_elf_info(elf_data, elf_size, &info)) {
        plogk("elf_loader: Failed to parse ELF info.\n");
        return 1;
    }

    uintptr_t load_bias = 0;
    if (ehdr->e_type == ET_DYN) {
        uintptr_t chosen_base = PROCESS_USER_CODE_MIN;
        load_bias             = compute_load_bias(ehdr, elf_data, chosen_base);
    }

    uintptr_t         phdr_addr = 0;
    const Elf64_Phdr *phdrs     = (const Elf64_Phdr *)(elf_data + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].type == PT_PHDR) {
            phdr_addr = phdrs[i].vaddr + load_bias;
            break;
        }
    }
    if (!phdr_addr) {
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].type == PT_LOAD) {
                size_t phdr_size = (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
                if (ehdr->e_phoff >= phdrs[i].offset && ehdr->e_phoff + phdr_size <= phdrs[i].offset + phdrs[i].filesz) {
                    phdr_addr = phdrs[i].vaddr + load_bias + (ehdr->e_phoff - phdrs[i].offset);
                    break;
                }
            }
        }
    }

    if (load_elf_segments(proc, ehdr, elf_data, elf_size, load_bias, 1)) {
        plogk("elf_loader: Failed to load ELF segments.\n");
        return 1;
    }

    if (process_mmap(proc, proc->stack_brk, (size_t)PROCESS_STACK_SIZE, VM_READ | VM_WRITE | VM_LAZY)) {
        plogk("elf_loader: Failed to allocate user stack.\n");
        return 1;
    }

    if (acquire_console) {
        vfs_node_t console = vfs_open("/dev/console");
        if (!console) {
            plogk("elf_loader: PID 1 cannot open /dev/console.\n");
            return 1;
        }

        int std_fd = process_fd_install(proc, console, O_RDWR | O_NOCTTY);
        if (std_fd != 0) {
            if (std_fd < 0) vfs_close(console);
            plogk("elf_loader: PID 1 failed to install /dev/console on standard input.\n");
            return 1;
        }

        int stdout_fd = process_fd_dup2(proc, 0, 1);
        int stderr_fd = process_fd_dup2(proc, 0, 2);
        int ctty      = tty_console_acquire(proc, O_RDWR);
        if (stdout_fd != 1 || stderr_fd != 2 || ctty) {
            plogk("elf_loader: PID 1 failed to acquire /dev/console as its controlling terminal.\n");
            return 1;
        }
    }

    proc->task->thread.fs_base = 0;
    proc->task->thread.gs_base = 0;

    Elf64_Addr interpreter_base  = 0;
    Elf64_Addr interpreter_entry = 0;
    uintptr_t  actual_entry      = ehdr->e_entry + load_bias;

    if (info.has_interp) {
        if (elf_loader_load_interpreter(proc, info.interp_path, &interpreter_base, &interpreter_entry)) {
            plogk("elf_loader: Failed to load required interpreter.\n");
            return 1;
        }
        actual_entry = interpreter_entry;
    }

    int valid_entry = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].type != PT_LOAD) continue;
        uintptr_t seg_start = phdrs[i].vaddr + load_bias;
        uintptr_t seg_end   = seg_start + phdrs[i].memsz;
        if ((phdrs[i].flags & PF_X) && ehdr->e_entry + load_bias >= seg_start && ehdr->e_entry + load_bias < seg_end) {
            valid_entry = 1;
            break;
        }
    }
    if (!valid_entry) {
        plogk("elf_loader: entry point not within any loaded segment.\n");
        return 1;
    }

    uintptr_t user_rsp
        = setup_user_stack(proc, phdr_addr, ehdr->e_phnum, ehdr->e_phentsize, interpreter_base, ehdr->e_entry + load_bias, argv, envp);
    if (!user_rsp) {
        plogk("elf_loader: Failed to initialize user stack.\n");
        return 1;
    }

    proc->task->context.rbx    = 0;
    proc->task->context.rbp    = 0;
    proc->task->context.r12    = 0;
    proc->task->context.r13    = 0;
    proc->task->context.r14    = 0;
    proc->task->context.r15    = 0;
    proc->task->context.rflags = 0x202;
    proc->task->context.rdi    = 0;

    if (!entry_out && !rsp_out) {
        uint64_t  kstack_top = (uint64_t)(proc->kernel_stack + PROCESS_KERNEL_STACK);
        uint64_t *kstack     = (uint64_t *)ALIGN_DOWN(kstack_top, 16ULL);

        *(--kstack) = 0x23;
        *(--kstack) = user_rsp;
        *(--kstack) = 0x202;
        *(--kstack) = 0x1B;
        *(--kstack) = actual_entry;
        *(--kstack) = (uint64_t)user_process_enter;

        proc->task->context.rsp = (uint64_t)kstack;
        proc->task->state       = TASK_READY;
    }
    if (entry_out) *entry_out = actual_entry;
    if (rsp_out) *rsp_out = user_rsp;

    return 0;
}

/* Load the system init executable from the already-populated root filesystem.
 * Limine only supplies the initramfs; PID 1 must follow the normal Unix boot
 * contract and execute /sbin/init from that filesystem. */
int elf_loader_load_initial_path(process_t *proc, const char *path, char *const argv[], char *const envp[])
{
    if (!proc || !path || !path[0]) return -EINVAL;

    vfs_node_t node = vfs_open(path);
    if (!node) {
        plogk("elf_loader: init executable not found: %s\n", path);
        return -ENOENT;
    }
    if (node->type & file_dir) {
        plogk("elf_loader: init path is a directory: %s\n", path);
        vfs_close(node);
        return -EISDIR;
    }
    if (!node->size || node->size > 64ULL * 1024ULL * 1024ULL) {
        plogk("elf_loader: init executable has invalid size: %s (%llu bytes)\n", path, (unsigned long long)node->size);
        vfs_close(node);
        return -EINVAL;
    }

    size_t   image_size = (size_t)node->size;
    uint8_t *image      = malloc(image_size);
    if (!image) {
        plogk("elf_loader: unable to allocate init image (%llu bytes)\n", (unsigned long long)image_size);
        vfs_close(node);
        return -ENOMEM;
    }

    size_t loaded = 0;
    while (loaded < image_size) {
        size_t amount = vfs_read(node, image + loaded, loaded, image_size - loaded);
        if (amount == (size_t)-1 || amount == 0) {
            plogk("elf_loader: failed to read init executable: %s\n", path);
            free(image);
            vfs_close(node);
            return -EIO;
        }
        if (amount > image_size - loaded) {
            plogk("elf_loader: init executable read overrun: %s\n", path);
            free(image);
            vfs_close(node);
            return -EIO;
        }
        loaded += amount;
    }
    vfs_close(node);

    plogk("elf_loader: init executable found: %s (%llu bytes)\n", path, (unsigned long long)image_size);

    int status = elf_loader_load_initial_process(proc, image, image_size, argv, envp);
    free(image);
    return status;
}
