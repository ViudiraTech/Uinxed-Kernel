/*
 *
 *      vdso.h
 *      Kernel/userspace shared vDSO data layout
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VDSO_H_
#define INCLUDE_VDSO_H_

#include <libs/std/stdint.h>

/*
 * The image is one loadable segment: the ELF header at virtual address 0, the
 * code behind it, and the clock data page page-aligned after that.  This is the
 * address of that data page, and the linker script is asserted to place it
 * exactly here -- the kernel maps its own shared page over the address.
 */
#define VDSO_DATA_OFFSET 0x1000

/* Interpolate from the cycle counter only in CYCLES mode, else use the snapshot. */
#define VDSO_CLOCKMODE_NONE   0
#define VDSO_CLOCKMODE_CYCLES 1

/*
 * Time data published read-only to userspace.  Writers bracket each update with
 * the seqlock (seq goes odd, data is stored, seq goes even) and readers retry
 * while seq is odd or changed, so the read path needs no lock.
 */
struct vdso_data {
        uint32_t seq;
        int32_t  clock_mode;
        uint64_t cycle_last; // cycle counter at the snapshot below
        uint32_t mult;       // ns = (delta cycles * mult) >> shift
        uint32_t shift;
        uint64_t max_cycles; // largest counter advance worth interpolating over
        uint64_t real_sec;   // CLOCK_REALTIME at cycle_last
        uint64_t real_nsec;
        uint64_t mono_sec;   // CLOCK_MONOTONIC at cycle_last
        uint64_t mono_nsec;
        uint64_t boot_sec;   // CLOCK_BOOTTIME at cycle_last
        uint64_t boot_nsec;
        uint64_t res_nsec;   // clock resolution reported by the kernel
};

struct process;

/* Allocate the shared pages and locate the image inside the embedded ELF. */
void vdso_init(void);

/* Timer-tick hook: refresh the published clock snapshot. */
void vdso_tick(void);

/* Map the image and data page into a process; returns the base for AT_SYSINFO_EHDR. */
uintptr_t vdso_map_process(struct process *proc);

#endif // INCLUDE_VDSO_H_
