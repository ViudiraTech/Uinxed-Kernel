/*
 *
 *      microcode.h
 *      Processor microcode update core header file
 *
 *      2026/10/5 Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MICROCODE_H_
#define INCLUDE_MICROCODE_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Outcome of one microcode operation. */
typedef enum {
    MICROCODE_OK = 0,  // the CPU already runs this or a newer revision
    MICROCODE_UPDATED, // the update was applied to this CPU
    MICROCODE_NFOUND,  // the blob holds no update for this CPU
    MICROCODE_ERROR,   // the blob or the update itself was rejected
} microcode_state_t;

/* Identity of one processor, as the matching rules see it. */
typedef struct cpu_signature {
        uint32_t sig; // CPUID.1:EAX, i.e. family/model/stepping
        uint32_t pf;  // Intel platform flag; zero on other vendors
        uint32_t rev; // microcode revision currently loaded
} cpu_signature_t;

/* One microcode image handed over by the bootloader. */
typedef struct microcode_blob {
        const char    *name; // module the image came from, for the log line
        const uint8_t *data;
        size_t         size;
} microcode_blob_t;

/* One CPU vendor's backend: it finds, validates and applies that vendor's updates. */
typedef struct microcode_ops {
        const char *name; // backend name used in log messages, e.g. "Intel"

        /* Name of the blob for this exact processor, e.g. "06-5e-03".  Tried before the wide names. */
        const char *(*builtin_name)(void);

        /* Names the vendor wide blob goes by, tried in the order listed. */
        const char *const *module_names;
        size_t             module_name_count;

        int (*match)(void);                                      // nonzero when this backend drives the running CPU
        int (*probe)(const uint8_t *data, size_t size);          // does this buffer look like this vendor's blob?
        microcode_state_t (*load)(const microcode_blob_t *blob); // update the boot processor
        microcode_state_t (*load_ap)(void);                      // update the calling application processor
        uint32_t (*revision)(void);                              // revision the calling CPU runs
} microcode_ops_t;

/* Update the boot processor; a missing module or a current CPU is normal. */
void init_microcode(void);

/* Update the calling application processor; needs the kernel page tables active. */
void init_microcode_ap(void);

/* Microcode revision processor @cpu runs, as /proc/cpuinfo reports it; the boot processor's when that CPU never reported one. */
uint32_t microcode_revision(uint32_t cpu);

#endif // INCLUDE_MICROCODE_H_
