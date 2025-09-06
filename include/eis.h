/*
 *
 *      eis.h
 *      Extended instruction set header file
 *
 *      2025/9/6 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_EIS_H_
#define INCLUDE_EIS_H_

#ifndef CPU_FEATURE_FPU
#    define CPU_FEATURE_FPU 1
#endif

#ifndef CPU_FEATURE_SSE
#    define CPU_FEATURE_SSE 1
#endif

#ifndef CPU_FEATURE_AVX
#    define CPU_FEATURE_AVX 1
#endif

/* Initialize the FPU, including MMX (if any) */
void init_fpu(void);

/* Initialize the SSE, including SSE2 (if any) */
void init_sse(void);

/* Initialize the AVX, including AVX2 (if any) */
void init_avx(void);

#endif // INCLUDE_EIS_H_
