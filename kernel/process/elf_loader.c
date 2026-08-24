/*
 *
 *      elf_loader.c
 *      ELF64 process-image loader
 *
 *      2026/7/21 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/module/elf.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <process/elf_loader.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/syscall.h>

#define INTERP_LOAD_BASE 0x7f0000000000ULL
#define INTERP_LOAD_END  0x7f0001000000ULL

typedef struct elf_source {
        const uint8_t *data;
        vfs_node_t     node;
        size_t         size;
        uint8_t       *window;
        size_t         window_capacity;
        size_t         window_offset;
        size_t         window_size;
} elf_source_t;

/* Read from the ELF source (memory buffer or VFS node), using the window when useful */
static int elf_source_read(elf_source_t *source, size_t offset, void *buffer, size_t size)
{
    if (!source || (!buffer && size) || offset > source->size || size > source->size - offset) return -EINVAL;
    if (source->data) {
        memcpy(buffer, source->data + offset, size);
        return 0;
    }
    if (!source->node) return -EIO;

    if (source->window && size <= source->window_capacity) {
        bool cached = offset >= source->window_offset && size <= source->window_size && offset - source->window_offset <= source->window_size - size;
        if (!cached) {
            source->window_offset = offset;
            source->window_size   = source->size - offset;
            if (source->window_size > source->window_capacity) source->window_size = source->window_capacity;

            size_t loaded = 0;
            while (loaded < source->window_size) {
                size_t amount = vfs_read(source->node, source->window + loaded, source->window_offset + loaded, source->window_size - loaded);
                if (!amount || amount == (size_t)-1 || amount > source->window_size - loaded) return -EIO;
                loaded += amount;
            }
        }
        memcpy(buffer, source->window + (offset - source->window_offset), size);
        return 0;
    }

    size_t done = 0;
    while (done < size) {
        size_t amount = vfs_read(source->node, (uint8_t *)buffer + done, offset + done, size - done);
        if (!amount || amount == (size_t)-1 || amount > size - done) return -EIO;
        done += amount;
    }
    return 0;
}

/* Validate the ELF header fields relevant to a process image */
static int validate_ehdr(const Elf64_Ehdr *ehdr, size_t size)
{
    if (!ehdr || size < sizeof(Elf64_Ehdr)) return -ENOEXEC;
    if (*(const uint32_t *)ehdr->e_ident != ELF_MAGIC) return -ENOEXEC;
    if (ehdr->e_ident[4] != 2 || ehdr->e_ident[5] != 1 || ehdr->e_ident[6] != 1) return -ENOEXEC;
    if (ehdr->e_machine != 0x3e) return -ENOEXEC;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return -ENOEXEC;
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) return -ENOEXEC;
    if (ehdr->e_phoff > size) return -ENOEXEC;
    if (ehdr->e_phnum > (size - ehdr->e_phoff) / sizeof(Elf64_Phdr)) return -ENOEXEC;
    return 0;
}

/* Validate the image and return its ELF header */
static int validate_elf(const uint8_t *data, size_t size, Elf64_Ehdr **ehdr_out)
{
    if (!data || size < sizeof(Elf64_Ehdr)) return -ENOEXEC;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;
    if (validate_ehdr(ehdr, size)) return -ENOEXEC;
    *ehdr_out = ehdr;
    return 0;
}

/* Extract entry point, program headers, interpreter and TLS metadata from the image */
int elf_loader_parse_elf_info(const uint8_t *data, size_t size, elf_load_info_t *info)
{
    memset(info, 0, sizeof(*info));

    Elf64_Ehdr *ehdr = NULL;
    if (validate_elf(data, size, &ehdr)) return -ENOEXEC;

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
                if (info->has_interp) return -ENOEXEC;
                if (phdr[i].filesz > 1 && phdr[i].filesz < sizeof(info->interp_path)) {
                    if (phdr[i].offset + phdr[i].filesz > size) break;
                    const char *interp = (const char *)data + phdr[i].offset;
                    if (interp[phdr[i].filesz - 1] != '\0') return -ENOEXEC;
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
    if (segment->vaddr > UINT64_MAX - load_bias) return -ENOEXEC;

    uintptr_t base = segment->vaddr + load_bias;
    if (base > UINT64_MAX - segment->memsz) return -ENOEXEC;

    uintptr_t mem_end = base + segment->memsz;
    if (mem_end > UINT64_MAX - (PAGE_4K_SIZE - 1)) return -ENOEXEC;
    uintptr_t end = ALIGN_UP(mem_end, PAGE_4K_SIZE);
    if (end < mem_end) return -ENOEXEC;

    if (base_out) *base_out = base;
    if (start_out) *start_out = ALIGN_DOWN(base, PAGE_4K_SIZE);
    if (end_out) *end_out = end;
    return 1;
}

/* Return the permissions for one page covered by one or more PT_LOADs. */
static int elf_page_attributes(const Elf64_Phdr *phdr, int phnum, uintptr_t load_bias, uintptr_t va, uint64_t *pte_flags_out, vm_flags_t *vm_flags_out)
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

/* Record a non-overlapping VMA for a loaded PT_LOAD range */
static int insert_elf_vma(process_t *proc, uintptr_t start, uintptr_t end, vm_flags_t flags)
{
    if (start >= end) return 0;

    vm_area_t *vma = calloc(1, sizeof(*vma));
    if (!vma) return -ENOMEM;
    vma->start = start;
    vma->end   = end;
    vma->flags = flags;
    vma->type  = VM_REGION_MMAP;
    if (vm_area_insert(proc, vma)) {
        free(vma);
        return -ENOMEM;
    }
    return 0;
}

/* Map PT_LOAD segments from an ELF source into the process and build their VMAs */
static int load_elf_segments_source(process_t *proc, const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdr, elf_source_t *source, uintptr_t load_bias, int set_brk)
{
    uintptr_t lowest_start = UINT64_MAX;
    uintptr_t highest_end  = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;
        if (phdr[i].filesz > phdr[i].memsz) return -ENOEXEC;
        if (phdr[i].offset > source->size || phdr[i].filesz > source->size - phdr[i].offset) return -ENOEXEC;
        if (phdr[i].align > 1 && ((phdr[i].align & (phdr[i].align - 1)) || (phdr[i].vaddr & (phdr[i].align - 1)) != (phdr[i].offset & (phdr[i].align - 1)))) return -ENOEXEC;

        uintptr_t seg_start, seg_end;
        int       range = elf_segment_range(&phdr[i], load_bias, NULL, &seg_start, &seg_end);
        if (range < 0) return -ENOEXEC;
        if (range == 0) continue;
        if (seg_start < PROCESS_HEAP_START || seg_end > PROCESS_USER_STACK_TOP) return -ENOEXEC;
        if (seg_start < lowest_start) lowest_start = seg_start;
        if (seg_end > highest_end) highest_end = seg_end;
    }

    if (lowest_start == UINT64_MAX) return -ENOEXEC;

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
        if (!frame) return -ENOMEM;

        uint8_t *page = phys_to_virt(frame);
        memset(page, 0, PAGE_4K_SIZE);
        if (page_map_new_to(proc->user_page_dir, va, frame, pte_flags)) {
            (void)frame_release_range(frame, 1);
            return -ENOMEM;
        }

        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].type != PT_LOAD || phdr[i].filesz == 0) continue;

            uintptr_t base;
            if (elf_segment_range(&phdr[i], load_bias, &base, NULL, NULL) <= 0) continue;
            uintptr_t file_start = MAX(va, base);
            uintptr_t file_end   = MIN(va + PAGE_4K_SIZE, base + phdr[i].filesz);
            if (file_start >= file_end) continue;

            uintptr_t file_delta = file_start - base;
            if (phdr[i].offset > source->size || file_delta > source->size - phdr[i].offset) return -ENOEXEC;
            uint64_t file_offset = phdr[i].offset + file_delta;
            size_t   copy_size   = file_end - file_start;
            if (copy_size > source->size - file_offset) return -ENOEXEC;
            int read_ret = elf_source_read(source, file_offset, page + (file_start - va), copy_size);
            if (read_ret) return read_ret;
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
            if (run_start && insert_elf_vma(proc, run_start, run_end, run_flags)) return -ENOMEM;
            run_start = 0;
            continue;
        }
        (void)pte_flags;

        if (!run_start) {
            run_start = va;
            run_end   = va + PAGE_4K_SIZE;
            run_flags = vm_flags;
        } else if (run_end != va || run_flags != vm_flags) {
            if (insert_elf_vma(proc, run_start, run_end, run_flags)) return -ENOMEM;
            run_start = va;
            run_end   = va + PAGE_4K_SIZE;
            run_flags = vm_flags;
        } else {
            run_end += PAGE_4K_SIZE;
        }
    }
    if (run_start && insert_elf_vma(proc, run_start, run_end, run_flags)) return -ENOMEM;

    if (set_brk && highest_end > PROCESS_HEAP_START) proc->start_brk = proc->heap_brk = highest_end;
    return 0;
}

/* Map all PT_LOAD segments of a memory-backed image and build their VMAs */
static int load_elf_segments(process_t *proc, const Elf64_Ehdr *ehdr, const uint8_t *data, size_t elf_size, uintptr_t load_bias, int set_brk)
{
    const Elf64_Phdr *phdr   = (const Elf64_Phdr *)(data + ehdr->e_phoff);
    elf_source_t      source = {.data = data, .node = NULL, .size = elf_size};
    return load_elf_segments_source(proc, ehdr, phdr, &source, load_bias, set_brk);
}

/* Translate a user virtual address to a directly writable kernel pointer */
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

/* Copy to user memory, resolving demand-fault pages as needed */
static int write_user(process_t *proc, uintptr_t dst, const void *src, size_t size)
{
    const uint8_t *in = src;
    while (size) {
        void *out = user_ptr(proc, dst);
        if (!out && process_demand_fault(proc, dst, 1, 0) == 0) out = user_ptr(proc, dst);
        if (!out) return -EFAULT;
        size_t chunk = PAGE_4K_SIZE - (dst & (PAGE_4K_SIZE - 1));
        if (chunk > size) chunk = size;
        memcpy(out, in, chunk);
        dst += chunk;
        in += chunk;
        size -= chunk;
    }
    return 0;
}

/* Find a free page-aligned range of the requested size in the mmap region */
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

/* Compute the load bias that places the image's lowest PT_LOAD at the chosen base */
static uintptr_t compute_load_bias(const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdr, uintptr_t chosen_base)
{
    if (ehdr->e_type == ET_DYN) {
        uintptr_t lowest = UINT64_MAX;
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

/* Load a dynamic linker and return its relocated base and entry point */
int elf_loader_load_interpreter(struct process *proc, const char *interp_path, Elf64_Addr *base_out, Elf64_Addr *entry_out)
{
    char resolved_path[VFS_PATH_MAX];
    int  resolve_status = process_resolve_path_at(proc, PROCESS_AT_FDCWD, interp_path, resolved_path, sizeof(resolved_path));
    if (resolve_status != EOK) return resolve_status;

    vfs_node_t node = vfs_open(resolved_path);
    if (!node) return -ENOENT;

    if (node->size == 0 || node->size > 0x400000) {
        vfs_close(node);
        return -EINVAL;
    }

    elf_source_t source = {.data = NULL, .node = node, .size = (size_t)node->size};
    Elf64_Ehdr   ehdr;
    int          result = elf_source_read(&source, 0, &ehdr, sizeof(ehdr));
    if (result || validate_ehdr(&ehdr, source.size)) {
        vfs_close(node);
        return result ? result : -ENOEXEC;
    }

    size_t      phdr_size = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdr      = malloc(phdr_size);
    if (!phdr) {
        vfs_close(node);
        return -ENOMEM;
    }
    result = elf_source_read(&source, (size_t)ehdr.e_phoff, phdr, phdr_size);
    if (result) goto out;

    uintptr_t image_start = UINT64_MAX;
    uintptr_t image_end   = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;
        uintptr_t start, end;
        int       range = elf_segment_range(&phdr[i], 0, NULL, &start, &end);
        if (range < 0) {
            result = -ENOEXEC;
            goto out;
        }
        if (!range) continue;
        if (start < image_start) image_start = start;
        if (end > image_end) image_end = end;
    }
    if (image_start == UINT64_MAX || image_end <= image_start) {
        result = -ENOEXEC;
        goto out;
    }

    uintptr_t interp_base = find_free_range(proc, INTERP_LOAD_BASE, INTERP_LOAD_END, image_end - image_start);
    if (!interp_base) {
        result = -ENOMEM;
        goto out;
    }

    uintptr_t load_bias = compute_load_bias(&ehdr, phdr, interp_base);

    /* Stream the linker through a small window instead of copying its entire file per exec. */
    source.window_capacity = (size_t)64U * 1024U;
    source.window          = malloc(source.window_capacity);
    if (!source.window) source.window_capacity = 0;

    result = load_elf_segments_source(proc, &ehdr, phdr, &source, load_bias, 0);
    if (result) goto out;

    int valid_entry = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].type == PT_LOAD && (phdr[i].flags & PF_X)) {
            uintptr_t entry = ehdr.e_entry + load_bias;
            uintptr_t start = phdr[i].vaddr + load_bias;
            if (entry >= start && phdr[i].memsz <= UINT64_MAX - start && entry < start + phdr[i].memsz) valid_entry = 1;
        }
        if (valid_entry) break;
    }
    if (!valid_entry) {
        result = -ENOEXEC;
        goto out;
    }

    *base_out  = load_bias;
    *entry_out = ehdr.e_entry + load_bias;
    result     = 0;
out:
    free(source.window);
    free(phdr);
    vfs_close(node);
    return result;
}

/* Count the entries in a NULL-terminated string array */
static int count_string_array(char *const arr[])
{
    if (!arr) return 0;
    int count = 0;
    for (int i = 0; arr[i]; i++) count++;
    return count;
}

/* Total byte size of the strings in a NULL-terminated array, including terminators */
static size_t string_array_size(char *const arr[])
{
    if (!arr) return 0;
    size_t total = 0;
    for (int i = 0; arr[i]; i++) total += strlen(arr[i]) + 1;
    return total;
}

/* Build the initial user stack: argv/envp/auxv vectors and the strings they point to */
static int setup_user_stack(process_t *proc, uintptr_t phdr_addr, uint16_t phnum, uint16_t phentsize, uintptr_t interp_base, uintptr_t main_entry, char *const argv[], char *const envp[],
                            uintptr_t *rsp_out)
{
    int         argc      = count_string_array(argv);
    int         envc      = count_string_array(envp);
    size_t      argv_strs = string_array_size(argv);
    size_t      envp_strs = string_array_size(envp);
    const char *execfn    = argc > 0 ? argv[0] : proc->name;

    const size_t aux_pairs    = 19;
    size_t       vector_words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + aux_pairs * 2;
    size_t       strings_size = argv_strs + envp_strs + strlen(execfn) + 1 + sizeof("x86_64") + 16;
    size_t       total_needed = ALIGN_UP(vector_words * sizeof(uint64_t) + strings_size + 16, 16);
    if (total_needed > (size_t)PROCESS_STACK_SIZE) return -ENOMEM;
    uintptr_t base_rsp    = ALIGN_DOWN(PROCESS_USER_STACK_TOP - total_needed, 16);
    uintptr_t string_area = base_rsp + vector_words * sizeof(uint64_t);
    uint64_t *vectors     = calloc(vector_words, sizeof(uint64_t));
    uint8_t  *strings     = calloc(1, strings_size);
    if (!vectors || !strings) {
        free(vectors);
        free(strings);
        return -ENOMEM;
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
    uint32_t hwcap_eax, hwcap_ebx, hwcap_ecx, hwcap_edx;
    cpuid_safe(1, 0, &hwcap_eax, &hwcap_ebx, &hwcap_ecx, &hwcap_edx);
    (void)hwcap_eax;
    (void)hwcap_ebx;
    (void)hwcap_ecx;
    vectors[n++] = AT_HWCAP;
    vectors[n++] = hwcap_edx;
    vectors[n++] = AT_HWCAP2;
    vectors[n++] = 0;
    vectors[n++] = AT_CLKTCK;
    vectors[n++] = TIMER_USER_HZ;
    vectors[n++] = AT_RANDOM;
    vectors[n++] = random_addr;
    vectors[n++] = AT_EXECFN;
    vectors[n++] = execfn_addr;
    vectors[n++] = AT_NULL;
    vectors[n++] = 0;

    int ret = write_user(proc, base_rsp, vectors, vector_words * sizeof(uint64_t));
    if (!ret) ret = write_user(proc, string_area, strings, strings_size);
    free(vectors);
    free(strings);
    if (ret) return ret;

    *rsp_out = base_rsp;
    return 0;
}

/*
 * Trampoline that enters the user image with a clean register file via IRETQ.
 * Reached from context_switch() with %gs -> per-CPU; swap back to the task's
 * user GS (parked in KERNEL_GS_BASE by the scheduler) before iretq.
 */
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
                     "cli\n\t"
                     "swapgs\n\t"
                     "iretq\n\t");
}

/* Load a memory-backed ELF into the process: map segments, set up the stack and registers */
int elf_loader_load_process_internal(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[], uintptr_t *entry_out, uintptr_t *rsp_out)
{
    Elf64_Ehdr *ehdr = NULL;
    if (validate_elf(elf_data, elf_size, &ehdr)) return -ENOEXEC;

    elf_load_info_t info;
    if (elf_loader_parse_elf_info(elf_data, elf_size, &info)) return -ENOEXEC;

    uintptr_t load_bias = 0;
    if (ehdr->e_type == ET_DYN) {
        uintptr_t         chosen_base = PROCESS_USER_CODE_MIN;
        const Elf64_Phdr *load_phdr   = (const Elf64_Phdr *)(elf_data + ehdr->e_phoff);
        load_bias                     = compute_load_bias(ehdr, load_phdr, chosen_base);
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

    int load_ret = load_elf_segments(proc, ehdr, elf_data, elf_size, load_bias, 1);
    if (load_ret) return load_ret;

    if (process_mmap(proc, proc->stack_brk, (size_t)PROCESS_STACK_SIZE, VM_READ | VM_WRITE | VM_LAZY)) return -ENOMEM;

    proc->task->thread.fs_base = 0;
    proc->task->thread.gs_base = 0;

    Elf64_Addr interpreter_base  = 0;
    Elf64_Addr interpreter_entry = 0;
    uintptr_t  actual_entry      = ehdr->e_entry + load_bias;

    if (info.has_interp) {
        int interp_ret = elf_loader_load_interpreter(proc, info.interp_path, &interpreter_base, &interpreter_entry);
        if (interp_ret) return interp_ret;
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
    if (!valid_entry) return -ENOEXEC;

    uintptr_t user_rsp  = 0;
    int       stack_ret = setup_user_stack(proc, phdr_addr, ehdr->e_phnum, ehdr->e_phentsize, interpreter_base, ehdr->e_entry + load_bias, argv, envp, &user_rsp);
    if (stack_ret) return stack_ret;

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

/*
 * Load a normal exec image directly from its VFS node.  Only the ELF and
 * program headers are retained in kernel heap; PT_LOAD contents are copied
 * one page at a time.  Large compiler backends can therefore exec in
 * parallel without requiring one power-of-two heap allocation per image.
 */
int elf_loader_load_user_node(process_t *proc, vfs_node_t node, char *const argv[], char *const envp[], uintptr_t *entry_out, uintptr_t *rsp_out)
{
    if (!proc || !node || !node->size || node->size > SIZE_MAX) return -EINVAL;

    elf_source_t source = {.data = NULL, .node = node, .size = (size_t)node->size};
    Elf64_Ehdr   ehdr;
    int          header_ret = elf_source_read(&source, 0, &ehdr, sizeof(ehdr));
    if (header_ret) return header_ret;
    if (validate_ehdr(&ehdr, source.size)) return -ENOEXEC;

    size_t      phdr_size = (size_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs     = malloc(phdr_size);
    if (!phdrs) return -ENOMEM;

    int phdr_ret = elf_source_read(&source, (size_t)ehdr.e_phoff, phdrs, phdr_size);
    if (phdr_ret) {
        free(phdrs);
        return phdr_ret;
    }

    char interp_path[256] = {0};
    int  has_interp       = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].type != PT_INTERP) continue;
        if (has_interp || phdrs[i].filesz <= 1 || phdrs[i].filesz >= sizeof(interp_path)) {
            free(phdrs);
            return -ENOEXEC;
        }
        int interp_ret = elf_source_read(&source, (size_t)phdrs[i].offset, interp_path, (size_t)phdrs[i].filesz);
        if (interp_ret) {
            free(phdrs);
            return interp_ret;
        }
        if (interp_path[phdrs[i].filesz - 1] != '\0') {
            free(phdrs);
            return -ENOEXEC;
        }
        has_interp = 1;
    }

    /*
     * A bounded read-ahead window keeps large node-backed images streaming
     * while avoiding one VFS/page-cache lookup for every mapped 4 KiB page.
     */
    source.window_capacity = (size_t)1024 * 1024;
    source.window          = malloc(source.window_capacity);
    if (!source.window) source.window_capacity = 0;

    uintptr_t load_bias = 0;
    if (ehdr.e_type == ET_DYN) load_bias = compute_load_bias(&ehdr, phdrs, PROCESS_USER_CODE_MIN);

    uintptr_t phdr_addr = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].type == PT_PHDR) {
            phdr_addr = phdrs[i].vaddr + load_bias;
            break;
        }
    }
    if (!phdr_addr) {
        for (int i = 0; i < ehdr.e_phnum; i++) {
            if (phdrs[i].type != PT_LOAD) continue;
            if (ehdr.e_phoff >= phdrs[i].offset && ehdr.e_phoff + phdr_size <= phdrs[i].offset + phdrs[i].filesz) {
                phdr_addr = phdrs[i].vaddr + load_bias + (ehdr.e_phoff - phdrs[i].offset);
                break;
            }
        }
    }

    int seg_ret = load_elf_segments_source(proc, &ehdr, phdrs, &source, load_bias, 1);
    if (seg_ret) {
        free(source.window);
        free(phdrs);
        return seg_ret;
    }
    if (process_mmap(proc, proc->stack_brk, (size_t)PROCESS_STACK_SIZE, VM_READ | VM_WRITE | VM_LAZY)) {
        free(source.window);
        free(phdrs);
        return -ENOMEM;
    }

    proc->task->thread.fs_base = 0;
    proc->task->thread.gs_base = 0;

    Elf64_Addr interpreter_base  = 0;
    Elf64_Addr interpreter_entry = 0;
    uintptr_t  actual_entry      = ehdr.e_entry + load_bias;
    if (has_interp) {
        int interp_ret = elf_loader_load_interpreter(proc, interp_path, &interpreter_base, &interpreter_entry);
        if (interp_ret) {
            free(source.window);
            free(phdrs);
            return interp_ret;
        }
        actual_entry = interpreter_entry;
    }

    int valid_entry = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].type != PT_LOAD || !(phdrs[i].flags & PF_X)) continue;
        uintptr_t entry     = ehdr.e_entry + load_bias;
        uintptr_t seg_start = phdrs[i].vaddr + load_bias;
        if (entry >= seg_start && entry < seg_start + phdrs[i].memsz) {
            valid_entry = 1;
            break;
        }
    }
    if (!valid_entry) {
        free(source.window);
        free(phdrs);
        return -ENOEXEC;
    }

    uintptr_t user_rsp  = 0;
    int       stack_ret = setup_user_stack(proc, phdr_addr, ehdr.e_phnum, ehdr.e_phentsize, interpreter_base, ehdr.e_entry + load_bias, argv, envp, &user_rsp);
    if (stack_ret) {
        free(source.window);
        free(phdrs);
        return stack_ret;
    }

    proc->task->context.rbx    = 0;
    proc->task->context.rbp    = 0;
    proc->task->context.r12    = 0;
    proc->task->context.r13    = 0;
    proc->task->context.r14    = 0;
    proc->task->context.r15    = 0;
    proc->task->context.rflags = 0x202;
    proc->task->context.rdi    = 0;
    if (entry_out) *entry_out = actual_entry;
    if (rsp_out) *rsp_out = user_rsp;
    free(source.window);
    free(phdrs);
    return 0;
}

/*
 * Load the system init executable from the already-populated root filesystem.
 * Limine only supplies the initramfs; PID 1 must follow the normal Unix boot
 * contract and execute /sbin/init from that filesystem.
 */
int elf_loader_load_initial_path(process_t *proc, const char *path, char *const argv[], char *const envp[])
{
    if (!proc || !path || !path[0]) return -EINVAL;

    vfs_node_t node = vfs_open(path);
    if (!node) return -ENOENT;
    if (node->type & file_dir) {
        vfs_close(node);
        return -EISDIR;
    }
    if (!node->size || node->size > 64ULL * 1024ULL * 1024ULL) {
        vfs_close(node);
        return -EINVAL;
    }

    size_t   image_size = (size_t)node->size;
    uint8_t *image      = malloc(image_size);
    if (!image) {
        vfs_close(node);
        return -ENOMEM;
    }

    size_t loaded = 0;
    while (loaded < image_size) {
        size_t amount = vfs_read(node, image + loaded, loaded, image_size - loaded);
        if (amount == (size_t)-1 || amount == 0) {
            free(image);
            vfs_close(node);
            return -EIO;
        }
        if (amount > image_size - loaded) {
            free(image);
            vfs_close(node);
            return -EIO;
        }
        loaded += amount;
    }
    vfs_close(node);

    int status = elf_loader_load_initial_process(proc, image, image_size, argv, envp);
    free(image);
    return status;
}
