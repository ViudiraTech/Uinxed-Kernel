/*
 *
 *      amd.h
 *      AMD processor microcode update header file
 *
 *      2026/10/5 MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_AMD_H_
#define INCLUDE_AMD_H_

#include <arch/cpu/microcode/microcode.h>
#include <libs/std/stdint.h>

/* A container starts with this magic, the ASCII "AMD\0". */
#define AMD_MC_MAGIC 0x00414d44

/* Section type of the equivalence table and of the updates themselves. */
#define AMD_MC_EQUIV_CPU_TABLE_TYPE 0x00000000
#define AMD_MC_MICROCODE_TYPE       0x00000001

/* Container and section header sizes, in bytes. */
#define AMD_MC_CONTAINER_HDR_SIZE 12
#define AMD_MC_SECTION_HDR_SIZE   8

/* Family specific limits on a patch payload, from AMD's revision guide. */
#define AMD_MC_F1XH_MPB_MAX_SIZE 2048
#define AMD_MC_F14H_MPB_MAX_SIZE 1824

/* Model specific registers of the update algorithm. */
#define AMD_MSR_PATCH_LOADER 0xC0010020 // takes the linear address of the patch
#define AMD_MSR_PATCH_LEVEL  0xC0010021 // revision currently installed

/* One equivalence table entry: an installed processor and its internal id. */
struct amd_equiv_cpu_entry {
        uint32_t installed_cpu; // CPUID.1:EAX of a shipped processor
        uint32_t fixed_errata_mask;
        uint32_t fixed_errata_compare;
        uint16_t equiv_cpu; // compared against processor_rev_id
        uint16_t reserved;
} __attribute__((packed));

/* Software header of one update section. */
struct amd_microcode_header {
        uint32_t data_code;
        uint32_t patch_id; // revision this patch installs
        uint16_t mc_patch_data_id;
        uint8_t  mc_patch_data_len;
        uint8_t  init_flag;
        uint32_t mc_patch_data_checksum;
        uint32_t nb_dev_id;
        uint32_t sb_dev_id;
        uint16_t processor_rev_id; // equivalence id of the target processor
        uint8_t  nb_rev_id;
        uint8_t  sb_rev_id;
        uint8_t  bios_api_rev;
        uint8_t  reserved1[3];
        uint32_t match_reg[8];
} __attribute__((packed));

_Static_assert(sizeof(struct amd_equiv_cpu_entry) == 16, "AMD equivalence table entry size");
_Static_assert(sizeof(struct amd_microcode_header) == 64, "AMD microcode header size");

/* SHA-256 digest of one signed AMD patch, checked before the update is applied. */
typedef struct amd_patch_digest {
        uint32_t patch_id;
        uint8_t  sha256[32];
} amd_patch_digest_t;

/* Digests of every signed patch, sorted by patch id. */
extern const amd_patch_digest_t amd_patch_digests[];
extern const size_t             amd_patch_digest_count;

/* AMD backend registered with the generic microcode dispatcher. */
extern const microcode_ops_t amd_microcode_ops;

#endif // INCLUDE_AMD_H_
