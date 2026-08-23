/*
 *
 *      symbols.c
 *      Symbol table
 *
 *      2025/5/4 By suhuajun
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <kernel/debug/symbols.h>
#include <kernel/uinxed.h>
#include <libs/std/string.h>

/* Get symbol information */
sym_info_t get_symbol_info(uint64_t *kernel_file_address, Elf64_Addr symbol_address)
{
    sym_info_t  sym_info = {0, 0, 0};
    Elf64_Ehdr *ehdr     = (Elf64_Ehdr *)kernel_file_address;
    Elf64_Shdr *shdr     = (Elf64_Shdr *)((char *)kernel_file_address + ehdr->e_shoff);
    const char *shstrtab = (const char *)kernel_file_address + shdr[ehdr->e_shstrndx].sh_offset;

    Elf64_Sym  *sym      = 0;
    const char *strtab   = 0;
    size_t      sym_size = 0;

    for (size_t i = 0; i < ehdr->e_shnum; ++i) {
        const char *sh_name = shstrtab + shdr[i].sh_name;
        if (!strcmp(sh_name, ".symtab")) {
            sym      = (Elf64_Sym *)((char *)kernel_file_address + shdr[i].sh_offset);
            sym_size = shdr[i].sh_size / sizeof(Elf64_Sym);
        } else if (!strcmp(sh_name, ".strtab")) {
            strtab = (const char *)kernel_file_address + shdr[i].sh_offset;
        }
    }
    if (!sym || !strtab) return sym_info;

    Elf64_Addr compare_addr;
    if (ehdr->e_type == 3) {
        if (kernel_address_request.response->virtual_base) {
            compare_addr = symbol_address - kernel_address_request.response->virtual_base;
        } else {
            compare_addr = symbol_address - KERNEL_BASE_ADDRESS;
        }
    } else {
        compare_addr = symbol_address;
    }

    for (size_t i = 0; i < sym_size; ++i) {
        unsigned char type = ELF64_ST_TYPE(sym[i].st_info);

        if (type != STT_FUNC) continue;
        Elf64_Addr  sym_start    = sym[i].st_value;
        Elf64_Xword sym_size_val = sym[i].st_size;

        if (compare_addr >= sym_start) {
            if (sym_size_val == 0) {
                if (compare_addr == sym_start) {
                    sym_info.name = strtab + sym[i].st_name;
                    sym_info.addr = sym_start;
                    sym_info.size = sym_size_val;
                    return sym_info;
                }
            } else {
                if (compare_addr < sym_start + sym_size_val) {
                    sym_info.name = strtab + sym[i].st_name;
                    sym_info.addr = sym_start;
                    sym_info.size = sym_size_val;
                    return sym_info;
                }
            }
        }
    }
    return sym_info;
}

/*
 * Return the runtime address just past the last kernel function.  Preferred
 * source is the end of the executable PT_LOAD segment from the ELF program
 * headers (survives a stripped symtab and tracks the real code layout); the
 * symbol table's highest STT_FUNC end is the fallback.  Only when no usable
 * ELF metadata exists is the fixed 64 MB window used.  Stack-scan bounds
 * therefore follow the actual code extent instead of a magic number, so valid
 * return addresses past the real text end are not missed and padding between
 * text end and any fallback window is not falsely reported.
 */
uintptr_t kernel_text_end(void)
{
    uintptr_t   base    = kernel_address_request.response && kernel_address_request.response->virtual_base ? (uintptr_t)kernel_address_request.response->virtual_base : KERNEL_BASE_ADDRESS;
    Elf64_Ehdr *ehdr    = kernel_file_request.response && kernel_file_request.response->kernel_file ? (Elf64_Ehdr *)kernel_file_request.response->kernel_file->address : NULL;
    Elf64_Addr  max_end = 0;

    if (!ehdr) return base + 0x4000000; // fallback: no kernel ELF available

    /* Preferred: end of the executable PT_LOAD segment(s). */
    {
        Elf64_Phdr *phdr = (Elf64_Phdr *)((char *)ehdr + ehdr->e_phoff);
        for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
            if (phdr[i].type != PT_LOAD || !(phdr[i].flags & PF_X)) continue;
            Elf64_Addr end = phdr[i].vaddr + phdr[i].memsz;
            if (end > max_end) max_end = end;
        }
    }
    if (max_end) return base + max_end;

    /* Fallback: the highest STT_FUNC symbol end from the symtab. */
    {
        Elf64_Shdr *shdr     = (Elf64_Shdr *)((char *)ehdr + ehdr->e_shoff);
        const char *shstrtab = (const char *)ehdr + shdr[ehdr->e_shstrndx].sh_offset;
        Elf64_Sym  *sym      = 0;
        size_t      sym_size = 0;
        for (Elf64_Half i = 0; i < ehdr->e_shnum; ++i) {
            const char *sh_name = shstrtab + shdr[i].sh_name;
            if (!strcmp(sh_name, ".symtab")) {
                sym      = (Elf64_Sym *)((char *)ehdr + shdr[i].sh_offset);
                sym_size = shdr[i].sh_size / sizeof(Elf64_Sym);
            }
        }
        if (sym) {
            for (size_t i = 0; i < sym_size; ++i) {
                if (ELF64_ST_TYPE(sym[i].st_info) != STT_FUNC) continue;
                Elf64_Addr end = sym[i].st_value + sym[i].st_size;
                if (end > max_end) max_end = end;
            }
        }
    }
    if (max_end) return base + max_end;

    return base + 0x4000000; // last resort: no usable ELF metadata
}
