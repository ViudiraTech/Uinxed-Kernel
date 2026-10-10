/* Uinxed guest probe: cc -O2 -Iinclude scripts/tests/vdso-clock.c -ldl -o vdso-clock
 * Run in the guest. The final seccomp filter marks actual syscall fallbacks.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
/* Use the host integer typedefs with the shared ABI header. */
#define INCLUDE_STDINT_H_
#include <kernel/vdso/vdso.h>
static int valid(struct timespec t)
{
    return t.tv_sec >= 0 && t.tv_sec <= (INT64_MAX - 999999999LL) / 1000000000LL && t.tv_nsec >= 0 && t.tv_nsec < 1000000000LL;
}

static int64_t ns(struct timespec t)
{
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}
int main(void)
{
    struct utsname system;
    if (uname(&system) || strcmp(system.sysname, "Uinxed")) {
        fputs("Run this probe in an Uinxed guest.\n", stderr);
        return 2;
    }
    void *h                           = dlopen("linux-vdso.so.1", RTLD_NOW);
    int (*fn)(int, struct timespec *) = h ? dlsym(h, "__vdso_clock_gettime") : NULL;
    uintptr_t base                    = getauxval(AT_SYSINFO_EHDR);
    if (!fn || !base) {
        fputs("NO_VDSO: clock_gettime image or symbol unavailable\n", stderr);
        return 2;
    }
    volatile struct vdso_data *d = (void *)(base + VDSO_DATA_OFFSET);
    printf("SNAPSHOT base=%lx seq=%u mode=%d cycle=%llu mult=%u shift=%u max=%llu mono=%llu.%09llu real=%llu.%09llu\n", base, d->seq, d->clock_mode, (unsigned long long)d->cycle_last, d->mult,
           d->shift, (unsigned long long)d->max_cycles, (unsigned long long)d->mono_sec, (unsigned long long)d->mono_nsec, (unsigned long long)d->real_sec, (unsigned long long)d->real_nsec);
    struct timespec a = {0}, b = {0}, c = {0};
    unsigned        bad = 0;
    int64_t         min = INT64_MAX, max = INT64_MIN;
    for (unsigned i = 0; i < 100000; i++) {
        long x  = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
        int  rc = fn(CLOCK_MONOTONIC, &b);
        long z  = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &c);
        if (x || rc || z || !valid(a) || !valid(b) || !valid(c)) {
            if (bad++ < 4) printf("BAD rc=%ld/%d/%ld raw_vdso=%ld.%09ld\n", x, rc, z, b.tv_sec, b.tv_nsec);
            continue;
        }
        int64_t low = ns(b) - ns(a), high = ns(b) - ns(c);
        if (low < min) min = low;
        if (high > max) max = high;
        if (low < -1000000 || high > 1000000) {
            if (bad++ < 4) printf("BAD bracket=%lld/%lld raw_vdso=%ld.%09ld\n", (long long)low, (long long)high, b.tv_sec, b.tv_nsec);
        }
    }
    printf("BRACKET samples=100000 bad=%u min_lower_ns=%lld max_upper_ns=%lld\n", bad, (long long)min, (long long)max);
    for (int k = 0; k < 2; k++) {
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
        for (unsigned i = 0; i < 200000; i++) {
            if (k)
                syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b);
            else
                fn(CLOCK_MONOTONIC, &b);
        }
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &c);
        printf("LATENCY %s ns_per_call=%.1f\n", k ? "syscall" : "vdso", (ns(c) - ns(a)) / 200000.0);
    }
    struct sock_filter code[] = {BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clock_gettime, 0, 1),
                                 BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 777), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
    struct sock_fprog  prog   = {4, code};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
        perror("seccomp");
        return 3;
    }
    unsigned fast = 0, fallback = 0, other = 0;
    for (unsigned i = 0; i < 100000; i++) {
        int rc = fn(CLOCK_MONOTONIC, &b);
        if (!rc)
            fast++;
        else if (rc == -777)
            fallback++;
        else
            other++;
    }
    printf("PATH samples=100000 fast=%u syscall_fallback=%u other=%u\n", fast, fallback, other);
    return bad || other;
}
