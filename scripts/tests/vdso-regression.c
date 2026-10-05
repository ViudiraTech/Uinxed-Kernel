/* Host test: cc -O2 -Iinclude scripts/tests/vdso-regression.c -ldl -o /tmp/vdso-regression
 * Run after make vdso/vdso.so: /tmp/vdso-regression ./vdso/vdso.so
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <link.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
/* Use the host integer typedefs with the shared ABI header. */
#define INCLUDE_STDINT_H_
#include <kernel/vdso/vdso.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        puts(dlerror());
        return 2;
    }
    struct link_map *map;
    assert(!dlinfo(handle, RTLD_DI_LINKMAP, &map));
    struct vdso_data *data = (void *)(map->l_addr + VDSO_DATA_OFFSET);
    assert(!mprotect(data, 4096, PROT_READ | PROT_WRITE));
    int (*gettime)(int, struct timespec *) = dlsym(handle, "__vdso_clock_gettime");
    int (*getres)(int, struct timespec *)  = dlsym(handle, "__vdso_clock_getres");
    assert(gettime && getres);

    /* A writer that stays odd must fall back to the requested monotonic clock. */
    struct timespec before, actual, after;
    data->seq = 1;
    assert(!syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &before));
    assert(!gettime(CLOCK_MONOTONIC, &actual));
    assert(!syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &after));
    assert(actual.tv_sec >= before.tv_sec && actual.tv_sec <= after.tv_sec);

    /* Legitimate uptime beyond the old 1,000,000-second ceiling stays in userspace. */
    memset(data, 0, sizeof(*data));
    data->seq        = 2;
    data->clock_mode = VDSO_CLOCKMODE_CYCLES;
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
    data->cycle_last = ((uint64_t)high << 32) | low;
    data->mult       = 1;
    data->shift      = 32;
    data->max_cycles = UINT64_MAX;
    data->mono_sec   = 1100000;
    data->mono_nsec  = 42;
    assert(!gettime(CLOCK_MONOTONIC, &actual));
    assert(actual.tv_sec == 1100000 && actual.tv_nsec >= 42);

    /* Tag syscall results by clock ID so even a successful wrong-clock fallback fails. */
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clock_gettime, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clock_getres, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 700),
        BPF_STMT(BPF_ALU | BPF_OR | BPF_K, SECCOMP_RET_ERRNO),
        BPF_STMT(BPF_RET | BPF_A, 0),
    };
    struct sock_fprog program = {sizeof(filter) / sizeof(filter[0]), filter};
    assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
    assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program));
    assert(!gettime(CLOCK_MONOTONIC, &actual));
    data->seq = 1;
    assert(gettime(CLOCK_MONOTONIC, &actual) == -701);
    assert(getres(CLOCK_MONOTONIC_COARSE, &actual) == -706);
    data->seq        = 2;
    data->clock_mode = VDSO_CLOCKMODE_NONE;
    assert(gettime(CLOCK_MONOTONIC, &actual) == -701);
    data->clock_mode = VDSO_CLOCKMODE_CYCLES;
    data->mono_nsec  = 1000000000;
    assert(gettime(CLOCK_MONOTONIC, &actual) == -701);
    data->mono_nsec  = 42;
    data->cycle_last = UINT64_MAX;
    assert(gettime(CLOCK_MONOTONIC, &actual) == -701);
    data->cycle_last = 0;
    data->max_cycles = 0;
    assert(gettime(CLOCK_MONOTONIC, &actual) == -701);
    puts("VDSO_REGRESSION: retry exhaustion, long uptime, disabled counter, malformed snapshot, backward and stale cycles passed");
    return 0;
}
