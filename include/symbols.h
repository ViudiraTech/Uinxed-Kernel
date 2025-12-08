/*
 *
 *      symbols.h
 *      Symbol table header file
 *
 *      2025/5/4 By suhuajun
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYMBOLS_H_
#define INCLUDE_SYMBOLS_H_

#include <elf.h>
#include <stdint.h>

typedef struct {
        const char *name;
        Elf64_Addr  addr;
        Elf64_Xword size;
} sym_info_t;

/* Get symbol information */
sym_info_t get_symbol_info(uint64_t *kernel_file_address, Elf64_Addr symbol_address);

#endif // INCLUDE_SYMBOLS_H_
