/*
 *
 *      uinxed.h
 *      Kernel description header file
 *
 *      2024/7/23 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_UINXED_H_
#define INCLUDE_UINXED_H_

/*
 * Version format: major.minor.patch[-alpha.N|-beta.N|-rc.N]
 *
 * Carry rule: incrementing a higher component resets all lower components to 0.
 *   Major++ -> Minor = 0, Patch = 0
 *   Minor++ -> Patch = 0
 *
 * Major: Increments on breaking changes (API incompatible, architectural refactoring), major updates, or historic releases.
 * Minor: Increments on new features or significant improvements.
 * Patch: Increments on bug fixes, performance optimizations, or security patches.
 *
 * Prerelease identifiers (optional):
 *   alpha.N  Initial development: features unstable, updates aggressive, APIs may change freely.
 *   beta.N   Stabilization: bug fixes, refinements, performance tuning; feature set frozen.
 *   rc.N     Release candidate: final polish before release; only critical fixes accepted.
 *   (none)   Final release.
 *
 * Rules:
 *   Alpha/Beta -> No tag, no Release (may exist on master as regular commits).
 *   RC         -> Tag + Pre-release for testing.
 *   Final      -> Tag + Release, promoted from the last verified RC, keep forever.
 */

#define BUILD_DATE     __DATE__
#define BUILD_TIME     __TIME__
#define KERNEL_NAME    "Uinxed"
#define KERNEL_VERSION "1.0.0-alpha.1"

/* Compiler judgment */

#ifdef __clang__
#    define COMPILER_NAME    "clang"
#    define STRINGIFY(x)     #x
#    define EXPAND(x)        STRINGIFY(x)
#    define COMPILER_VERSION EXPAND(__clang_major__.__clang_minor__.__clang_patchlevel__)
#elif defined(__GNUC__)
#    define COMPILER_NAME    "gcc"
#    define STRINGIFY(x)     #x
#    define EXPAND(x)        STRINGIFY(x)
#    define COMPILER_VERSION EXPAND(__GNUC__.__GNUC_MINOR__.__GNUC_PATCHLEVEL__)
#else
#    error "Unknown compiler"
#endif

#define KERNEL_BASE_ADDRESS 0xffffffff80000000

extern volatile struct limine_rsdp_request           rsdp_request;
extern volatile struct limine_kernel_file_request    kernel_file_request;
extern volatile struct limine_smp_request            smp_request;
extern volatile struct limine_framebuffer_request    framebuffer_request;
extern volatile struct limine_smbios_request         smbios_request;
extern volatile struct limine_memmap_request         memmap_request;
extern volatile struct limine_hhdm_request           hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_entry_point_request    entry_point_request;
extern volatile struct limine_module_request         module_request;

/* Executable entry */
void executable_entry(void);

/* Kernel entry */
void kernel_entry(void);

#endif // INCLUDE_UINXED_H_
