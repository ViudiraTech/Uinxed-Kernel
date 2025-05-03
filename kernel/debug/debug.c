/*
 *
 *      debug.c
 *      Kernel debug
 *
 *      2024/6/27 By Rainy101112
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "debug.h"
#include "common.h"
#include "printk.h"
#include "symbols.h"
#include "vargs.h"

/* Dump stack */
void dump_stack(void)
{
    uintptr_t *rbp;
    __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
    uintptr_t rip;
    __asm__ volatile("leaq (%%rip), %0" : "=r"(rip));
    long long sym_idx = 0;

    plogk("Call Trace:\n");
    plogk(" <TASK>\n");

    for (int i = 0; i < 16 && rip && (uintptr_t)rbp > 0x1000; ++i) {
        sym_idx = symbol_idx_lookup((size_t)(rip));
        if (sym_idx < 0) {
            plogk("  [<0x%016zx>] %s\n", rip, "unknown");
        } else {
            plogk("  [<0x%016zx>] `%s`+0x%llx/0x%llx\n", rip, symbols[sym_idx], rip - addresses[sym_idx],
                  addresses[sym_idx + 1] - addresses[sym_idx] - 1);
        }
        rip = *(rbp + 1);
        rbp = (uintptr_t *)(*rbp);
    }
    plogk(" </TASK>\n");
}

/* Kernel panic */
void panic(const char *format, ...)
{
    static char buff[1024];
    va_list args;
    int i;

    va_start(args, format);
    i = vsprintf(buff, format, args);
    va_end(args);

    buff[i] = '\0';

    plogk("\n");
    plogk("Kernel panic - not syncing: %s\n", buff);
    dump_stack();
    plogk("---[ end Kernel panic - not syncing: %s ]---", buff);
    krn_halt();
}

/* Assertion failure */
void assertion_failure(const char *exp, const char *file, int line)
{
    printk("assert(%s) failed!\nfile: %s\nline: %d\n\n", exp, file, line);
    krn_halt();
}
