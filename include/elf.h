/*
 *
 *		elf.h
 *		处理ELF格式头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_ELF_H_
#define INCLUDE_ELF_H_

#include "types.h"
#include "multiboot.h"
#include "libelf_parse.lib.h"
#include "memory.h"

#define ELF32_ST_TYPE(i) ((i)&0xf)

/* ELF 格式区段头 */
typedef
struct elf_section_header_t {
	uint32_t name;
	uint32_t type;
	uint32_t flags;
	uint32_t addr;
	uint32_t offset;
	uint32_t size;
	uint32_t link;
	uint32_t info;
	uint32_t addralign;
	uint32_t entsize;
} __attribute__((packed)) elf_section_header_t;

/* ELF 格式符号 */
typedef
struct elf_symbol_t {
	uint32_t name;
	uint32_t value;
	uint32_t size;
	uint8_t  info;
	uint8_t  other;
	uint16_t shndx;
} __attribute__((packed)) elf_symbol_t;

/* ELF 信息 */
typedef
struct elf_t {
	elf_symbol_t	*symtab;
	uint32_t		symtabsz;
	const char		*strtab;
	uint32_t		strtabsz;
} elf_t;

/* 回调函数 */
void segment_callback(struct ElfSegment segment);

/* ELF加载并返回入口 */
uint32_t elf_load(size_t elf_size,uint8_t *elf_data);

/* 查看ELF的符号信息 */
const char *elf_lookup_symbol(uint32_t addr, elf_t *elf);

#endif // INCLUDE_ELF_H_
