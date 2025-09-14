/*
 *
 *      simd.h
 *      SIMD related header file
 *
 *      2025/9/6 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SIMD_H_
#define INCLUDE_SIMD_H_

/* Initialize the FPU, including MMX (if any) */
void init_fpu(void);

/* Initialize the SSE, including SSE2 (if any) */
void init_sse(void);

/* Initialize the AVX, including AVX2 (if any) */
void init_avx(void);

#endif // INCLUDE_SIMD_H_
