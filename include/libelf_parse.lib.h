/*
 *
 *		libelf_parse.lib.h
 *		libelf_parse库头文件
 *
 *		2024/12/5 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */ 

#ifndef INCLUDE_LIBELF_PARSE_LIB_H_
#define INCLUDE_LIBELF_PARSE_LIB_H_

typedef enum ElfParseResult_Tag {
	EntryPoint,
	InvalidElfData,
	ElfContainsNoSegments,
	FailedToGetSegmentData,
	AllocFunctionNotProvided,
} ElfParseResult_Tag;

typedef struct ElfParseResult {
	ElfParseResult_Tag tag;
	union {
		struct {
			size_t entry_point;
		};
	};
} ElfParseResult;

typedef struct ElfSegment {
	size_t address;
	size_t size;
	const uint8_t *data;
} ElfSegment;

struct ElfParseResult parse_elf(const uint8_t *elf_data, size_t elf_size,
                                void (*mapping_callback)(struct ElfSegment segment));

#endif // INCLUDE_LIBELF_PARSE_LIB_H_
