/*
 *
 *      eis.h
 *      Extended instruction set header file
 *
 *      2025/9/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EIS_H_
#define INCLUDE_EIS_H_

struct task;

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

/* Allocate and release the extended-state area owned by a task. */
int  fpu_task_init(struct task *task);
void fpu_task_destroy(struct task *task);

/* Preserve fork/clone semantics and reset state on exec. */
void fpu_task_clone(struct task *parent, struct task *child);
void fpu_task_reset(struct task *task);

/* Scheduler and #NM hooks implementing lazy restore. */
void fpu_context_switch(struct task *previous);
int  fpu_handle_device_not_available(void);

#endif // INCLUDE_EIS_H_
