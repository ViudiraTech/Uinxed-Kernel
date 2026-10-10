/*
 *
 *      numa.h
 *      NUMA topology and allocation policy
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NUMA_H_
#define INCLUDE_NUMA_H_

#include <arch/cpu/cpumask.h>

#define NUMA_NO_NODE         UINT16_MAX
#define NUMA_LOCAL_DISTANCE  10
#define NUMA_REMOTE_DISTANCE 20

_Static_assert(CONFIG_NUMA_MAX_NODES > 0 && CONFIG_NUMA_MAX_NODES <= 64, "NUMA nodemask fits in a word");

typedef uint64_t nodemask_t;

typedef struct {
        uint64_t base;
        uint64_t end; // exclusive, page aligned
        uint16_t node;
} numa_range_t;

typedef struct {
        uint32_t  proximity;
        cpumask_t cpus;
} numa_node_t;

typedef struct {
        uint32_t apic_id;
        uint16_t node;
} numa_cpu_affinity_t;

typedef struct {
        uint16_t            nr_nodes;
        uint16_t            nr_ranges;
        uint32_t            nr_affinities;
        bool                firmware;
        numa_node_t         nodes[CONFIG_NUMA_MAX_NODES];
        numa_range_t        ranges[CONFIG_NUMA_MAX_RANGES];
        numa_cpu_affinity_t affinities[CONFIG_SCHED_MAX_CPUS];
        uint8_t             distances[CONFIG_NUMA_MAX_NODES][CONFIG_NUMA_MAX_NODES];
        uint16_t            cpu_nodes[CONFIG_SCHED_MAX_CPUS];
} numa_topology_t;

enum {
    NUMA_POLICY_DEFAULT    = 0,
    NUMA_POLICY_PREFERRED  = 1,
    NUMA_POLICY_BIND       = 2,
    NUMA_POLICY_INTERLEAVE = 3,
    NUMA_POLICY_LOCAL      = 4,
};

typedef struct {
        uint32_t   mode;
        uint16_t   preferred;
        nodemask_t nodes;
} numa_policy_t;

extern numa_topology_t numa_topology;

/* Bootstrap discovery runs before the allocator and never allocates memory. */
void numa_init(void);
void numa_reset(void);

/* Length-bounded parser; malformed SRAT falls back atomically to UMA. */
int numa_parse_tables(const void *srat, size_t srat_size, const void *slit, size_t slit_size);

/* CPU bindings are completed by smp_init before topology publication. */
void       numa_bind_cpu(uint32_t cpu, uint32_t apic_id);
uint16_t   numa_cpu_node(uint32_t cpu);
uint16_t   numa_phys_node(uint64_t address);
uint8_t    numa_distance(uint16_t from, uint16_t to);
nodemask_t numa_possible_nodes(void);

/* Ordered node selection, also used by policy-constrained frame allocation. */
uint16_t numa_nearest_node(uint16_t from, nodemask_t candidates);
int      numa_policy_validate(const numa_policy_t *policy, nodemask_t allowed);

#endif // INCLUDE_NUMA_H_
