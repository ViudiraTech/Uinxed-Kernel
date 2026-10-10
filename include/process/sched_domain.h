/*
 *
 *      sched_domain.h
 *      Immutable scheduling topology and per-CPU balance state
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SCHED_DOMAIN_H_
#define INCLUDE_SCHED_DOMAIN_H_

#include <arch/cpu/cpumask.h>

typedef enum {
    SCHED_DOMAIN_SMT = 0,
    SCHED_DOMAIN_PACKAGE,
    SCHED_DOMAIN_NODE,
    SCHED_DOMAIN_NUMA,
    SCHED_DOMAIN_SYSTEM,
} sched_domain_level_t;

enum {
    SCHED_DOMAIN_BALANCE_WAKE     = 1U << 0,
    SCHED_DOMAIN_BALANCE_NEWIDLE  = 1U << 1,
    SCHED_DOMAIN_BALANCE_PERIODIC = 1U << 2,
    SCHED_DOMAIN_WAKE_AFFINE      = 1U << 3,
    SCHED_DOMAIN_SHARE_CAPACITY   = 1U << 4,
    SCHED_DOMAIN_SHARE_CACHE      = 1U << 5,
    SCHED_DOMAIN_REMOTE           = 1U << 6,
};

typedef struct sched_group {
        cpumask_t           span;
        uint64_t            capacity;
        struct sched_group *next; // immutable circular ring
} sched_group_t;

typedef struct sched_domain {
        uint8_t              level;
        uint8_t              distance;
        uint16_t             flags;
        uint32_t             span_weight;
        uint32_t             group_count;
        uint32_t             balance_interval;
        uint32_t             max_interval;
        uint16_t             imbalance_pct;
        cpumask_t            span;
        sched_group_t       *groups; // immutable groups may be shared across CPUs
        struct sched_domain *parent;
        struct sched_domain *child;
} sched_domain_t;

typedef struct {
        uint8_t        nr_domains;
        sched_domain_t domains[CONFIG_SCHED_DOMAIN_MAX_LEVELS];
} sched_domain_cpu_t;

extern sched_domain_cpu_t *cpu_sched_domains;

/* Called once, after all admitted CPUs have completed topology discovery. */
int         sched_domain_build(uint32_t count);
int         sched_domain_validate(uint32_t count);
bool        sched_domain_contains(uint32_t anchor, const sched_domain_t *domain, uint32_t cpu);
const char *sched_domain_name(uint8_t level);

#endif // INCLUDE_SCHED_DOMAIN_H_
