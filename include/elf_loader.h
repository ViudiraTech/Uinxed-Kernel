/*
 *
 *      elf_loader.h
 *      ELF executable file loader.
 *
 *      2026/7/21 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ELF_LOADER_H_
#define INCLUDE_ELF_LOADER_H_

#include <stddef.h>
#include <stdint.h>

struct process;

typedef struct {
        uint8_t  ident[16];
        uint16_t type;
        uint16_t machine;
        uint32_t version;
        uint64_t entry;
        uint64_t phoff;
        uint64_t shoff;
        uint32_t flags;
        uint16_t ehsize;
        uint16_t phentsize;
        uint16_t phnum;
        uint16_t shentsize;
        uint16_t shnum;
        uint16_t shstrndx;
} elf_loader_ehdr_t;

typedef struct {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t vaddr;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t align;
} elf_loader_phdr_t;

elf_loader_ehdr_t *elf_loader_validate(const uint8_t *data, size_t size);

int elf_loader_load_segments(struct process *proc, const elf_loader_ehdr_t *ehdr, const uint8_t *data);

uintptr_t elf_loader_setup_user_stack(struct process *proc, const elf_loader_ehdr_t *ehdr);

void *elf_loader_user_ptr(struct process *proc, uintptr_t addr);

#endif /* INCLUDE_ELF_LOADER_H_ */
