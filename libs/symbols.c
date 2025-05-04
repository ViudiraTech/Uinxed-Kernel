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
#include "string.h"

/* Get symbol information */
sym_info_t get_symbol_info(void *kernel_file_address, Elf64_Addr symbol_address)
{
    sym_info_t sym_info  = {};
    Elf64_Ehdr *ehdr     = kernel_file_address;
    Elf64_Shdr *shdr     = ehdr->e_shoff + kernel_file_address;
    const char *shstrtab = (const char *)(shdr[ehdr->e_shstrndx].sh_offset + kernel_file_address);
    Elf64_Sym *sym       = 0;
    const char *strtab   = 0;
    size_t sym_size      = 0;
    for (size_t i = 0; i < ehdr->e_shnum; ++i) {
        const char *sh_name = &shstrtab[shdr[i].sh_name];
        if (!strcmp(sh_name, ".symtab")) {
            sym      = (Elf64_Sym *)(shdr[i].sh_offset + kernel_file_address);
            sym_size = shdr[i].sh_size / shdr[i].sh_entsize;
        }
        if (!strcmp(sh_name, ".strtab")) strtab = (const char *)(shdr[i].sh_offset + kernel_file_address);
    }
    if (!sym || !strtab) return sym_info;
    for (size_t i = 0; i < sym_size; ++i) {
        if (ELF64_ST_TYPE(sym[i].st_info) != STT_FUNC) continue;
        if (symbol_address - sym_info.addr <= symbol_address - sym[i].st_value) continue;
        const char *st_name = &strtab[sym[i].st_name];
        sym_info            = (sym_info_t) {st_name, sym[i].st_value};
    }
    if (symbol_address - sym_info.addr >= 0x1000) sym_info.name = 0;
    return sym_info;
}
