/*
 *
 *      module_elf.h
 *      ELF helpers for loadable kernel modules
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MODULE_ELF_H_
#define INCLUDE_MODULE_ELF_H_

#include <kernel/module/elf.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

typedef struct module_elf_view {
        const uint8_t    *image;
        size_t            size;
        const Elf64_Ehdr *header;
        const Elf64_Shdr *sections;
        size_t            section_count;
        size_t            section_name_index;
} module_elf_view_t;

/* Validate an x86-64 ET_REL image and all of its section-table ranges. */
int module_elf_validate(const void *image, size_t size, module_elf_view_t *view);

/* Apply one x86-64 relocation, including the ABI-mandated overflow checks. */
int module_elf_apply_relocation(uint32_t type, void *location, uint64_t symbol_value, int64_t addend, uintptr_t place);

#endif // INCLUDE_MODULE_ELF_H_
