/*
 *
 *		elf.c
 *		处理ELF格式
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "common.h"
#include "string.h"
#include "elf.h"
#include "printk.h"

/* 回调函数 */
void segment_callback(struct ElfSegment segment)
{
	page_directory_t *pg_dir = current_directory;
	for (size_t i = segment.address; i < segment.address + segment.size; i += 4096) {
		alloc_frame(get_page(i, 1, pg_dir), 0, 1);
	}
	memcpy((void*)segment.address,segment.data,segment.size);
}

/* ELF加载并返回入口 */
uint32_t elf_load(size_t elf_size, uint8_t *elf_data)
{
	struct ElfParseResult result = parse_elf(elf_data, elf_size, segment_callback);
	switch (result.tag) {
		case EntryPoint:
			return result.entry_point;
		case InvalidElfData:
			break;
		case ElfContainsNoSegments:
			break;
		case FailedToGetSegmentData:
			break;
		case AllocFunctionNotProvided:
			break;
		default:
			break;
	}
	return 0;
}

/* 从 multiboot_t 结构获取ELF信息 */
elf_t elf_from_multiboot(multiboot_elf_section_header_table_t *mb)
{
	int i;
	elf_t elf;
	elf_section_header_t *sh = (elf_section_header_t *)mb->addr;

	uint32_t shstrtab = sh[mb->shndx].addr;
	for (i = 0; i < (int)mb->num; i++) {
		const char *name = (const char *)(shstrtab + sh[i].name);

		/* 在 GRUB 提供的 multiboot 信息中寻找 */
		/* 内核 ELF 格式所提取的字符串表和符号表 */
		if (strcmp(name, ".strtab") == 0) {
			elf.strtab = (const char *)sh[i].addr;
			elf.strtabsz = sh[i].size;
		}
		if (strcmp(name, ".symtab") == 0) {
			elf.symtab = (elf_symbol_t*)sh[i].addr;
			elf.symtabsz = sh[i].size;
		}
	}
	return elf;
}

/* 查看ELF的符号信息 */
const char *elf_lookup_symbol(uint32_t addr, elf_t *elf)
{
	for (uint32_t i = 0; i < (elf->symtabsz / sizeof(elf_symbol_t)); i++) {
		if (ELF32_ST_TYPE(elf->symtab[i].info) != 0x2) {
			continue;
		}

		/* 通过函数调用地址查到函数的名字 */
		if ( (addr >= elf->symtab[i].value) && (addr < (elf->symtab[i].value + elf->symtab[i].size)) ) {
			return (const char *)((uint32_t)elf->strtab + elf->symtab[i].name);
		}
	}
	return 0;
}
