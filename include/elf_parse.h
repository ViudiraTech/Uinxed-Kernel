/*
 *
 *      elf_parse.h
 *      ELF parse header files
 *
 *      2025/5/4 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ELF_PARSE_H_
#define INCLUDE_ELF_PARSE_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
    entry_point,
    elf_invalid_data,
    elf_no_segments,
    elf_segment_error,
    elf_allocator_missing,
} elf_result_tag_t;

typedef struct {
        elf_result_tag_t tag;
        union {
                struct {
                        size_t entry_point;
                };
        };
} elf_result_t;

typedef struct {
        size_t         address;
        size_t         size;
        const uint8_t *data;
} elf_segment_t;

/* Parse ELF */
elf_result_t parse_elf(const uint8_t *elf_data, size_t elf_size, void (*mapping_callback)(elf_segment_t segment));

#endif // INCLUDE_ELF_PARSE_H_
