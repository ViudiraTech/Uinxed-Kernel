#include <elf_loader.h>
#include <frame.h>
#include <hhdm.h>
#include <page.h>
#include <printk.h>
#include <process.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <vfs.h>

#define ELF_MAGIC 0x464c457f
#define PT_LOAD   0x1
#define PF_X      (0x1 << 0)
#define PF_W      (0x1 << 1)
#define PF_R      (0x1 << 2)

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
} elf_ehdr_t;

typedef struct {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t vaddr;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t align;
} elf_phdr_t;

static elf_ehdr_t *validate_elf(const uint8_t *data, size_t size)
{
    elf_ehdr_t *ehdr = (elf_ehdr_t *)data;

    if (size < sizeof(elf_ehdr_t)) return NULL;
    if (*(const uint32_t *)ehdr->ident != ELF_MAGIC) return NULL;
    if (ehdr->ident[4] != 2 || ehdr->ident[5] != 1 || ehdr->ident[6] != 1) return NULL;
    if (ehdr->machine != 0x3e) return NULL;
    if (ehdr->phoff + ehdr->phnum * ehdr->phentsize > size) return NULL;
    return ehdr;
}

static int load_elf_segments(process_t *proc, const elf_ehdr_t *ehdr, const uint8_t *data)
{
    const elf_phdr_t *phdr        = (const elf_phdr_t *)(data + ehdr->phoff);
    uintptr_t         highest_end = 0;

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

static uintptr_t setup_user_stack(process_t *proc, const elf_ehdr_t *ehdr)
{
    uintptr_t top         = PROCESS_USER_STACK_TOP;
    uintptr_t random_addr = top - 32;
    uintptr_t string_addr = top - 64;
    uintptr_t rsp         = top - 512;
    size_t    name_len    = strlen(proc->name) + 1;

    char *name_dst = user_ptr(proc, string_addr);
    if (!name_dst) return 0;
    memcpy(name_dst, proc->name, name_len);

    uint8_t *random = user_ptr(proc, random_addr);
    if (!random) return 0;
    for (int i = 0; i < 16; i++) random[i] = (uint8_t)(0xa5 + i);

    uint64_t *stack = user_ptr(proc, rsp);
    if (!stack) return 0;

    size_t n   = 0;
    stack[n++] = 1;           /* argc */
    stack[n++] = string_addr; /* argv[0] */
    stack[n++] = 0;           /* argv[1] */
    stack[n++] = 0;           /* envp[0] */
    stack[n++] = 3;
    stack[n++] = ehdr->phoff ? PROCESS_USER_CODE_MIN + ehdr->phoff : 0; /* AT_PHDR */
    stack[n++] = 4;
    stack[n++] = ehdr->phentsize; /* AT_PHENT */
    stack[n++] = 5;
    stack[n++] = ehdr->phnum; /* AT_PHNUM */
    stack[n++] = 6;
    stack[n++] = PAGE_4K_SIZE; /* AT_PAGESZ */
    stack[n++] = 9;
    stack[n++] = ehdr->entry; /* AT_ENTRY */
    stack[n++] = 11;
    stack[n++] = proc->uid; /* AT_UID */
    stack[n++] = 12;
    stack[n++] = proc->uid; /* AT_EUID */
    stack[n++] = 13;
    stack[n++] = proc->gid; /* AT_GID */
    stack[n++] = 14;
    stack[n++] = proc->gid; /* AT_EGID */
    stack[n++] = 23;
    stack[n++] = 0; /* AT_SECURE */
    stack[n++] = 25;
    stack[n++] = random_addr; /* AT_RANDOM */
    stack[n++] = 0;
    stack[n++] = 0; /* AT_NULL */
    return rsp;
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

int elf_loader_load_user_process(process_t *proc, const uint8_t *elf_data, size_t elf_size)
{
    elf_ehdr_t *ehdr = validate_elf(elf_data, elf_size);
    if (!ehdr) {
        plogk("elf_loader: Invalid ELF binary.\n");
        return 1;
    }

    if (load_elf_segments(proc, ehdr, elf_data)) {
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
    uintptr_t tls_user_addr = 0x700000000000ULL;
    page_map_to(proc->user_page_dir, tls_user_addr, tls_frame, PTE_USER | PTE_PRESENT | PTE_WRITEABLE);

    uintptr_t user_rsp = setup_user_stack(proc, ehdr);
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
    *(--kstack) = ehdr->entry;
    *(--kstack) = (uint64_t)user_process_enter;

    proc->task->context.rsp = (uint64_t)kstack;
    proc->task->state       = TASK_READY;

    plogk("elf_loader: Loaded ELF into process %llu (%s), entry=%p\n", proc->task->pid, proc->task->name, (void *)ehdr->entry);
    return 0;
}
