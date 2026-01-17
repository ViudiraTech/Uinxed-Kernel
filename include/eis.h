/*
 *
 *      eis.h
 *      Extended instruction set header file
 *
 *      2025/9/6 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EIS_H_
#define INCLUDE_EIS_H_

#define CPU_FEATURE_FPU 1
#define CPU_FEATURE_SSE 1
#define CPU_FEATURE_AVX 1

/* Initialize the FPU, including MMX (if any) */
void init_fpu(void);

/* Initialize the SSE, including SSE2 (if any) */
void init_sse(void);

/* Initialize the AVX, including AVX2 (if any) */
void init_avx(void);

#endif // INCLUDE_EIS_H_
