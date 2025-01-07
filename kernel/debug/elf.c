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
	for (size_t i = segment.address; i < segment.address + segment.size; i += 4096) {
		page_alloc(i, PT_W|PT_U);
	}
	memcpy((void*)segment.address,segment.data,segment.size);
}

/* ELF加载并返回入口 */
uint32_t elf_load(size_t elf_size,uint8_t *elf_data)
{
	struct ElfParseResult result = parse_elf(elf_data, elf_size, segment_callback);
	switch (result.tag) {
		case EntryPoint:
			return result.entry_point;
		case InvalidElfData:
			printk("Invalid ELF data.\n");
			break;
		case ElfContainsNoSegments:
			printk("ELF contains no segments.\n");
			break;
		case FailedToGetSegmentData:
			printk("Failed to get segment data.\n");
			break;
		case AllocFunctionNotProvided:
			printk("Allocation function not provided.\n");
			break;
		default:
			printk("Unknown error.\n");
			break;
	}
	return 0;
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
