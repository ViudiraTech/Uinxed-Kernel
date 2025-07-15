/*
 *
 *      symbols.h
 *      Symbol Table Header File
 *
 *      2025/5/4 By suhuajun
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SYMBOLS_H_
#define INCLUDE_SYMBOLS_H_

#include "elf.h"
#include "stddef.h"

typedef struct {
        const char *name;
        Elf64_Addr addr;
        Elf64_Xword size;
} sym_info_t;

/* Get symbol information */
sym_info_t get_symbol_info(void *kernel_file_address, Elf64_Addr symbol_address);

#endif // INCLUDE_SYMBOLS_H_
