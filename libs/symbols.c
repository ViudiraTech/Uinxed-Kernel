#include "symbols.h"
#include "string.h"
long long symbol_idx_lookup(unsigned long addr)
{
    for (unsigned long i = 0; i < sym_count; i++) {
        if (addresses[i] > addr) { return i - 1; }
    }
    return -1;
}

long long symbol_addr_idx_lookup(char *sym_name)
{
    for (unsigned long i = 0; i < sym_count; i++) {
        if (strcmp(symbols[i],sym_name) == 0) { return i; }
    }
    return -1;
}