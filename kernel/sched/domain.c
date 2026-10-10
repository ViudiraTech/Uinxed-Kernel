/*
 *
 *      domain.c
 *      SMT, package, node and NUMA-distance scheduling domains
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/smp.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <mem/numa.h>
#include <process/sched_domain.h>

_Static_assert(CONFIG_SCHED_DOMAIN_MAX_LEVELS >= 2 && CONFIG_SCHED_DOMAIN_MAX_LEVELS <= 16, "domain depth retains a system root");

sched_domain_cpu_t *cpu_sched_domains;

/* Groups are the immediate hardware subdivisions of each level. */
static bool same_group(uint8_t level, uint32_t first, uint32_t second)
{
    if (level == SCHED_DOMAIN_SMT || level == SCHED_DOMAIN_SYSTEM) return first == second;
    if (level == SCHED_DOMAIN_PACKAGE) return cpu_topology_same_core(first, second);
    if (level == SCHED_DOMAIN_NODE) return cpu_topology_same_package(first, second);
    return numa_cpu_node(first) == numa_cpu_node(second);
}

bool sched_domain_contains(uint32_t anchor, const sched_domain_t *domain, uint32_t cpu)
{
    (void)anchor;
    return domain && cpumask_test_cpu(cpu, &domain->span);
}

/* Package domains are clipped to the node: a socket may span several nodes. */
static void build_span(uint32_t anchor, uint8_t level, uint8_t distance, uint32_t count, cpumask_t *span)
{
    cpumask_clear(span);
    uint16_t node = numa_cpu_node(anchor);
    for (uint32_t cpu = 0; cpu < count; cpu++) {
        bool member;
        switch (level) {
            case SCHED_DOMAIN_SMT :
                member = numa_cpu_node(cpu) == node && cpu_topology_same_core(anchor, cpu);
                break;
            case SCHED_DOMAIN_PACKAGE :
                member = numa_cpu_node(cpu) == node && cpu_topology_same_package(anchor, cpu);
                break;
            case SCHED_DOMAIN_NODE :
                member = numa_cpu_node(cpu) == node;
                break;
            case SCHED_DOMAIN_NUMA :
                member = numa_distance(node, numa_cpu_node(cpu)) <= distance;
                break;
            default :
                member = true;
                break;
        }
        if (member) cpumask_set_cpu(cpu, span);
    }
}

/* Reuse immutable rings with an identical level and span to bound memory. */
static sched_domain_t *find_shared(uint32_t cpu, const sched_domain_t *wanted)
{
    for (uint32_t previous = 0; previous < cpu; previous++) {
        sched_domain_cpu_t *topology = &cpu_sched_domains[previous];
        for (uint8_t i = 0; i < topology->nr_domains; i++) {
            sched_domain_t *domain = &topology->domains[i];
            if (domain->level == wanted->level && cpumask_equal(&domain->span, &wanted->span)) return domain;
        }
    }
    return NULL;
}

static int build_groups(uint32_t cpu, sched_domain_t *domain)
{
    sched_domain_t *shared = find_shared(cpu, domain);
    if (shared) {
        domain->groups      = shared->groups;
        domain->group_count = shared->group_count;
        return 0;
    }
    cpumask_t      remaining = domain->span;
    sched_group_t *last      = NULL;
    while (cpumask_weight(&remaining)) {
        uint32_t       first = cpumask_next(0, &remaining);
        sched_group_t *group = calloc(1, sizeof(*group));
        if (!group) return -ENOMEM;
        if (last)
            last->next = group;
        else
            domain->groups = group;
        last = group;
        for (uint32_t member = cpumask_next(0, &remaining); member < CONFIG_SCHED_MAX_CPUS; member = cpumask_next(member + 1, &remaining)) {
            if (!same_group(domain->level, first, member)) continue;
            cpumask_set_cpu(member, &group->span);
            remaining.bits[member / 64] &= ~(1ULL << (member % 64));
            const cpu_processor_t *processor = get_cpu_processor(member);
            group->capacity += processor && processor->capacity ? processor->capacity : 1024;
        }
        domain->group_count++;
    }
    if (last) last->next = domain->groups;
    return 0;
}

/* Preserve the root slot when many SLIT distance levels are present. */
static int add_domain(uint32_t cpu, uint8_t level, uint8_t distance, uint32_t count, bool root)
{
    sched_domain_cpu_t *topology = &cpu_sched_domains[cpu];
    if (topology->nr_domains >= CONFIG_SCHED_DOMAIN_MAX_LEVELS - (root ? 0 : 1)) return 0;
    cpumask_t span;
    build_span(cpu, level, distance, count, &span);
    sched_domain_t *child = topology->nr_domains ? &topology->domains[topology->nr_domains - 1] : NULL;
    if (child && cpumask_equal(&child->span, &span)) return 0;
    if (cpumask_weight(&span) < 2 && !root) return 0;
    if (child && !cpumask_subset(&child->span, &span)) return -EINVAL;
    sched_domain_t *domain = &topology->domains[topology->nr_domains];
    domain->span           = span;
    domain->level          = level;
    domain->distance       = distance;
    domain->span_weight    = cpumask_weight(&span);
    domain->flags          = SCHED_DOMAIN_BALANCE_WAKE | SCHED_DOMAIN_BALANCE_NEWIDLE | SCHED_DOMAIN_BALANCE_PERIODIC;
    uint32_t interval      = CONFIG_SCHED_LOAD_BALANCE_INTERVAL;
    if (level == SCHED_DOMAIN_SMT) {
        domain->flags |= SCHED_DOMAIN_SHARE_CAPACITY | SCHED_DOMAIN_SHARE_CACHE | SCHED_DOMAIN_WAKE_AFFINE;
        interval = interval > 2 ? interval / 4 : 1;
    } else if (level == SCHED_DOMAIN_PACKAGE) {
        domain->flags |= SCHED_DOMAIN_SHARE_CACHE | SCHED_DOMAIN_WAKE_AFFINE;
    } else if (level == SCHED_DOMAIN_NODE) {
        interval *= 2;
    } else {
        domain->flags |= SCHED_DOMAIN_REMOTE;
        interval *= 4 + (distance / NUMA_LOCAL_DISTANCE);
    }
    domain->balance_interval = interval ? interval : 1;
    domain->max_interval     = domain->balance_interval * 8;
    domain->imbalance_pct    = (domain->flags & SCHED_DOMAIN_REMOTE) ? 125 : 110;
    int rc                   = build_groups(cpu, domain);
    if (rc) return rc;
    domain->child = child;
    if (child) child->parent = domain;
    topology->nr_domains++;
    return 0;
}

/* Build privately before scheduler.started; readers never observe a rebuild. */
int sched_domain_build(uint32_t count)
{
    if (!count || count > CONFIG_SCHED_MAX_CPUS || cpu_sched_domains) return -EINVAL;
    cpu_sched_domains = calloc(count, sizeof(*cpu_sched_domains));
    if (!cpu_sched_domains) return -ENOMEM;
    for (uint32_t cpu = 0; cpu < count; cpu++) {
        int rc = add_domain(cpu, SCHED_DOMAIN_SMT, NUMA_LOCAL_DISTANCE, count, false);
        if (!rc) rc = add_domain(cpu, SCHED_DOMAIN_PACKAGE, NUMA_LOCAL_DISTANCE, count, false);
        if (!rc) rc = add_domain(cpu, SCHED_DOMAIN_NODE, NUMA_LOCAL_DISTANCE, count, false);
        if (rc) return rc;
        /* 255 denotes unreachable locality; the system root still covers it. */
        for (uint16_t distance = NUMA_LOCAL_DISTANCE + 1; distance < 255; distance++) {
            bool present = false;
            for (uint16_t node = 0; node < numa_topology.nr_nodes; node++)
                if (cpumask_weight(&numa_topology.nodes[node].cpus) && numa_distance(numa_cpu_node(cpu), node) == distance) present = true;
            if (!present) continue;
            rc = add_domain(cpu, SCHED_DOMAIN_NUMA, (uint8_t)distance, count, false);
            if (rc) return rc;
        }
        rc = add_domain(cpu, SCHED_DOMAIN_SYSTEM, 255, count, true);
        if (rc) return rc;
    }
    return sched_domain_validate(count);
}

/* Check complete, disjoint rings, CPU membership and parent/child links. */
int sched_domain_validate(uint32_t count)
{
    cpumask_t system;
    cpumask_fill(&system, count);
    for (uint32_t cpu = 0; cpu < count; cpu++) {
        const sched_domain_cpu_t *topology = &cpu_sched_domains[cpu];
        if (!topology->nr_domains || topology->nr_domains > CONFIG_SCHED_DOMAIN_MAX_LEVELS) return -EINVAL;
        for (uint8_t i = 0; i < topology->nr_domains; i++) {
            const sched_domain_t *domain = &topology->domains[i];
            if (!cpumask_test_cpu(cpu, &domain->span) || domain->span_weight != cpumask_weight(&domain->span) || !cpumask_subset(&domain->span, &system)) return -EINVAL;
            if (domain->parent != (i + 1 < topology->nr_domains ? &topology->domains[i + 1] : NULL) || domain->child != (i ? &topology->domains[i - 1] : NULL)) return -EINVAL;
            if (domain->parent && !cpumask_subset(&domain->span, &domain->parent->span)) return -EINVAL;
            cpumask_t            union_mask = {0};
            const sched_group_t *group      = domain->groups;
            for (uint32_t index = 0; index < domain->group_count; index++) {
                if (!group || !group->capacity || !cpumask_weight(&group->span)) return -EINVAL;
                for (size_t word = 0; word < CPUMASK_WORDS; word++) {
                    if (union_mask.bits[word] & group->span.bits[word]) return -EINVAL;
                    union_mask.bits[word] |= group->span.bits[word];
                }
                group = group->next;
                if (group == domain->groups && index + 1 != domain->group_count) return -EINVAL;
            }
            if (group != domain->groups || !cpumask_equal(&union_mask, &domain->span)) return -EINVAL;
        }
        if (!cpumask_equal(&topology->domains[topology->nr_domains - 1].span, &system)) return -EINVAL;
    }
    return 0;
}

const char *sched_domain_name(uint8_t level)
{
    static const char *const names[] = {"SMT", "PACKAGE", "NODE", "NUMA", "SYSTEM"};
    return level <= SCHED_DOMAIN_SYSTEM ? names[level] : "UNKNOWN";
}
