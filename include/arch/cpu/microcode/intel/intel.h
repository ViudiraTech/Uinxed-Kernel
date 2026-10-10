/*
 *
 *      intel.h
 *      Intel processor microcode update header file
 *
 *      2026/10/5 Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_INTEL_H_
#define INCLUDE_INTEL_H_

#include <arch/cpu/microcode/microcode.h>
#include <libs/std/stdint.h>

/* Record layout, from the "microcode update" chapter of the Intel SDM. */
#define INTEL_MC_HEADER_SIZE       48
#define INTEL_MC_EXT_HEADER_SIZE   20
#define INTEL_MC_EXT_SIG_SIZE      12
#define INTEL_MC_DEFAULT_DATASIZE  2000
#define INTEL_MC_DEFAULT_TOTALSIZE (INTEL_MC_DEFAULT_DATASIZE + INTEL_MC_HEADER_SIZE)

/* hdrver of an update record; in-field scan records use another type. */
#define INTEL_MC_HEADER_TYPE 1

/* ldrver this loader implements. */
#define INTEL_MC_LOADER_VERSION 1

/* Model specific registers of the update algorithm. */
#define INTEL_MSR_PLATFORM_ID     0x17 // bits 52:50 select the platform flag bit
#define INTEL_MSR_MICROCODE_WRITE 0x79 // IA32_BIOS_UPDT_TRIG, linear address of the payload
#define INTEL_MSR_MICROCODE_REV   0x8B // IA32_BIOS_SIGN_ID, revision in the upper half

/* Software header that precedes every payload. */
struct intel_microcode_header {
        uint32_t hdrver;      // header format, INTEL_MC_HEADER_TYPE for updates
        uint32_t rev;         // update revision, a.k.a. the patch level
        uint32_t date;        // packed, in BCD, as year:16 | month:8 | day:8
        uint32_t sig;         // processor signature, CPUID.1:EAX
        uint32_t cksum;       // header plus payload sum to zero
        uint32_t ldrver;      // loader revision, INTEL_MC_LOADER_VERSION
        uint32_t pf;          // platform mask; an empty mask matches every platform
        uint32_t datasize;    // payload size; zero means the legacy 2000 bytes
        uint32_t totalsize;   // header, payload and table; zero means the legacy size
        uint32_t min_req_ver; // minimum revision a late load may start from; unused here
        uint32_t reserved[2];
};

/* One extra processor signature sharing the same payload. */
struct intel_microcode_extended_sig {
        uint32_t sig;
        uint32_t pf;
        uint32_t cksum; // sig plus pf plus cksum equals the one in the record header
};

/* Optional table of extra signatures, placed right after the payload. */
struct intel_microcode_extended_table {
        uint32_t                            count;
        uint32_t                            cksum; // every dword of the table sums to zero
        uint32_t                            reserved[3];
        struct intel_microcode_extended_sig sigs[];
};

_Static_assert(sizeof(struct intel_microcode_header) == INTEL_MC_HEADER_SIZE, "Intel microcode header size");
_Static_assert(sizeof(struct intel_microcode_extended_sig) == INTEL_MC_EXT_SIG_SIZE, "Intel extended signature size");
_Static_assert(sizeof(struct intel_microcode_extended_table) == INTEL_MC_EXT_HEADER_SIZE, "Intel extended table header size");

/* Intel backend registered with the generic microcode dispatcher. */
extern const microcode_ops_t intel_microcode_ops;

#endif // INCLUDE_INTEL_H_
