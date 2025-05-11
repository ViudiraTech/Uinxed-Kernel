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
#include "limine.h"
#include "printk.h"
#include "stdarg.h"
#include "symbols.h"
#include "uinxed.h"

/* Dump stack */
void dump_stack(void)
{
    union RbpNode {
            uintptr_t inner;
            union RbpNode *next;
    } *rbp; // A way to avoid performance-no-int-to-ptr
    uintptr_t rip;
    __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
    __asm__ volatile("leaq (%%rip), %0" : "=r"(rip));

    plogk("Call Trace:\n");
    plogk(" <TASK>\n");

    for (int i = 0; i < 16 && rip && (uintptr_t)rbp > 0x1000; ++i) {
        sym_info_t sym_info = get_symbol_info(kernel_file_request.response->kernel_file->address, rip);
        if (!sym_info.name) {
            plogk("  [<0x%016zx>] %s\n", rip, "unknown");
        } else {
            plogk("  [<0x%016zx>] `%s`+0x%lx\n", rip, sym_info.name, rip - sym_info.addr);
        }
        rip = *(uintptr_t *)(rbp + 1);
        rbp = rbp->next;
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
