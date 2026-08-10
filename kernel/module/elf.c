/*
 *
 *      elf.c
 *      ELF validation and x86-64 relocation for loadable modules
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/module/elf.h>
#include <kernel/module/module_elf.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1
#define EM_X86_64     62
#define R_X86_64_PC64 24

static int range_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static int power_of_two(uint64_t value)
{
    return !value || !(value & (value - 1));
}

int module_elf_validate(const void *image, size_t size, module_elf_view_t *view)
{
    if (!image || !view || size < sizeof(Elf64_Ehdr)) {
        plogk("module: Invalid ELF image (image=%p, view=%p, size=%lu)\n", image, (void *)view, (unsigned long)size);
        return -ENOEXEC;
    }

    const Elf64_Ehdr *header = image;
    if (header->e_ident[0] != 0x7f || header->e_ident[1] != 'E' || header->e_ident[2] != 'L' || header->e_ident[3] != 'F'
        || header->e_ident[4] != ELFCLASS64 || header->e_ident[5] != ELFDATA2LSB || header->e_ident[6] != EV_CURRENT || header->e_type != ET_REL
        || header->e_machine != EM_X86_64 || header->e_version != EV_CURRENT || header->e_ehsize != sizeof(Elf64_Ehdr)
        || header->e_shentsize != sizeof(Elf64_Shdr)) {
        plogk("module: Malformed ELF header (type=%u, machine=%u, version=%u, ehsize=%u, shentsize=%u)\n", header->e_type, header->e_machine,
              header->e_version, header->e_ehsize, header->e_shentsize);
        return -ENOEXEC;
    }

    if (!header->e_shoff || !range_valid((size_t)header->e_shoff, sizeof(Elf64_Shdr), size)) {
        plogk("module: Invalid section header table (shoff=%lu, size=%lu)\n", (unsigned long)header->e_shoff, (unsigned long)size);
        return -ENOEXEC;
    }
    const Elf64_Shdr *sections = (const Elf64_Shdr *)((const uint8_t *)image + header->e_shoff);
    size_t            count    = header->e_shnum ? header->e_shnum : (size_t)sections[0].sh_size;
    size_t            names    = header->e_shstrndx == SHN_XINDEX ? sections[0].sh_link : header->e_shstrndx;
    if (!count || count > UINT16_MAX || !range_valid((size_t)header->e_shoff, count * sizeof(Elf64_Shdr), size)) {
        plogk("module: Invalid section count (%lu, size=%lu)\n", (unsigned long)count, (unsigned long)size);
        return -ENOEXEC;
    }
    if (names != SHN_UNDEF && names >= count) {
        plogk("module: Section name index %lu out of range (count=%lu)\n", (unsigned long)names, (unsigned long)count);
        return -ENOEXEC;
    }

    for (size_t index = 0; index < count; index++) {
        const Elf64_Shdr *section = &sections[index];
        if (!power_of_two(section->sh_addralign)) {
            plogk("module: Section %lu invalid addralign %lu\n", (unsigned long)index, (unsigned long)section->sh_addralign);
            return -ENOEXEC;
        }
        if (section->sh_type != SHT_NOBITS && !range_valid((size_t)section->sh_offset, (size_t)section->sh_size, size)) {
            plogk("module: Section %lu data out of range (type=%u, offset=%lu, size=%lu)\n", (unsigned long)index, section->sh_type,
                  (unsigned long)section->sh_offset, (unsigned long)section->sh_size);
            return -ENOEXEC;
        }
        if ((section->sh_type == SHT_SYMTAB || section->sh_type == SHT_DYNSYM)
            && (section->sh_entsize != sizeof(Elf64_Sym) || section->sh_link >= count || section->sh_size % sizeof(Elf64_Sym))) {
            plogk("module: Invalid symbol table section %lu (entsize=%lu, link=%lu, size=%lu)\n", (unsigned long)index,
                  (unsigned long)section->sh_entsize, (unsigned long)section->sh_link, (unsigned long)section->sh_size);
            return -ENOEXEC;
        }
        if (section->sh_type == SHT_RELA
            && (section->sh_entsize != sizeof(Elf64_Rela) || section->sh_link >= count || section->sh_info >= count
                || section->sh_size % sizeof(Elf64_Rela))) {
            plogk("module: Invalid RELA section %lu (entsize=%lu, link=%lu, info=%lu, size=%lu)\n", (unsigned long)index,
                  (unsigned long)section->sh_entsize, (unsigned long)section->sh_link, (unsigned long)section->sh_info,
                  (unsigned long)section->sh_size);
            return -ENOEXEC;
        }
        if (section->sh_type == SHT_REL) {
            plogk("module: Unsupported SHT_REL section %lu\n", (unsigned long)index);
            return -ENOEXEC;
        }
    }
    if (names != SHN_UNDEF && sections[names].sh_type != SHT_STRTAB) {
        plogk("module: Section name table %lu is not a string table (type=%u)\n", (unsigned long)names, sections[names].sh_type);
        return -ENOEXEC;
    }

    view->image              = image;
    view->size               = size;
    view->header             = header;
    view->sections           = sections;
    view->section_count      = count;
    view->section_name_index = names;
    return EOK;
}

static int store_signed(void *location, __int128 value, unsigned int bits)
{
    __int128 minimum = -((__int128)1 << (bits - 1));
    __int128 maximum = ((__int128)1 << (bits - 1)) - 1;
    if (value < minimum || value > maximum) {
        plogk("module: Relocation value out of range (bits=%u, place=%p)\n", bits, (void *)location);
        return -ENOEXEC;
    }
    if (bits == 8)
        *(int8_t *)location = (int8_t)value;
    else if (bits == 16)
        *(int16_t *)location = (int16_t)value;
    else if (bits == 32)
        *(int32_t *)location = (int32_t)value;
    else
        *(int64_t *)location = (int64_t)value;
    return EOK;
}

static int store_unsigned(void *location, __int128 value, unsigned int bits)
{
    __int128 maximum = ((__int128)1 << bits) - 1;
    if (value < 0 || value > maximum) {
        plogk("module: Unsigned relocation value out of range (bits=%u, place=%p)\n", bits, (void *)location);
        return -ENOEXEC;
    }
    if (bits == 8)
        *(uint8_t *)location = (uint8_t)value;
    else if (bits == 16)
        *(uint16_t *)location = (uint16_t)value;
    else
        *(uint32_t *)location = (uint32_t)value;
    return EOK;
}

int module_elf_apply_relocation(uint32_t type, void *location, uint64_t symbol_value, int64_t addend, uintptr_t place)
{
    if (!location) {
        plogk("module: Relocation with null location (type=%u, place=%p)\n", type, (void *)place);
        return -EINVAL;
    }
    __int128 absolute = (__int128)symbol_value + (__int128)addend;
    __int128 relative = absolute - (__int128)place;

    switch (type) {
        case R_X86_64_NONE :
            return EOK;
        case R_X86_64_64 :
            *(uint64_t *)location = symbol_value + (uint64_t)addend;
            return EOK;
        case R_X86_64_PC32 :
        case R_X86_64_PLT32 :
            return store_signed(location, relative, 32);
        case R_X86_64_32 :
            return store_unsigned(location, absolute, 32);
        case R_X86_64_32S :
            return store_signed(location, absolute, 32);
        case R_X86_64_16 :
            return store_unsigned(location, absolute, 16);
        case R_X86_64_PC16 :
            return store_signed(location, relative, 16);
        case R_X86_64_8 :
            return store_unsigned(location, absolute, 8);
        case R_X86_64_PC8 :
            return store_signed(location, relative, 8);
        case R_X86_64_PC64 :
            return store_signed(location, relative, 64);
        case R_X86_64_SIZE32 :
            return store_unsigned(location, absolute, 32);
        case R_X86_64_SIZE64 :
            *(uint64_t *)location = symbol_value + (uint64_t)addend;
            return EOK;
        default :
            plogk("module: Unsupported relocation type %u (place=%p)\n", type, (void *)place);
            return -ENOEXEC;
    }
}
