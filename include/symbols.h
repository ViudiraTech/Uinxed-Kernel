/*
 *
 *      symbols.h
 *      Symbol Table Header File
 *
 *      2025/5/2 By W9pi3cZ1
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SYMBOLS_H_
#define INCLUDE_SYMBOLS_H_

#include "stddef.h"

extern const size_t sym_count __attribute__((weak));
extern const char *const symbols[] __attribute__((weak));
extern const size_t addresses[] __attribute__((weak));

/* Find the corresponding symbol index in the symbol table according to the address */
long long symbol_idx_lookup(size_t addr);

/* Find the corresponding symbol index in the symbol table according to the symbol name */
long long symbol_addr_idx_lookup(char *sym_name);

#endif // INCLUDE_SYMBOLS_H_
