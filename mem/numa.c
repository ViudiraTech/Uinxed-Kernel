/*
 *
 *      numa.c
 *      Firmware NUMA topology, distance lookup and memory policy
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <libs/std/string.h>
#include <libs/util/bitops.h>
#include <mem/numa.h>

numa_topology_t numa_topology;

/* ACPI records are packed. memcpy avoids unaligned/aliasing C accesses. */
static uint32_t read_u32(const uint8_t *bytes)
{
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint64_t read_u64(const uint8_t *bytes)
{
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

/* Require the entire advertised table and its checksum before parsing. */
static bool valid_table(const void *table, size_t size, const char *signature, size_t minimum)
{
    if (!table || size < minimum) return false;
    const uint8_t *bytes  = table;
    uint32_t       length = read_u32(bytes + 4);
    if (memcmp(bytes, signature, 4) != 0 || length < minimum || length > size) return false;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) sum += bytes[i];
    return sum == 0;
}

/* Dense node IDs are independent of sparse firmware proximity IDs. */
static uint16_t node_for_proximity(uint32_t proximity)
{
    for (uint16_t node = 0; node < numa_topology.nr_nodes; node++)
        if (numa_topology.nodes[node].proximity == proximity) return node;
    if (numa_topology.nr_nodes == CONFIG_NUMA_MAX_NODES) return NUMA_NO_NODE;
    uint16_t node                       = numa_topology.nr_nodes++;
    numa_topology.nodes[node].proximity = proximity;
    return node;
}

/* Reset to a fully usable single-node topology on absent/broken firmware. */
void numa_reset(void)
{
    memset(&numa_topology, 0, sizeof(numa_topology));
    numa_topology.nr_nodes = 1;
    for (uint16_t from = 0; from < CONFIG_NUMA_MAX_NODES; from++)
        for (uint16_t to = 0; to < CONFIG_NUMA_MAX_NODES; to++) numa_topology.distances[from][to] = from == to ? NUMA_LOCAL_DISTANCE : NUMA_REMOTE_DISTANCE;
}

/* Duplicate APIC assignments may be repeated, but may not contradict. */
static int add_cpu(uint32_t apic_id, uint32_t proximity)
{
    uint16_t node = node_for_proximity(proximity);
    if (node == NUMA_NO_NODE) return -E2BIG;
    for (uint32_t i = 0; i < numa_topology.nr_affinities; i++) {
        numa_cpu_affinity_t *affinity = &numa_topology.affinities[i];
        if (affinity->apic_id == apic_id) return affinity->node == node ? 0 : -EINVAL;
    }
    if (numa_topology.nr_affinities == CONFIG_SCHED_MAX_CPUS) return -E2BIG;
    numa_topology.affinities[numa_topology.nr_affinities++] = (numa_cpu_affinity_t) {apic_id, node};
    return 0;
}

/* Reject overlap and wraparound; never infer usable RAM from SRAT alone. */
static int add_memory(uint64_t base, uint64_t length, uint32_t proximity)
{
    if (!length) return 0;
    if (length > UINT64_MAX - base || (base & 4095) || (length & 4095)) return -EINVAL;
    uint16_t node = node_for_proximity(proximity);
    if (node == NUMA_NO_NODE || numa_topology.nr_ranges == CONFIG_NUMA_MAX_RANGES) return -E2BIG;
    uint64_t end = base + length;
    for (uint16_t i = 0; i < numa_topology.nr_ranges; i++) {
        const numa_range_t *range = &numa_topology.ranges[i];
        if (base < range->end && end > range->base) return -EINVAL;
    }
    numa_topology.ranges[numa_topology.nr_ranges++] = (numa_range_t) {base, end, node};
    return 0;
}

/* Invalid SLIT is optional: retain conservative defaults as a whole. */
static void parse_slit(const uint8_t *bytes, size_t size)
{
    if (!valid_table(bytes, size, "SLIT", 44)) return;
    uint64_t count  = read_u64(bytes + 36);
    size_t   length = read_u32(bytes + 4);
    if (!count || count > (length - 44) / count) return;
    for (uint16_t node = 0; node < numa_topology.nr_nodes; node++)
        if (numa_topology.nodes[node].proximity >= count) return;
    for (uint64_t from = 0; from < count; from++)
        for (uint64_t to = 0; to < count; to++) {
            uint8_t distance = bytes[44 + (from * count) + to];
            if ((from == to && distance != NUMA_LOCAL_DISTANCE) || (from != to && distance <= NUMA_LOCAL_DISTANCE)) return;
        }
    for (uint16_t from = 0; from < numa_topology.nr_nodes; from++)
        for (uint16_t to = 0; to < numa_topology.nr_nodes; to++)
            numa_topology.distances[from][to] = bytes[44 + ((uint64_t)numa_topology.nodes[from].proximity * count) + numa_topology.nodes[to].proximity];
}

/* Parse SRAT local APIC, memory and x2APIC affinity records (ACPI 6.6). */
int numa_parse_tables(const void *srat, size_t srat_size, const void *slit, size_t slit_size)
{
    numa_reset();
    if (!CONFIG_NUMA || !srat) return 0;
    int rc = -EINVAL;
    if (!valid_table(srat, srat_size, "SRAT", 48)) return rc;
    const uint8_t *bytes   = srat;
    size_t         length  = read_u32(bytes + 4);
    numa_topology.nr_nodes = 0;
    for (size_t at = 48; at < length;) {
        if (length - at < 2) goto fallback;
        const uint8_t *entry      = bytes + at;
        size_t         entry_size = entry[1];
        if (entry_size < 2 || entry_size > length - at) goto fallback;
        rc = 0;
        switch (entry[0]) {
            case 0 :
                if (entry_size < 16) goto malformed;
                if (read_u32(entry + 4) & 1) {
                    uint32_t proximity = entry[2];
                    if (bytes[8] >= 2) proximity |= (uint32_t)entry[9] << 8 | (uint32_t)entry[10] << 16 | (uint32_t)entry[11] << 24;
                    rc = add_cpu(entry[3], proximity);
                }
                break;
            case 1 :
                if (entry_size < 40) goto malformed;
                if (read_u32(entry + 28) & 1) rc = add_memory(read_u64(entry + 8), read_u64(entry + 16), read_u32(entry + 2));
                break;
            case 2 :
                if (entry_size < 24) goto malformed;
                if (read_u32(entry + 12) & 1) rc = add_cpu(read_u32(entry + 8), read_u32(entry + 4));
                break;
            default :
                break; // Future records are skipped using their verified length.
        }
        if (rc) goto fallback;
        at += entry_size;
    }
    if (!numa_topology.nr_nodes || !numa_topology.nr_ranges) goto malformed;
    /* Stable physical ordering simplifies splitting usable boot ranges. */
    for (uint16_t i = 1; i < numa_topology.nr_ranges; i++) {
        numa_range_t range = numa_topology.ranges[i];
        uint16_t     j     = i;
        while (j && numa_topology.ranges[j - 1].base > range.base) {
            numa_topology.ranges[j] = numa_topology.ranges[j - 1];
            j--;
        }
        numa_topology.ranges[j] = range;
    }
    numa_topology.firmware = true;
    parse_slit(slit, slit_size);
    return 0;
malformed:
    rc = -EINVAL;
fallback:
    if (!rc) rc = -EINVAL;
    numa_reset();
    return rc;
}

/* Bind only CPUs actually admitted by smp_init, including the BSP as CPU 0. */
void numa_bind_cpu(uint32_t cpu, uint32_t apic_id)
{
    if (cpu >= CONFIG_SCHED_MAX_CPUS) return;
    uint16_t node = 0;
    for (uint32_t i = 0; i < numa_topology.nr_affinities; i++)
        if (numa_topology.affinities[i].apic_id == apic_id) {
            node = numa_topology.affinities[i].node;
            break;
        }
    numa_topology.cpu_nodes[cpu] = node;
    cpumask_set_cpu(cpu, &numa_topology.nodes[node].cpus);
}

uint16_t numa_cpu_node(uint32_t cpu)
{
    return cpu < CONFIG_SCHED_MAX_CPUS ? numa_topology.cpu_nodes[cpu] : 0;
}

uint16_t numa_phys_node(uint64_t address)
{
    for (uint16_t i = 0; i < numa_topology.nr_ranges; i++) {
        const numa_range_t *range = &numa_topology.ranges[i];
        if (address >= range->base && address < range->end) return range->node;
    }
    return 0; // Firmware gaps belong to the boot node; reserved RAM is never freed.
}

uint8_t numa_distance(uint16_t from, uint16_t to)
{
    if (from >= numa_topology.nr_nodes || to >= numa_topology.nr_nodes) return NUMA_REMOTE_DISTANCE;
    return numa_topology.distances[from][to];
}

nodemask_t numa_possible_nodes(void)
{
    return numa_topology.nr_nodes == 64 ? UINT64_MAX : (1ULL << numa_topology.nr_nodes) - 1;
}

uint16_t numa_nearest_node(uint16_t from, nodemask_t candidates)
{
    candidates &= numa_possible_nodes();
    uint16_t best     = NUMA_NO_NODE;
    uint16_t distance = UINT16_MAX;
    while (candidates) {
        uint16_t node    = (uint16_t)__builtin_ctzll(candidates);
        uint8_t  current = numa_distance(from, node);
        if (current < distance) {
            best     = node;
            distance = current;
        }
        candidates &= candidates - 1;
    }
    return best;
}

int numa_policy_validate(const numa_policy_t *policy, nodemask_t allowed)
{
    if (!policy || policy->mode > NUMA_POLICY_LOCAL || (policy->nodes & ~allowed)) return -EINVAL;
    if (policy->mode == NUMA_POLICY_DEFAULT || policy->mode == NUMA_POLICY_LOCAL) return policy->nodes ? -EINVAL : 0;
    if (!policy->nodes) return -EINVAL;
    if (policy->mode == NUMA_POLICY_PREFERRED && (policy->preferred >= 64 || popcount64(policy->nodes) != 1 || !(policy->nodes & (1ULL << policy->preferred)))) return -EINVAL;
    return 0;
}
