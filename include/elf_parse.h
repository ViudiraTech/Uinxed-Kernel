/*
 *
 *      elf_parse.h
 *      ELF Parse Header Files
 *
 *      2025/5/4 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_ELF_PARSE_H_
#define INCLUDE_ELF_PARSE_H_

#include "stddef.h"
#include "stdint.h"

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

/* Parse ELF */
struct ElfParseResult parse_elf(const uint8_t *elf_data, size_t elf_size,
                                void (*mapping_callback)(struct ElfSegment segment));

#endif // INCLUDE_ELF_PARSE_H_
