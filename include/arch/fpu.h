/*
 *
 *      fpu.h
 *      FPU/SSE/AVX management header file
 *
 *      2026/8/5 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FPU_H_
#define INCLUDE_FPU_H_

struct task;

/* Initialize FPU/SSE/AVX support on the current logical CPU.
 * Detects features via CPUID, programs CR0/CR4/XCR0 and caches the
 * XSAVE/FXSAVE area size.  Must be called on the BSP and on every AP
 * before the scheduler runs FP code on that CPU. */
void fpu_init(void);

/* Allocate and release the extended-state area owned by a task. */
int  fpu_task_init(struct task *task);
void fpu_task_destroy(struct task *task);

/* Preserve fork/clone semantics and reset state on exec. */
void fpu_task_clone(struct task *parent, struct task *child);
void fpu_task_reset(struct task *task);

/* Active save/restore hook for the scheduler.  Saves `prev`'s live FPU
 * state (if any) into its area and restores `next`'s state, so the
 * incoming task's state is live as soon as it resumes.  Never touches
 * CR0.TS and never relies on #NM. */
void fpu_switch(struct task *prev, struct task *next);

/* Enter/leave a kernel FPU section.  Must be called with interrupts in
 * any state; IRQs are masked for the whole section so that a context
 * switch can never interrupt FP register use.  Sections may nest.
 * kernel_fpu_begin() saves the current task's live state into its area;
 * kernel_fpu_end() restores it.  A kernel FPU section must never block
 * or sleep. */
void kernel_fpu_begin(void);
void kernel_fpu_end(void);

#endif /* INCLUDE_FPU_H_ */
