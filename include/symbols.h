#ifndef SYMBOLS_H
#define SYMBOLS_H
extern const unsigned long sym_count __attribute__((weak));
extern const char *const symbols[] __attribute__((weak));
extern const unsigned long addresses[] __attribute__((weak));

long long symbol_idx_lookup(unsigned long addr);
long long symbol_addr_idx_lookup(char *sym_name);
#endif // SYMBOLS_H