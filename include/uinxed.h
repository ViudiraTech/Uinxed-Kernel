/*
 *
 *      uinxed.h
 *      Kernel description header file
 *
 *      2024/7/23 By Rainy101112
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_UINXED_H_
#define INCLUDE_UINXED_H_

#define BUILD_DATE __DATE__     // Compilation Date
#define BUILD_TIME __TIME__     // Compile time
#define KERNL_VERS "0.0.250504" // Version format: v[major version].[patch version].[YY-MM-DD]

/* Compiler judgment */
#if defined(__clang__)
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

#endif // INCLUDE_UINXED_H_
