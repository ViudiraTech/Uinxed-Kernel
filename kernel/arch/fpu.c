/*
 *
 *      fpu.c
 *      FPU/SSE/AVX management: active save/restore
 *
 *      2026/8/5 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <arch/fpu.h>
#include <arch/smp.h>
#include <kernel/debug/debug.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/sched.h>
#include <process/task.h>

#define FPU_FXSAVE_SIZE     512U
#define FPU_STATE_ALIGNMENT 64U
#define FPU_INITIAL_MAX     4096U // static template cap; see guard in fpu_init()
#define FPU_DEFAULT_FCW     0x037fU
#define FPU_DEFAULT_MXCSR   0x1f80U

#define XCR0_X87_BIT       (1ULL << 0)
#define XCR0_SSE_BIT       (1ULL << 1)
#define XCR0_AVX_BIT       (1ULL << 2)
#define XCR0_OPMASK_BIT    (1ULL << 5)
#define XCR0_ZMM_HI256_BIT (1ULL << 6)
#define XCR0_HI16_ZMM_BIT  (1ULL << 7)
#define XCR0_FPU_MASK      (XCR0_X87_BIT | XCR0_SSE_BIT)
#define XCR0_AVX512_MASK   (XCR0_OPMASK_BIT | XCR0_ZMM_HI256_BIT | XCR0_HI16_ZMM_BIT)

#define CR0_EM_SHIFT 2
#define CR0_MP_SHIFT 1
#define CR0_TS_SHIFT 3

#define CR4_OSFXSR_SHIFT     9
#define CR4_OSXMMEXCPT_SHIFT 10
#define CR4_OSXSAVE_SHIFT    18

static size_t   fpu_save_size   = FPU_FXSAVE_SIZE;
static uint64_t fpu_xstate_mask = XCR0_FPU_MASK;
static uint8_t  fpu_use_xsave;
static uint8_t  fpu_use_xsaveopt;

/*
 * Set only when fpu_init() actually enabled SSE in hardware (CR4.OSFXSR).
 * Kernel SIMD paths must check kernel_sse_available() before executing any
 * SSE/AVX/crc32 instruction: with CR4.OSFXSR clear those raise #UD.
 */
static uint8_t fpu_sse_enabled;

int kernel_sse_available(void)
{
    return fpu_sse_enabled;
}

size_t fpu_signal_state_size(void)
{
#if CPU_FEATURE_FPU
    return fpu_save_size;
#else
    return 0;
#endif
}

/*
 * Initial (XINIT-style) extended-state template: x87 inits, MXCSR and
 * zeroed XMM/YMM, with the XSTATE_BV header covering every enabled
 * component.  Built in fpu_init() and XRSTORed whenever a task that has
 * never used the FPU becomes live, so a fresh/exec'd task can never
 * observe another task's register leftovers.
 */
static uint8_t fpu_initial_state[FPU_INITIAL_MAX] __attribute__((aligned(64)));

/* Early-boot (pre-SMP) fallback for the BSP only */
static fpu_percpu_t fpu_early_percpu;

/* Return the per-CPU FPU state, falling back to the early-boot area pre-SMP */
static fpu_percpu_t *fpu_percpu(void)
{
    cpu_processor_t *cpu = get_current_cpu();
    return cpu ? &cpu->fpu : &fpu_early_percpu;
}

/* Save live FPU/SSE/AVX state, using XSAVE if the feature is active */
static inline void fpu_save(void *state)
{
    if (fpu_use_xsave) {
        uint32_t low  = (uint32_t)fpu_xstate_mask;
        uint32_t high = (uint32_t)(fpu_xstate_mask >> 32);
        if (fpu_use_xsaveopt) {
            __asm__ volatile("xsaveopt64 (%0)" : : "r"(state), "a"(low), "d"(high) : "memory");
        } else {
            __asm__ volatile("xsave64 (%0)" : : "r"(state), "a"(low), "d"(high) : "memory");
        }
    } else {
        __asm__ volatile("fxsave64 (%0)" : : "r"(state) : "memory");
    }
}

/* Restore FPU/SSE/AVX state, using XRSTOR if the feature is active */
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

/* Reset a task's FPU state area to the x86-64 initial state */
static void fpu_state_set_initial(void *state)
{
    if (!state) return;
    memset(state, 0, fpu_save_size);
    *(uint16_t *)state = FPU_DEFAULT_FCW;
    if (fpu_use_xsave) *(uint64_t *)((uint8_t *)state + 512) = fpu_xstate_mask;
    *(uint32_t *)((uint8_t *)state + 24) = FPU_DEFAULT_MXCSR;
}

/* Reset the live FPU hardware registers to the x86-64 initial state */
static void fpu_hw_reset_initial(void)
{
    __asm__ volatile("fninit");
    fpu_restore(fpu_initial_state);
}

/* Save the interrupt flag and disable interrupts, returning whether they were enabled */
static inline uint64_t fpu_save_irq_and_cli(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("cli");
    return (rflags >> 9) & 1ULL;
}

/* Re-enable interrupts if they were enabled when fpu_save_irq_and_cli() ran */
static inline void fpu_restore_irq(uint64_t if_enabled)
{
    if (if_enabled) __asm__ volatile("sti");
}

/* fpu_init - detect features and enable the FPU on this CPU */
void fpu_init(void)
{
    fpu_percpu_t *fp   = fpu_percpu();
    fp->fpu_live       = NULL;
    fp->fpu_kernel_cnt = 0;

#if CPU_FEATURE_FPU
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0) : : "memory");
    cr0 &= ~(1ULL << CR0_EM_SHIFT); // EM = 0
    cr0 |= (1ULL << CR0_MP_SHIFT);  // MP = 1
    cr0 &= ~(1ULL << CR0_TS_SHIFT); // TS = 0 (no lazy FPU)
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

#    if CPU_FEATURE_SSE
    /*
     * CR4.OSFXSR must be set before any SSE instruction executes
     * (LDMXCSR and the SSE/AVX instructions themselves raise #UD
     * otherwise).  This also applies to the XSAVE path.
     */
    if (cpu_support_sse()) {
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4) : : "memory");
        cr4 |= (1ULL << CR4_OSFXSR_SHIFT);
        cr4 |= (1ULL << CR4_OSXMMEXCPT_SHIFT);
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        fpu_sse_enabled = 1;
    }
#    endif

    /*
     * Prefer XSAVE/XRSTOR whenever the CPU and XCR0 support it.
     * CR4.OSXSAVE must be written before reading the CPUID.1.ECX[27]
     * OSXSAVE status bit (it reflects current CR4.OSXSAVE).
     */
    if (cpu_support_xsave() && cpu_xcr0_supports(XCR0_FPU_MASK)) {
        uint64_t xcr0_mask = XCR0_FPU_MASK;
#    if CPU_FEATURE_AVX
        if (cpu_support_avx() && cpu_xcr0_supports(XCR0_FPU_MASK | XCR0_AVX_BIT)) xcr0_mask |= XCR0_AVX_BIT;
#        if CPU_FEATURE_AVX512
        if (cpu_support_avx512f() && cpu_xcr0_supports(XCR0_FPU_MASK | XCR0_AVX_BIT | XCR0_AVX512_MASK)) xcr0_mask |= XCR0_AVX512_MASK;
#        endif
#    endif
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4) : : "memory");
        cr4 |= (1ULL << CR4_OSXSAVE_SHIFT);
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

        if (cpu_support_osxsave()) {
            __asm__ volatile("xsetbv" : : "a"((uint32_t)xcr0_mask), "d"((uint32_t)(xcr0_mask >> 32)), "c"(0) : "memory");

            uint32_t eax, ebx, ecx, edx;
            cpuid_count(0x0000000d, 0, &eax, &ebx, &ecx, &edx);
            fpu_save_size = (ebx + FPU_STATE_ALIGNMENT - 1) & ~(FPU_STATE_ALIGNMENT - 1);

            /*
             * The heap is not available this early, so the initial-state
             * template cannot be grown at runtime.  Rather than refuse to
             * boot, drop back to legacy x87/SSE state (XCR0_FPU_MASK),
             * whose XSAVE area always fits the static template.
             */
            if (fpu_save_size > sizeof(fpu_initial_state)) {
                plogk("fpu: XSAVE area (%zu B) exceeds the %zu B template; disabling extended state.\n", fpu_save_size, sizeof(fpu_initial_state));
                xcr0_mask = XCR0_FPU_MASK;
                __asm__ volatile("xsetbv" : : "a"((uint32_t)xcr0_mask), "d"((uint32_t)(xcr0_mask >> 32)), "c"(0) : "memory");
                cpuid_count(0x0000000d, 0, &eax, &ebx, &ecx, &edx);
                fpu_save_size = (ebx + FPU_STATE_ALIGNMENT - 1) & ~(FPU_STATE_ALIGNMENT - 1);
            }
            cpuid_count(0x0000000d, 1, &eax, &ebx, &ecx, &edx);

            fpu_xstate_mask  = xcr0_mask;
            fpu_use_xsave    = 1;
            fpu_use_xsaveopt = (eax & 0x1) ? 1 : 0;
        }
    }
    if (!fpu_use_xsave) {
        fpu_save_size    = FPU_FXSAVE_SIZE;
        fpu_xstate_mask  = XCR0_FPU_MASK;
        fpu_use_xsave    = 0;
        fpu_use_xsaveopt = 0;
    }

    fpu_state_set_initial(fpu_initial_state);
    fpu_hw_reset_initial();
#else
    (void)fp;
#endif
}

/* Task FPU state lifecycle */

int fpu_task_init(struct task *task)
{
#if CPU_FEATURE_FPU
    if (!task) return -1;
    task->thread.fpu_state = aligned_alloc(FPU_STATE_ALIGNMENT, fpu_save_size);
    if (!task->thread.fpu_state) {
        plogk("fpu: %s: FPU state allocation failed (%zu bytes)\n", task->name, fpu_save_size);
        return -1;
    }
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
    /*
     * Defensive: if the dying task is still marked live on this CPU,
     * drop the pointer so a later fpu_switch() can never compare
     * against - or save into - the freed area.
     */
    fpu_percpu_t *fp = fpu_percpu();
    if (fp->fpu_live == task) fp->fpu_live = NULL;
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

    /*
     * If the parent is running on this CPU, its newest state is still
     * in the registers; flush it to the area first.  If the parent runs
     * elsewhere (or not at all), its area is already up to date thanks
     * to active save/restore.
     */
    fpu_percpu_t *fp = fpu_percpu();
    if (fp->fpu_live == parent) {
        fpu_save(parent->thread.fpu_state);
        parent->thread.fpu_initialized = 1;
        parent->thread.fpu_active      = 0;
    }

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

    fpu_state_set_initial(task->thread.fpu_state);
    task->thread.fpu_initialized = 0;

    /*
     * If the task is live on this CPU, reset the hardware as well so
     * that the live registers match the (initial) area contents.
     */
    fpu_percpu_t *fp = fpu_percpu();
    if (fp->fpu_live == task) {
        fpu_hw_reset_initial();
        task->thread.fpu_active = 1;
    } else {
        task->thread.fpu_active = 0;
    }
#else
    (void)task;
#endif
}

/* fpu_switch - active save/restore on context switch */
void fpu_switch(struct task *prev, struct task *next)
{
#if CPU_FEATURE_FPU
    if (!prev || !next || prev == next) return;

    fpu_percpu_t *fp = fpu_percpu();

    /* Save the outgoing task's live state (if it is live on this CPU). */
    if (fp->fpu_live == prev && prev->thread.fpu_state) {
        fpu_save(prev->thread.fpu_state);
        prev->thread.fpu_initialized = 1;
        prev->thread.fpu_active      = 0;
    }

    /* Restore the incoming task's state so it is live on resume. */
    fp->fpu_live = next;
    if (next->thread.fpu_state) {
        if (next->thread.fpu_initialized) {
            fpu_restore(next->thread.fpu_state);
        } else {
            fpu_hw_reset_initial();
        }
        next->thread.fpu_active = 1;
    }
#else
    (void)prev;
    (void)next;
#endif
}

int fpu_signal_save(struct task *task, void *state, size_t capacity)
{
#if CPU_FEATURE_FPU
    if (!task || !state || !task->thread.fpu_state || capacity < fpu_save_size || fpu_save_size > FPU_SIGNAL_STATE_MAX) return -1;

    uint64_t      if_enabled = fpu_save_irq_and_cli();
    fpu_percpu_t *fp         = fpu_percpu();
    if (fp->fpu_kernel_cnt || current_task() != task) {
        fpu_restore_irq(if_enabled);
        return -1;
    }

    if (fp->fpu_live == task) {
        fpu_save(task->thread.fpu_state);
        task->thread.fpu_initialized = 1;
    } else if (!task->thread.fpu_initialized) {
        fpu_state_set_initial(task->thread.fpu_state);
    }
    memcpy(state, task->thread.fpu_state, fpu_save_size);
    fpu_restore_irq(if_enabled);
    return 0;
#else
    (void)task;
    (void)state;
    (void)capacity;
    return 0;
#endif
}

int fpu_signal_restore(struct task *task, const void *state, size_t size)
{
#if CPU_FEATURE_FPU
    if (!task || !state || !task->thread.fpu_state || size != fpu_save_size || size > FPU_SIGNAL_STATE_MAX) return -1;

    uint64_t      if_enabled = fpu_save_irq_and_cli();
    fpu_percpu_t *fp         = fpu_percpu();
    if (fp->fpu_kernel_cnt || current_task() != task) {
        fpu_restore_irq(if_enabled);
        return -1;
    }

    memcpy(task->thread.fpu_state, state, fpu_save_size);

    /*
     * MXCSR reserved bits make FXRSTOR/XRSTOR raise #GP.  Mask them using
     * the hardware-provided mask from the initial FXSAVE image.
     */
    uint32_t mxcsr_mask = *(uint32_t *)(fpu_initial_state + 28);
    if (!mxcsr_mask) mxcsr_mask = 0x0000ffbfU;
    *(uint32_t *)((uint8_t *)task->thread.fpu_state + 24) &= mxcsr_mask;

    if (fpu_use_xsave) {
        uint8_t  *header    = (uint8_t *)task->thread.fpu_state + 512;
        uint64_t *xstate_bv = (uint64_t *)header;
        uint64_t *xcomp_bv  = (uint64_t *)(header + 8);
        *xstate_bv &= fpu_xstate_mask;
        *xcomp_bv = 0; // standard, non-compacted XSAVE format
        memset(header + 16, 0, 48);
    }

    fpu_restore(task->thread.fpu_state);
    fp->fpu_live                 = task;
    task->thread.fpu_initialized = 1;
    task->thread.fpu_active      = 1;
    fpu_restore_irq(if_enabled);
    return 0;
#else
    (void)task;
    (void)state;
    return size ? -1 : 0;
#endif
}

/* kernel_fpu_begin/end - explicit kernel FPU sections */

void kernel_fpu_begin(void)
{
#if CPU_FEATURE_FPU
    fpu_percpu_t *fp         = fpu_percpu();
    uint64_t      if_enabled = fpu_save_irq_and_cli();

    if (fp->fpu_kernel_cnt++ == 0) {
        fp->fpu_irq_saved = (uint8_t)if_enabled;
        /*
         * Save the current task's live state; the kernel owns the FPU
         * registers until kernel_fpu_end().
         */
        task_t *cur = current_task();
        if (cur && fp->fpu_live == cur && cur->thread.fpu_state) {
            fpu_save(cur->thread.fpu_state);
            cur->thread.fpu_initialized = 1;
            cur->thread.fpu_active      = 0;
        }
        fp->fpu_live = NULL;
    }
#endif
}

void kernel_fpu_end(void)
{
#if CPU_FEATURE_FPU
    fpu_percpu_t *fp = fpu_percpu();

    if (--fp->fpu_kernel_cnt == 0) {
        /*
         * Restore the current task's state (or an initial state for
         * tasks that never used the FPU).
         */
        task_t *cur = current_task();
        if (cur && cur->thread.fpu_state) {
            if (cur->thread.fpu_initialized) {
                fpu_restore(cur->thread.fpu_state);
            } else {
                fpu_hw_reset_initial();
            }
            cur->thread.fpu_active = 1;
        }
        fp->fpu_live = cur;
        fpu_restore_irq(fp->fpu_irq_saved);
    }
#endif
}
