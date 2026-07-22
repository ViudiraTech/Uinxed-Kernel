#include <fs/vfs.h>
#include <kernel/elf.h>
#include <kernel/elf_loader.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/uaccess.h>
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
    if (ehdr->e_phoff + ehdr->e_phnum * ehdr->e_phentsize > size) return -1;
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
                if (phdr[i].filesz > 0 && phdr[i].filesz < sizeof(info->interp_path)) {
                    const char *interp = (const char *)data + phdr[i].offset;
                    memcpy(info->interp_path, interp, phdr[i].filesz);
                    if (info->interp_path[phdr[i].filesz - 1] == '\0')
                        info->has_interp = 1;
                    else {
                        info->interp_path[phdr[i].filesz] = '\0';
                        info->has_interp                  = 1;
                    }
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
        }
    }
    return 0;
}

static int load_elf_segments(process_t *proc, const Elf64_Ehdr *ehdr, const uint8_t *data, uintptr_t load_bias)
{
    const Elf64_Phdr *phdr        = (const Elf64_Phdr *)(data + ehdr->e_phoff);
    uintptr_t         highest_end = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;
        uintptr_t seg_end = ALIGN_UP(phdr[i].vaddr + load_bias + phdr[i].memsz, PAGE_4K_SIZE);
        if (seg_end > highest_end) highest_end = seg_end;
    }

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;

        uint64_t pte_flags = PTE_USER | PTE_PRESENT;
        if (phdr[i].flags & PF_W) pte_flags |= PTE_WRITEABLE;
        if (!(phdr[i].flags & PF_X)) pte_flags |= PTE_NO_EXECUTE;

        uintptr_t base      = phdr[i].vaddr + load_bias;
        uintptr_t seg_start = ALIGN_DOWN(base, PAGE_4K_SIZE);
        uintptr_t seg_end   = ALIGN_UP(base + phdr[i].memsz, PAGE_4K_SIZE);

        for (uintptr_t va = seg_start; va < seg_end; va += PAGE_4K_SIZE) {
            uint64_t frame = alloc_frames(1);
            if (!frame) return 1;

            uint8_t *page = phys_to_virt(frame);
            memset(page, 0, PAGE_4K_SIZE);

            uintptr_t file_start = MAX(va, base);
            uintptr_t file_end   = MIN(va + PAGE_4K_SIZE, base + phdr[i].filesz);
            if (file_start < file_end) {
                size_t page_offset = file_start - va;
                size_t file_offset = phdr[i].offset + file_start - base;
                memcpy(page + page_offset, data + file_offset, file_end - file_start);
            }
            page_map_to(proc->user_page_dir, va, frame, pte_flags);
        }
    }

    if (highest_end > PROCESS_HEAP_START) proc->heap_brk = highest_end;
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
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff);
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].type == PT_LOAD) { return ALIGN_DOWN(chosen_base - phdr[i].vaddr, PAGE_4K_SIZE); }
        }
        return ALIGN_DOWN(chosen_base, PAGE_4K_SIZE);
    }
    return 0;
}

int elf_loader_load_interpreter(struct process *proc, const char *interp_path, Elf64_Addr *base_out, Elf64_Addr *entry_out)
{
    vfs_node_t node = vfs_open(interp_path);
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
        vfs_close(node);
        return -1;
    }

    size_t total = 0;
    while (total < node->size) {
        size_t remaining = node->size - total;
        size_t to_read   = remaining < 4096 ? remaining : 4096;
        size_t n         = vfs_read(node->handle, elf_data + total, total, to_read);
        if (n == 0) break;
        total += n;
    }
    vfs_close(node);

    if (total < sizeof(Elf64_Ehdr)) {
        free(elf_data);
        return -1;
    }

    Elf64_Ehdr *iehdr = NULL;
    if (validate_elf(elf_data, total, &iehdr)) {
        plogk("elf_loader: invalid interpreter ELF: %s\n", interp_path);
        free(elf_data);
        return -1;
    }

    uintptr_t interp_base = find_free_range(proc, INTERP_LOAD_BASE, INTERP_LOAD_END, total);
    if (!interp_base) {
        plogk("elf_loader: no free space for interpreter\n");
        free(elf_data);
        return -1;
    }

    uintptr_t load_bias = compute_load_bias(iehdr, elf_data, interp_base);

    if (load_elf_segments(proc, iehdr, elf_data, load_bias)) {
        plogk("elf_loader: failed to load interpreter segments\n");
        free(elf_data);
        return -1;
    }

    const Elf64_Phdr *iphdr            = (const Elf64_Phdr *)(elf_data + iehdr->e_phoff);
    uintptr_t         interp_map_start = (uintptr_t)-1;
    uintptr_t         interp_map_end   = 0;
    for (int i = 0; i < iehdr->e_phnum; i++) {
        if (iphdr[i].type != PT_LOAD) continue;
        uintptr_t va_start = ALIGN_DOWN(iphdr[i].vaddr + load_bias, PAGE_4K_SIZE);
        uintptr_t va_end   = ALIGN_UP(iphdr[i].vaddr + load_bias + iphdr[i].memsz, PAGE_4K_SIZE);
        if (va_start < interp_map_start) interp_map_start = va_start;
        if (va_end > interp_map_end) interp_map_end = va_end;
    }
    if (interp_map_start < interp_map_end) {
        process_mmap(proc, interp_map_start, interp_map_end - interp_map_start, VM_READ | VM_WRITE | VM_EXEC);
    }

    uintptr_t first_load_vaddr = 0;
    for (int i = 0; i < iehdr->e_phnum; i++) {
        if (iphdr[i].type == PT_LOAD) {
            first_load_vaddr = iphdr[i].vaddr;
            break;
        }
    }

    *base_out  = interp_base;
    *entry_out = iehdr->e_entry + (interp_base - first_load_vaddr);

    free(elf_data);
    plogk("elf_loader: loaded interpreter %s base=%p entry=%p\n", interp_path, (void *)*base_out, (void *)*entry_out);
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
    int    argc      = count_string_array(argv);
    int    envc      = count_string_array(envp);
    size_t argv_strs = string_array_size(argv);
    size_t envp_strs = string_array_size(envp);

    size_t total_strings = argv_strs + envp_strs + strlen(proc->name) + 1;
    size_t pointer_size  = (argc + 1 + envc + 1) * sizeof(uint64_t);
    size_t auxv_entries  = 18;
    size_t auxv_size     = auxv_entries * 2 * sizeof(uint64_t) + 2 * sizeof(uint64_t);
    size_t padding       = 16;

    size_t total_needed = pointer_size + auxv_size + total_strings + padding + 32;
    total_needed        = ALIGN_UP(total_needed, 16);

    uintptr_t top = PROCESS_USER_STACK_TOP;
    uintptr_t rsp = top - total_needed;

    uint8_t *stack_area = user_ptr(proc, rsp);
    if (!stack_area) return 0;
    memset(stack_area, 0, total_needed);

    /* Layout: [strings][padding][auxv][envp ptrs][argv ptrs][argc] */
    uintptr_t string_area = rsp;
    uintptr_t auxv_area   = string_area + total_strings + padding;
    uintptr_t envp_area   = auxv_area + auxv_size;
    uintptr_t argv_area   = envp_area + (envc + 1) * sizeof(uint64_t);
    uintptr_t argc_addr   = argv_area + (argc + 1) * sizeof(uint64_t);
    uintptr_t base_rsp    = argc_addr;

    char *string_ptr = user_ptr(proc, string_area);
    if (!string_ptr) return 0;

    uint64_t *argv_ptr = user_ptr(proc, argv_area);
    if (!argv_ptr) return 0;

    uint64_t *envp_ptr = user_ptr(proc, envp_area);
    if (!envp_ptr) return 0;

    uint64_t *argc_ptr = user_ptr(proc, argc_addr);
    if (!argc_ptr) return 0;

    uint64_t *auxv_ptr = user_ptr(proc, auxv_area);
    if (!auxv_ptr) return 0;

    char *sp = string_ptr;

    /* Copy program name */
    size_t name_len = strlen(proc->name) + 1;
    memcpy(sp, proc->name, name_len);
    sp += name_len;

    /* Copy argv strings */
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        memcpy(sp, argv[i], len);
        argv_ptr[i] = string_area + (sp - string_ptr);
        sp += len;
    }
    argv_ptr[argc] = 0;

    /* Copy envp strings */
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        memcpy(sp, envp[i], len);
        envp_ptr[i] = string_area + (sp - string_ptr);
        sp += len;
    }
    envp_ptr[envc] = 0;

    /* Build auxv */
    size_t     n              = 0;
    const char platform_str[] = "x86_64";
    memcpy(sp, platform_str, sizeof(platform_str));
    uintptr_t platform_addr = string_area + (sp - string_ptr);
    sp += sizeof(platform_str);

    /* 16 random bytes */
    uintptr_t random_addr = string_area + (sp - string_ptr);
    for (int i = 0; i < 16; i++) sp[i] = (uint8_t)(0xa5 + i);
    sp += 16;

    auxv_ptr[n++] = AT_PHDR;
    auxv_ptr[n++] = phdr_addr;
    auxv_ptr[n++] = AT_PHENT;
    auxv_ptr[n++] = phentsize;
    auxv_ptr[n++] = AT_PHNUM;
    auxv_ptr[n++] = phnum;
    auxv_ptr[n++] = AT_PAGESZ;
    auxv_ptr[n++] = PAGE_4K_SIZE;
    auxv_ptr[n++] = AT_BASE;
    auxv_ptr[n++] = interp_base;
    auxv_ptr[n++] = AT_FLAGS;
    auxv_ptr[n++] = 0;
    auxv_ptr[n++] = AT_ENTRY;
    auxv_ptr[n++] = main_entry;
    auxv_ptr[n++] = AT_UID;
    auxv_ptr[n++] = proc->uid;
    auxv_ptr[n++] = AT_EUID;
    auxv_ptr[n++] = proc->uid;
    auxv_ptr[n++] = AT_GID;
    auxv_ptr[n++] = proc->gid;
    auxv_ptr[n++] = AT_EGID;
    auxv_ptr[n++] = proc->gid;
    auxv_ptr[n++] = AT_SECURE;
    auxv_ptr[n++] = 0;
    auxv_ptr[n++] = AT_PLATFORM;
    auxv_ptr[n++] = platform_addr;
    auxv_ptr[n++] = AT_RANDOM;
    auxv_ptr[n++] = random_addr;
    auxv_ptr[n++] = AT_EXECFN;
    auxv_ptr[n++] = string_area;
    auxv_ptr[n++] = AT_NULL;
    auxv_ptr[n++] = 0;

    *argc_ptr = (uint64_t)argc;

    return base_rsp;
}

__attribute__((naked)) static void user_process_enter(void)
{
    __asm__ volatile("movq %rdi, %rax\n\t"
                     "movq %rdi, %rdx\n\t"
                     "shrq $32, %rdx\n\t"
                     "movl $0xC0000100, %ecx\n\t"
                     "wrmsr\n\t"
                     "iretq\n\t");
}

int elf_loader_load_user_process(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[])
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
                phdr_addr = phdrs[i].vaddr + load_bias + ehdr->e_phoff;
                break;
            }
        }
    }

    if (load_elf_segments(proc, ehdr, elf_data, load_bias)) {
        plogk("elf_loader: Failed to load ELF segments.\n");
        return 1;
    }

    if (process_mmap(proc, proc->stack_brk, PROCESS_STACK_SIZE, VM_READ | VM_WRITE)) {
        plogk("elf_loader: Failed to allocate user stack.\n");
        return 1;
    }

    vfs_node_t console = vfs_open("/dev/console");
    if (console) {
        int std_fd = process_fd_install(proc, console, O_RDWR);
        if (std_fd == 0) {
            process_fd_dup2(proc, 0, 1);
            process_fd_dup2(proc, 0, 2);
        }
    } else {
        plogk("elf_loader: warning - /dev/console not found.\n");
    }

    uint64_t tls_frame = alloc_frames(1);
    if (!tls_frame) {
        plogk("elf_loader: Failed to allocate TLS frame.\n");
        return 1;
    }
    memset(phys_to_virt(tls_frame), 0, PAGE_4K_SIZE);

    /* Initialize TLS area from PT_TLS segment data and BSS */
    if (info.tls_size > 0) {
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].type == PT_TLS) {
                uint64_t copy_size = phdrs[i].filesz;
                if (copy_size > PAGE_4K_SIZE) copy_size = PAGE_4K_SIZE;
                if (copy_size > 0) { memcpy(phys_to_virt(tls_frame), elf_data + phdrs[i].offset, copy_size); }
                break;
            }
        }
    }

    uintptr_t tls_user_addr = 0x700000000000ULL;
    page_map_to(proc->user_page_dir, tls_user_addr, tls_frame, PTE_USER | PTE_PRESENT | PTE_WRITEABLE);
    proc->task->thread.fs_base = tls_user_addr;

    Elf64_Addr interpreter_base  = 0;
    Elf64_Addr interpreter_entry = 0;
    uintptr_t  actual_entry      = ehdr->e_entry + load_bias;

    if (info.has_interp) {
        plogk("elf_loader: program requires interpreter: %s\n", info.interp_path);
        if (elf_loader_load_interpreter(proc, info.interp_path, &interpreter_base, &interpreter_entry)) {
            plogk("elf_loader: Failed to load interpreter, running static.\n");
        } else {
            actual_entry = interpreter_entry;
        }
    }

    if (!info.has_interp && info.is_dynamic) {
        plogk("elf_loader: PT_DYNAMIC without PT_INTERP, trying musl interpreter\n");
        if (elf_loader_load_interpreter(proc, MUSL_INTERPRETER_PATH, &interpreter_base, &interpreter_entry) == 0) {
            actual_entry    = interpreter_entry;
            info.has_interp = 1;
        }
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
    proc->task->context.rdi    = tls_user_addr;

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

    plogk("elf_loader: Loaded process %llu (%s) entry=%p\n", proc->task->pid, proc->task->name, (void *)actual_entry);
    return 0;
}
