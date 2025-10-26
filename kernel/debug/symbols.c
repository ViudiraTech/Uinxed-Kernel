/*
 *
 *      symbols.c
 *      Symbol Table
 *
 *      2025/5/4 By suhuajun
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "symbols.h"
#include "limine.h"
#include "string.h"
#include "uinxed.h"

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

    Elf64_Addr relative_addr;
    if (kernel_address_request.response->virtual_base) {
        relative_addr = symbol_address - kernel_address_request.response->virtual_base;
    } else {
        relative_addr = symbol_address - KERNEL_BASE_ADDRESS;
    }

    for (size_t i = 0; i < sym_size; ++i) {
        unsigned char type = ELF64_ST_TYPE(sym[i].st_info);

        if (type != STT_FUNC) continue;
        Elf64_Addr  sym_start    = sym[i].st_value;
        Elf64_Xword sym_size_val = sym[i].st_size;

        if (relative_addr >= sym_start && (!sym_size_val ? relative_addr == sym_start : relative_addr < sym_start + sym_size_val)) {
            sym_info.name = strtab + sym[i].st_name;
            sym_info.addr = sym_start;
            sym_info.size = sym_size_val;
            return sym_info;
        }
    }
    return sym_info;
}
