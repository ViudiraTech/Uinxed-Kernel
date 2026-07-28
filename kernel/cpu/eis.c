/*
 *
 *      eis.c
 *      Extended instruction set
 *
 *      2025/9/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <arch/eis.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <proc/sched.h>
#include <proc/task.h>

#define FPU_FXSAVE_SIZE      512U
#define FPU_STATE_ALIGNMENT  64U
#define FPU_DEFAULT_FCW      0x037fU
#define FPU_DEFAULT_MXCSR    0x1f80U

static size_t   fpu_save_size   = FPU_FXSAVE_SIZE;
static uint64_t fpu_xstate_mask = 0x3;
static uint8_t  fpu_use_xsave;

static inline void fpu_clts(void)
{
    __asm__ volatile("clts" ::: "memory");
}

static inline void fpu_stts(void)
{
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 1ULL << 3;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static inline void fpu_save(void *state)
{
    if (fpu_use_xsave) {
        uint32_t low  = (uint32_t)fpu_xstate_mask;
        uint32_t high = (uint32_t)(fpu_xstate_mask >> 32);
        __asm__ volatile("xsave64 (%0)" : : "r"(state), "a"(low), "d"(high) : "memory");
    } else {
        __asm__ volatile("fxsave64 (%0)" : : "r"(state) : "memory");
    }
}

static inline void fpu_restore(const void *state)
{
    if (fpu_use_xsave) {
        uint32_t low  = (uint32_t)fpu_xstate_mask;
        uint32_t high = (uint32_t)(fpu_xstate_mask >> 32);
        __asm__ volatile("xrstor64 (%0)" : : "r"(state), "a"(low), "d"(high) : "memory");
    } else {
        __asm__ volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
    }
}

static void fpu_state_set_initial(void *state)
{
    if (!state) return;
    memset(state, 0, fpu_save_size);
    *(uint16_t *)state                    = FPU_DEFAULT_FCW;
    *(uint32_t *)((uint8_t *)state + 24) = FPU_DEFAULT_MXCSR;
}

/* Initialize the FPU, including MMX (if any) */
void init_fpu(void)
{
#if CPU_FEATURE_FPU
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0)::"memory");

    cr0 &= ~(1UL << 2); // EM = 0
    cr0 |= (1UL << 1);  // MP = 1
    cr0 &= ~(1UL << 3); // TS = 0

    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    __asm__ volatile("fninit");
#else
    return;
#endif
}

/* Initialize the SSE, including SSE2 (if any) */
void init_sse(void)
{
#if CPU_FEATURE_SSE
    if (!cpu_support_sse()) return;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4)::"memory");

    cr4 |= (1UL << 9);  // OSFXSR = 1
    cr4 |= (1UL << 10); // OSXMMEXCPT = 1

    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
#else
    return;
#endif
}

/* Initialize the AVX, including AVX2 (if any) */
void init_avx(void)
{
#if CPU_FEATURE_AVX
    if (!cpu_support_avx() || !cpu_support_xsave()) return;

    uint64_t cr4;
    uint64_t xcr0 = 0x7;
    if (!cpu_xcr0_supports(xcr0)) return;

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4)::"memory");

    cr4 |= (1UL << 18); // OSXSAVE = 1

    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    __asm__ volatile("xsetbv" ::"a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)), "c"(0) : "memory");

    uint32_t eax, ebx, ecx, edx;
    cpuid_count(0x0000000d, 0, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ecx;
    (void)edx;
    fpu_xstate_mask = xcr0;
    fpu_save_size   = (ebx + FPU_STATE_ALIGNMENT - 1) & ~(FPU_STATE_ALIGNMENT - 1);
    fpu_use_xsave   = 1;
#else
    return;
#endif
}

int fpu_task_init(struct task *task)
{
#if CPU_FEATURE_FPU
    if (!task) return -1;
    task->thread.fpu_state = aligned_alloc(FPU_STATE_ALIGNMENT, fpu_save_size);
    if (!task->thread.fpu_state) return -1;
    fpu_state_set_initial(task->thread.fpu_state);
    task->thread.fpu_initialized = 0;
    task->thread.fpu_active      = 0;
#else
    (void)task;
#endif
    return 0;
}

void fpu_task_destroy(struct task *task)
{
#if CPU_FEATURE_FPU
    if (!task) return;
    free(task->thread.fpu_state);
    task->thread.fpu_state       = NULL;
    task->thread.fpu_initialized = 0;
    task->thread.fpu_active      = 0;
#else
    (void)task;
#endif
}

void fpu_task_clone(struct task *parent, struct task *child)
{
#if CPU_FEATURE_FPU
    if (!parent || !child || !parent->thread.fpu_state || !child->thread.fpu_state) return;

    /* A running parent's newest state may not have reached memory yet. */
    if (parent->thread.fpu_active) fpu_save(parent->thread.fpu_state);
    if (parent->thread.fpu_initialized) {
        memcpy(child->thread.fpu_state, parent->thread.fpu_state, fpu_save_size);
        child->thread.fpu_initialized = 1;
    } else {
        fpu_state_set_initial(child->thread.fpu_state);
        child->thread.fpu_initialized = 0;
    }
    child->thread.fpu_active = 0;
#else
    (void)parent;
    (void)child;
#endif
}

void fpu_task_reset(struct task *task)
{
#if CPU_FEATURE_FPU
    if (!task || !task->thread.fpu_state) return;
    if (task->thread.fpu_active) fpu_stts();
    fpu_state_set_initial(task->thread.fpu_state);
    task->thread.fpu_initialized = 0;
    task->thread.fpu_active      = 0;
#else
    (void)task;
#endif
}

void fpu_context_switch(struct task *previous)
{
#if CPU_FEATURE_FPU
    /* Save only tasks which have actually executed an FP/SIMD instruction. */
    if (previous && previous->thread.fpu_active && previous->thread.fpu_state) {
        fpu_clts();
        fpu_save(previous->thread.fpu_state);
        previous->thread.fpu_initialized = 1;
        previous->thread.fpu_active      = 0;
    }
    /* The incoming task pays XRSTOR only on its first FP/SIMD instruction. */
    fpu_stts();
#else
    (void)previous;
#endif
}

__attribute__((target("general-regs-only"))) int fpu_handle_device_not_available(void)
{
#if CPU_FEATURE_FPU
    fpu_clts();

    task_t *task = current_task();
    if (!task || !task->thread.fpu_state) {
        __asm__ volatile("fninit");
        return 1;
    }

    fpu_restore(task->thread.fpu_state);
    task->thread.fpu_initialized = 1;
    task->thread.fpu_active      = 1;
    return 1;
#else
    return 0;
#endif
}
