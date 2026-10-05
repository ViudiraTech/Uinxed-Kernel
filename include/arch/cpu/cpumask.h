/*
 *
 *      cpumask.h
 *      Bounded CPU masks shared by topology, affinity and cpusets
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CPUMASK_H_
#define INCLUDE_CPUMASK_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/util/bitops.h>

#define CPUMASK_WORDS ((CONFIG_SCHED_MAX_CPUS + 63) / 64)

typedef struct {
        uint64_t bits[CPUMASK_WORDS];
} cpumask_t;

/* Word loads also support task masks published under scheduler.lock. */
static inline bool cpumask_test_cpu(uint32_t cpu, const cpumask_t *mask)
{
    return cpu < CONFIG_SCHED_MAX_CPUS && (__atomic_load_n(&mask->bits[cpu / 64], __ATOMIC_RELAXED) & (1ULL << (cpu % 64)));
}

static inline void cpumask_set_cpu(uint32_t cpu, cpumask_t *mask)
{
    if (cpu < CONFIG_SCHED_MAX_CPUS) mask->bits[cpu / 64] |= 1ULL << (cpu % 64);
}

static inline void cpumask_clear(cpumask_t *mask)
{
    for (size_t i = 0; i < CPUMASK_WORDS; i++) mask->bits[i] = 0;
}

static inline void cpumask_fill(cpumask_t *mask, uint32_t count)
{
    cpumask_clear(mask);
    for (uint32_t cpu = 0; cpu < count && cpu < CONFIG_SCHED_MAX_CPUS; cpu++) cpumask_set_cpu(cpu, mask);
}

static inline void cpumask_and(cpumask_t *dst, const cpumask_t *a, const cpumask_t *b)
{
    for (size_t i = 0; i < CPUMASK_WORDS; i++) dst->bits[i] = a->bits[i] & b->bits[i];
}

static inline bool cpumask_subset(const cpumask_t *a, const cpumask_t *b)
{
    for (size_t i = 0; i < CPUMASK_WORDS; i++)
        if (a->bits[i] & ~b->bits[i]) return false;
    return true;
}

static inline bool cpumask_equal(const cpumask_t *a, const cpumask_t *b)
{
    return cpumask_subset(a, b) && cpumask_subset(b, a);
}

static inline uint32_t cpumask_weight(const cpumask_t *mask)
{
    uint32_t count = 0;
    for (size_t i = 0; i < CPUMASK_WORDS; i++) count += popcount64(mask->bits[i]);
    return count;
}

/* Return CONFIG_SCHED_MAX_CPUS at end; never pass an empty mask to ctz. */
static inline uint32_t cpumask_next(uint32_t start, const cpumask_t *mask)
{
    if (start >= CONFIG_SCHED_MAX_CPUS) return CONFIG_SCHED_MAX_CPUS;
    size_t   word = start / 64;
    uint64_t bits = mask->bits[word] & (UINT64_MAX << (start % 64));
    while (!bits && ++word < CPUMASK_WORDS) bits = mask->bits[word];
    uint32_t cpu = bits ? (uint32_t)((word * 64) + __builtin_ctzll(bits)) : CONFIG_SCHED_MAX_CPUS;
    return cpu < CONFIG_SCHED_MAX_CPUS ? cpu : CONFIG_SCHED_MAX_CPUS;
}

/* Parse a strict decimal CPU list; an empty list denotes inheritance. */
int cpumask_parse_list(const char *buf, size_t size, uint32_t limit, cpumask_t *mask);

/* Format a canonical range list, including a final newline. */
int cpumask_format_list(char *buf, size_t size, const cpumask_t *mask);

#endif // INCLUDE_CPUMASK_H_
