/*
 *
 *      vdso.c
 *      vDSO image: time reads that never enter the kernel
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/vdso/vdso.h>
#include <libs/std/stddef.h>

typedef long time_t;

struct timespec {
        long tv_sec;
        long tv_nsec;
};

struct timeval {
        long tv_sec;
        long tv_usec;
};

/*
 * The linker script places this at VDSO_DATA_OFFSET, ahead of the code, and the
 * kernel maps its own read-only data page over it.  Keeping the symbol local and
 * hidden lets the compiler reach it with a PC-relative load instead of the GOT,
 * which is what makes the image position independent without a dynamic loader.
 */
__attribute__((section(".vvar"), used)) static struct vdso_data vdso_data;

/* vDSO errors follow the raw syscall ABI; libc need not retry them. */
static long vdso_syscall2(long number, long first, void *second)
{
    long result;

    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first), "S"(second) : "rcx", "r11", "memory");
    return result;
}

/* Read the cycle counter, ordered against the surrounding loads. */
static inline uint64_t vdso_cycles(void)
{
    uint32_t low, high;

    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | (uint64_t)low;
}

/* Copy the published snapshot, retrying if a writer was mid-update. */
static int vdso_snapshot(struct vdso_data *out, uint64_t *cycles)
{
    for (int retry = 0; retry < 64; retry++) {
        uint32_t seq = __atomic_load_n(&vdso_data.seq, __ATOMIC_ACQUIRE);

        if (seq & 1) continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        /* Each shared field must be loaded anew, without compiler vectorisation. */
#define VDSO_LOAD(field) out->field = __atomic_load_n(&vdso_data.field, __ATOMIC_RELAXED)
        VDSO_LOAD(clock_mode);
        VDSO_LOAD(cycle_last);
        VDSO_LOAD(mult);
        VDSO_LOAD(shift);
        VDSO_LOAD(max_cycles);
        VDSO_LOAD(real_sec);
        VDSO_LOAD(real_nsec);
        VDSO_LOAD(mono_sec);
        VDSO_LOAD(mono_nsec);
        VDSO_LOAD(boot_sec);
        VDSO_LOAD(boot_nsec);
        VDSO_LOAD(res_nsec);
#undef VDSO_LOAD
        /* A preemption between snapshot and counter read must invalidate both. */
        if (cycles) *cycles = vdso_cycles();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vdso_data.seq, __ATOMIC_RELAXED) == seq) return 0;
    }
    return -1;
}

/* Resolve one clock from the snapshot, interpolating over the cycle counter. */
static int vdso_clock(int clockid, struct timespec *ts)
{
    struct vdso_data data;
    uint64_t         sec, nsec, now;

    if (!ts || vdso_snapshot(&data, &now)) return -1;

    switch (clockid) {
        case 0 : // CLOCK_REALTIME
        case 5 : // CLOCK_REALTIME_COARSE
            sec  = data.real_sec;
            nsec = data.real_nsec;
            break;
        case 1 : // CLOCK_MONOTONIC
        case 4 : // CLOCK_MONOTONIC_RAW
        case 6 : // CLOCK_MONOTONIC_COARSE
        case 7 : // CLOCK_BOOTTIME
            sec  = data.mono_sec;
            nsec = data.mono_nsec;
            break;
        default :
            return -1;
    }

    if (nsec >= 1000000000ULL) return -1;
    /* Only coarse clocks may use a stale tick when no safe userspace counter exists. */
    if (clockid != 5 && clockid != 6) {
        if (data.clock_mode != VDSO_CLOCKMODE_CYCLES || !data.shift || data.shift >= 64) return -1;
        uint64_t delta = now - data.cycle_last;

        /*
         * Interpolate only over a counter that moved forward by a plausible
         * amount.  The counter is per-CPU, so a reader can land on one whose
         * value trails the CPU that published the snapshot; the unsigned
         * difference would then wrap into a jump of centuries, and nothing
         * downstream can tell that apart from a real reading.  Fall back to
         * the same clock's syscall rather than return a stale high-resolution
         * timestamp.
         */
        if (now >= data.cycle_last && delta <= data.max_cycles && (!data.mult || delta <= ~(uint64_t)0 / data.mult)) {
            uint64_t added = (delta * data.mult) >> data.shift;

            nsec += added;
            sec += nsec / 1000000000ULL;
            nsec %= 1000000000ULL;
        } else
            return -1;
    }

    ts->tv_sec  = (long)sec;
    ts->tv_nsec = (long)nsec;
    return 0;
}

/* vDSO entry point for clock_gettime(2). */
int __vdso_clock_gettime(int clockid, struct timespec *ts)
{
    return vdso_clock(clockid, ts) ? (int)vdso_syscall2(228, clockid, ts) : 0;
}

/* vDSO entry point for clock_getres(2). */
int __vdso_clock_getres(int clockid, struct timespec *ts)
{
    struct vdso_data data;

    if (!ts) return 0;
    if (clockid != 0 && clockid != 1 && clockid != 4 && clockid != 5 && clockid != 6 && clockid != 7) return (int)vdso_syscall2(229, clockid, ts);
    if (vdso_snapshot(&data, NULL)) return (int)vdso_syscall2(229, clockid, ts);

    ts->tv_sec  = 0;
    ts->tv_nsec = (long)data.res_nsec;
    return 0;
}

/* vDSO entry point for gettimeofday(2); the timezone argument is ignored. */
int __vdso_gettimeofday(struct timeval *tv, void *tz)
{
    struct timespec ts;

    (void)tz;
    if (!tv) return 0;
    /* Report failure rather than succeeding with the caller's buffer untouched. */
    if (vdso_clock(0, &ts)) return (int)vdso_syscall2(96, (long)tv, tz);

    tv->tv_sec  = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

/* vDSO entry point for time(2). */
time_t __vdso_time(time_t *t)
{
    struct timespec ts;
    time_t          now;

    if (vdso_clock(0, &ts)) return vdso_syscall2(201, (long)t, NULL);
    now = ts.tv_sec;
    if (t) *t = now;
    return now;
}
