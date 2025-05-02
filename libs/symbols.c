/*
 *
 *      symbols.c
 *      Symbol Table
 *
 *      2025/5/2 By W9pi3cZ1
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "symbols.h"
#include "string.h"

/* Find the corresponding symbol index in the symbol table according to the address */
long long symbol_idx_lookup(size_t addr)
{
    for (size_t i = 0; i < sym_count; i++) {
        if (addresses[i] > addr) return i - 1;
    }
    return -1;
}

/* Find the corresponding symbol index in the symbol table according to the symbol name */
long long symbol_addr_idx_lookup(char *sym_name)
{
    for (size_t i = 0; i < sym_count; i++) {
        if (strcmp(symbols[i], sym_name) == 0) return i;
    }
    return -1;
}
