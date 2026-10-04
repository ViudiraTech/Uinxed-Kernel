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

/* Read the cycle counter, ordered against the surrounding loads. */
static inline uint64_t vdso_cycles(void)
{
    uint32_t low, high;

    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | (uint64_t)low;
}

/* Copy the published snapshot, retrying if a writer was mid-update. */
static int vdso_snapshot(struct vdso_data *out)
{
    for (int retry = 0; retry < 64; retry++) {
        uint32_t seq = __atomic_load_n(&vdso_data.seq, __ATOMIC_ACQUIRE);

        if (seq & 1) continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *out = vdso_data;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vdso_data.seq, __ATOMIC_RELAXED) == seq) return 0;
    }
    return -1;
}

/* Resolve one clock from the snapshot, interpolating over the cycle counter. */
static int vdso_clock(int clockid, struct timespec *ts)
{
    struct vdso_data data;
    uint64_t         sec, nsec;

    if (!ts || vdso_snapshot(&data)) return -1;

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

    /* The coarse clocks report tick granularity by definition, so leave them alone. */
    if (data.clock_mode == VDSO_CLOCKMODE_CYCLES && data.shift && clockid != 4 && clockid != 5 && clockid != 6) {
        uint64_t delta = vdso_cycles() - data.cycle_last;
        uint64_t added = (delta * data.mult) >> data.shift;

        nsec += added;
        sec += nsec / 1000000000ULL;
        nsec %= 1000000000ULL;
    }

    ts->tv_sec  = (long)sec;
    ts->tv_nsec = (long)nsec;
    return 0;
}

/* vDSO entry point for clock_gettime(2). */
int __vdso_clock_gettime(int clockid, struct timespec *ts)
{
    return vdso_clock(clockid, ts) ? -22 : 0;
}

/* vDSO entry point for clock_getres(2). */
int __vdso_clock_getres(int clockid, struct timespec *ts)
{
    struct vdso_data data;

    if (!ts) return 0;
    if (clockid != 0 && clockid != 1 && clockid != 4 && clockid != 5 && clockid != 6 && clockid != 7) return -22;
    if (vdso_snapshot(&data)) return -22;

    ts->tv_sec  = 0;
    ts->tv_nsec = (long)data.res_nsec;
    return 0;
}

/* vDSO entry point for gettimeofday(2); the timezone argument is ignored. */
int __vdso_gettimeofday(struct timeval *tv, void *tz)
{
    struct timespec ts;

    (void)tz;
    if (tv && !vdso_clock(0, &ts)) {
        tv->tv_sec  = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    return 0;
}

/* vDSO entry point for time(2). */
time_t __vdso_time(time_t *t)
{
    struct timespec ts;
    time_t          now = 0;

    if (!vdso_clock(0, &ts)) now = ts.tv_sec;
    if (t) *t = now;
    return now;
}
