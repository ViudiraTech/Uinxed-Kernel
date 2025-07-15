/*
 *
 *      mmu.c
 *      Memory Management Unit
 *
 *      2025/7/15 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "mmu.h"
#include "common.h"
#include "printk.h"
#include "stdint.h"

const char *pat_types[8] = {"WB ", "WC ", "UC-", "UC ", "WB ", "WP ", "UC-", "WT "};

/* Get the PAT configuration string */
const char *get_pat_config(void)
{
    static char pat_str[64];
    int pos = 0;

    for (int i = 0; i < 8; i++) {
        uint8_t entry = (rdmsr(MSR_IA32_PAT) >> (i * 8)) & 0xFF;
        uint8_t type  = entry & 0x7;
        if (type > 7) type = 0;
        pos += sprintf(pat_str + pos, "%s ", pat_types[type]);
    }
    if (pos > 0) pat_str[pos - 1] = '\0';
    return pat_str;
}
