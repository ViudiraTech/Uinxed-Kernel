/*
 *
 *      topology_sysfs.c
 *      /sys/devices/system/{cpu,node} topology and scheduler diagnostics
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smp.h>
#include <fs/sysfs/topology_sysfs.h>
#include <kernel/errno.h>
#include <libs/kobject/kobject.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <process/sched.h>

#if CONFIG_SYSFS

typedef struct {
        struct kobject kobj;
        uint32_t       id;
        bool           node;
} topology_object_t;

static struct attribute cpulist_attr      = __ATTR_RO(cpulist);
static struct attribute distance_attr     = __ATTR_RO(distance);
static struct attribute meminfo_attr      = __ATTR_RO(meminfo);
static struct attribute proximity_attr    = __ATTR_RO(proximity_domain);
static struct attribute online_attr       = __ATTR_RO(online);
static struct attribute possible_attr     = __ATTR_RO(possible);
static struct attribute present_attr      = __ATTR_RO(present);
static struct attribute memory_attr       = __ATTR_RO(has_memory);
static struct attribute node_attr         = __ATTR_RO(numa_node);
static struct attribute capacity_attr     = __ATTR_RO(cpu_capacity);
static struct attribute domains_attr      = __ATTR_RO(sched_domains);
static struct attribute core_attr         = __ATTR_RO(core_id);
static struct attribute package_attr      = __ATTR_RO(physical_package_id);
static struct attribute siblings_attr     = __ATTR_RO(thread_siblings_list);
static struct attribute package_cpus_attr = __ATTR_RO(package_cpus_list);

static struct attribute *node_attrs[]      = {&cpulist_attr, &distance_attr, &meminfo_attr, &proximity_attr, NULL};
static struct attribute *cpu_attrs[]       = {&online_attr, &node_attr, &capacity_attr, &domains_attr, NULL};
static struct attribute *topology_attrs[]  = {&core_attr, &package_attr, &siblings_attr, &package_cpus_attr, NULL};
static struct attribute *cpu_root_attrs[]  = {&online_attr, &possible_attr, &present_attr, NULL};
static struct attribute *node_root_attrs[] = {&online_attr, &possible_attr, &memory_attr, NULL};

/* Domains are immutable; mutable per-CPU counters use relaxed snapshots. */
static ssize_t domains_show(uint32_t cpu, char *buf)
{
    size_t                    at       = 0;
    const sched_domain_cpu_t *topology = &cpu_sched_domains[cpu];
    for (uint8_t i = 0; i < topology->nr_domains; i++) {
        const sched_domain_t *domain = &topology->domains[i];
        char                  span[SYSFS_PAGE_SIZE];
        int                   length = cpumask_format_list(span, sizeof(span), &domain->span);
        if (length < 0) return length;
        span[length - 1] = '\0';
        int n = snprintf(buf + at, SYSFS_PAGE_SIZE - at, "%u %s distance=%u span=%s groups=%u flags=%#x interval=%u attempts=%llu moved=%llu\n", i, sched_domain_name(domain->level), domain->distance,
                         span, domain->group_count, domain->flags, __atomic_load_n(&cpu_rqs[cpu].domain_interval[i], __ATOMIC_RELAXED),
                         __atomic_load_n(&cpu_rqs[cpu].domain_attempts[i], __ATOMIC_RELAXED), __atomic_load_n(&cpu_rqs[cpu].domain_moved[i], __ATOMIC_RELAXED));
        if (n < 0 || (size_t)n >= SYSFS_PAGE_SIZE - at) return -ENOSPC;
        at += (size_t)n;
    }
    return (ssize_t)at;
}

static ssize_t object_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    topology_object_t *object = container_of(kobj, topology_object_t, kobj);
    uint32_t           id     = object->id;
    if (object->node) {
        if (attr == &cpulist_attr) return cpumask_format_list(buf, SYSFS_PAGE_SIZE, &numa_topology.nodes[id].cpus);
        if (attr == &proximity_attr) return sysfs_emit(buf, "%u\n", numa_topology.nodes[id].proximity);
        if (attr == &distance_attr) {
            size_t at = 0;
            for (uint16_t node = 0; node < numa_topology.nr_nodes; node++) at += (size_t)sysfs_emit_at(buf, (int)at, "%s%u", node ? " " : "", numa_distance((uint16_t)id, node));
            return (ssize_t)(at + (size_t)sysfs_emit_at(buf, (int)at, "\n"));
        }
        if (attr == &meminfo_attr) {
            frame_stats_t stats;
            if (frame_get_node_stats((uint16_t)id, &stats)) return -EINVAL;
            return sysfs_emit(buf, "Node %u MemTotal: %zu kB\nNode %u MemFree: %zu kB\nNode %u MemUsed: %zu kB\n", id, stats.total_frames * 4, id, stats.free_frames * 4, id,
                              (stats.total_frames > stats.free_frames ? stats.total_frames - stats.free_frames : 0) * 4);
        }
    } else {
        const cpu_processor_t *processor = get_cpu_processor(id);
        if (attr == &online_attr) return sysfs_emit(buf, "%u\n", __atomic_load_n(&cpu_rqs[id].online, __ATOMIC_ACQUIRE));
        if (attr == &node_attr) return sysfs_emit(buf, "%u\n", numa_cpu_node(id));
        if (attr == &capacity_attr) return sysfs_emit(buf, "%u\n", processor ? processor->capacity : 1024);
        if (attr == &domains_attr) return domains_show(id, buf);
        if (attr == &core_attr) return sysfs_emit(buf, "%u\n", processor ? processor->core_id : id);
        if (attr == &package_attr) return sysfs_emit(buf, "%u\n", processor ? processor->package_id : 0);
        cpumask_t mask = {0};
        if (attr == &siblings_attr || attr == &package_cpus_attr) {
            for (uint32_t cpu = 0; cpu < sched_cpu_count(); cpu++)
                if (attr == &siblings_attr ? cpu_topology_same_core(id, cpu) : cpu_topology_same_package(id, cpu)) cpumask_set_cpu(cpu, &mask);
            return cpumask_format_list(buf, SYSFS_PAGE_SIZE, &mask);
        }
    }
    return -EINVAL;
}

static ssize_t root_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    cpumask_t mask = {0};
    if (!strcmp(kobject_name(kobj), "cpu"))
        cpumask_fill(&mask, sched_cpu_count());
    else
        mask.bits[0] = attr == &memory_attr ? frame_memory_nodes() : numa_possible_nodes();
    return cpumask_format_list(buf, SYSFS_PAGE_SIZE, &mask);
}

static const struct sysfs_ops object_ops     = {.show = object_show};
static const struct sysfs_ops root_ops       = {.show = root_show};
static struct kobj_type       node_type      = {.release = kobject_static_release, .sysfs_ops = &object_ops, .default_attrs = node_attrs};
static struct kobj_type       cpu_type       = {.release = kobject_static_release, .sysfs_ops = &object_ops, .default_attrs = cpu_attrs};
static struct kobj_type       topology_type  = {.release = kobject_static_release, .sysfs_ops = &object_ops, .default_attrs = topology_attrs};
static struct kobj_type       cpu_root_type  = {.release = kobject_static_release, .sysfs_ops = &root_ops, .default_attrs = cpu_root_attrs};
static struct kobj_type       node_root_type = {.release = kobject_static_release, .sysfs_ops = &root_ops, .default_attrs = node_root_attrs};

typedef struct {
        struct kobject    *cpu_root;
        struct kobject    *node_root;
        topology_object_t *nodes;
        topology_object_t *cpus;
        topology_object_t *hardware;
} topology_export_t;

static topology_export_t *topology_export;

/* Unwind partial registration before any userspace reader can observe it. */
static void topology_export_release(topology_export_t *objects)
{
    for (uint32_t cpu = 0; cpu < sched_cpu_count(); cpu++) {
        char name[32];
        if (objects->cpus && objects->cpus[cpu].kobj.state_initialized) {
            (void)snprintf(name, sizeof(name), "node%u", numa_cpu_node(cpu));
            sysfs_remove_symlink(&objects->cpus[cpu].kobj, name);
        }
        if (objects->nodes && objects->nodes[numa_cpu_node(cpu)].kobj.state_initialized) {
            (void)snprintf(name, sizeof(name), "cpu%u", cpu);
            sysfs_remove_symlink(&objects->nodes[numa_cpu_node(cpu)].kobj, name);
        }
    }
    for (uint32_t cpu = 0; cpu < sched_cpu_count(); cpu++) {
        if (objects->hardware && objects->hardware[cpu].kobj.state_initialized) {
            kobject_del(&objects->hardware[cpu].kobj);
            kobject_put(&objects->hardware[cpu].kobj);
        }
        if (objects->cpus && objects->cpus[cpu].kobj.state_initialized) {
            kobject_del(&objects->cpus[cpu].kobj);
            kobject_put(&objects->cpus[cpu].kobj);
        }
    }
    for (uint16_t node = 0; node < numa_topology.nr_nodes; node++) {
        if (objects->nodes && objects->nodes[node].kobj.state_initialized) {
            kobject_del(&objects->nodes[node].kobj);
            kobject_put(&objects->nodes[node].kobj);
        }
    }
    if (objects->cpu_root && objects->cpu_root->state_initialized) {
        kobject_del(objects->cpu_root);
        kobject_put(objects->cpu_root);
    }
    if (objects->node_root && objects->node_root->state_initialized) {
        kobject_del(objects->node_root);
        kobject_put(objects->node_root);
    }
    free(objects->cpu_root);
    free(objects->node_root);
    free(objects->hardware);
    free(objects->cpus);
    free(objects->nodes);
    free(objects);
}

/* Objects and immutable topology masks live for this boot's lifetime. */
void topology_sysfs_init(void)
{
    if (!sysfs_root_kobj || topology_export) return;
    struct kobject *devices = kobject_find_child(sysfs_root_kobj, "devices");
    if (!devices) return;
    struct kobject *system = kobject_find_child(devices, "system");
    if (!system) system = kobject_create_and_add("system", devices);
    if (!system) return;
    topology_export_t *objects = calloc(1, sizeof(*objects));
    if (!objects) return;
    objects->cpu_root  = calloc(1, sizeof(*objects->cpu_root));
    objects->node_root = calloc(1, sizeof(*objects->node_root));
    objects->nodes     = calloc(numa_topology.nr_nodes, sizeof(*objects->nodes));
    objects->cpus      = calloc(sched_cpu_count(), sizeof(*objects->cpus));
    objects->hardware  = calloc(sched_cpu_count(), sizeof(*objects->hardware));
    if (!objects->cpu_root || !objects->node_root || !objects->nodes || !objects->cpus || !objects->hardware) goto fail;
    if (kobject_init_and_add(objects->cpu_root, &cpu_root_type, system, "cpu") || kobject_init_and_add(objects->node_root, &node_root_type, system, "node")) goto fail;
    for (uint16_t node = 0; node < numa_topology.nr_nodes; node++) {
        objects->nodes[node].id   = node;
        objects->nodes[node].node = true;
        if (kobject_init_and_add(&objects->nodes[node].kobj, &node_type, objects->node_root, "node%u", node)) goto fail;
    }
    for (uint32_t cpu = 0; cpu < sched_cpu_count(); cpu++) {
        objects->cpus[cpu].id = objects->hardware[cpu].id = cpu;
        if (kobject_init_and_add(&objects->cpus[cpu].kobj, &cpu_type, objects->cpu_root, "cpu%u", cpu)) goto fail;
        if (kobject_init_and_add(&objects->hardware[cpu].kobj, &topology_type, &objects->cpus[cpu].kobj, "topology")) goto fail;
        char name[32];
        (void)snprintf(name, sizeof(name), "node%u", numa_cpu_node(cpu));
        if (sysfs_create_symlink(&objects->cpus[cpu].kobj, &objects->nodes[numa_cpu_node(cpu)].kobj, name)) goto fail;
        (void)snprintf(name, sizeof(name), "cpu%u", cpu);
        if (sysfs_create_symlink(&objects->nodes[numa_cpu_node(cpu)].kobj, &objects->cpus[cpu].kobj, name)) goto fail;
    }
    topology_export = objects;
    return;
fail:
    topology_export_release(objects);
    plogk("numa: Cannot publish CPU/node sysfs topology.\n");
}

#else

void topology_sysfs_init(void) {}

#endif // CONFIG_SYSFS
