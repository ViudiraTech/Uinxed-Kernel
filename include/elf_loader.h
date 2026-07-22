#ifndef INCLUDE_ELF_LOADER_H_
#define INCLUDE_ELF_LOADER_H_

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

struct process;

#define MUSL_INTERPRETER_PATH "/lib/ld-musl-x86_64.so.1"

typedef struct {
        Elf64_Addr base_addr;
        Elf64_Addr entry;
        uintptr_t  phdr;
        uint16_t   phnum;
        uint16_t   phentsize;
        int        is_dynamic;
        int        has_interp;
        char       interp_path[256];
        Elf64_Addr tls_align;
        Elf64_Addr tls_size;
        Elf64_Addr tls_vaddr;
        Elf64_Addr pt_dynamic_vaddr;
} elf_load_info_t;

int elf_loader_load_user_process(struct process *proc, const uint8_t *elf_data, size_t elf_size,
                                 char *const argv[], char *const envp[]);
int elf_loader_parse_elf_info(const uint8_t *elf_data, size_t elf_size, elf_load_info_t *info);
int elf_loader_load_interpreter(struct process *proc, const char *interp_path, Elf64_Addr *base_out, Elf64_Addr *entry_out);

#endif /* INCLUDE_ELF_LOADER_H_ */
