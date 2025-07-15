/*
 *
 *      mmu.h
 *      Memory Management Unit Header File
 *
 *      2025/7/15 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_MMU_H_
#define INCLUDE_MMU_H_

#define MSR_IA32_PAT 0x277

/* Get the PAT configuration string */
const char *get_pat_config(void);

#endif // INCLUDE_MMU_H_
