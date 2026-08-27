/*
 *
 *      cgroup.h
 *      Control group (cgroup) unified hierarchy header
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CGROUP_H_
#define INCLUDE_CGROUP_H_

#include <libs/list/intrusive_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

struct task;
typedef struct cgroup cgroup_t;

#define CGROUP_CONTROLLER_PIDS   (1ULL << 0)
#define CGROUP_CONTROLLER_MEMORY (1ULL << 1)
#define CGROUP_CONTROLLER_CPU    (1ULL << 2)
#define CGROUP_CONTROLLER_IO     (1ULL << 3)
#define CGROUP_CONTROLLER_CPUSET (1ULL << 4)

#define CGROUP_PIDS_MAX        UINT64_MAX
#define CGROUP_MEMORY_MAX      UINT64_MAX

/* Initialize the cgroup unified hierarchy (root cgroup + controllers) */
void cgroup_init(void);

/* Get the root cgroup (NULL until cgroup_init) */
cgroup_t *cgroup_root(void);

/* Take a reference on a cgroup, refusing to resurrect a dying one */
cgroup_t *cgroup_get(cgroup_t *cgroup);

/* Drop a cgroup reference, freeing it once the last dying reference is gone */
void cgroup_put(cgroup_t *cgroup);

/* Register a controller */
int cgroup_register_controller(const char *name, uint64_t id);

/* Attach a newly forked task to its parent's cgroup */
int cgroup_task_fork(struct task *task, struct task *parent);

/* Detach a task from its cgroup as it exits */
void cgroup_task_exit(struct task *task);

/* Move an existing task into the given cgroup */
int cgroup_attach_task(cgroup_t *cgroup, struct task *task);

/* Create a child cgroup under parent, rejecting duplicate names */
int cgroup_create(cgroup_t *parent, const char *name, cgroup_t **result);

/* Destroy an empty cgroup, refusing to remove the root or a busy subtree */
int cgroup_destroy(cgroup_t *cgroup);

/* Apply a whitespace-separated list of +controller/-controller control operations */
int cgroup_set_subtree_control(cgroup_t *cgroup, const char *value, size_t size);

/* Move the task with the given PID (0 = current task) into this cgroup */
int cgroup_move_pid(cgroup_t *cgroup, const char *value, size_t size);

/* Kill all tasks in this cgroup and its descendants */
int cgroup_kill(cgroup_t *cgroup, const char *value, size_t size);

/* Freeze/unfreeze a cgroup */
int cgroup_set_freeze(cgroup_t *cgroup, const char *value, size_t size);

/* Controllers setter/getter functions */
int cgroup_set_pids_max(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_memory_max(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_memory_high(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_memory_low(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_cpu_max(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_cpu_weight(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_io_max(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_io_weight(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_cpuset_cpus(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_cpuset_mems(cgroup_t *cgroup, const char *value, size_t size);

/* Get the parent of a cgroup */
cgroup_t *cgroup_parent(cgroup_t *cgroup);

/* Get the name of a cgroup */
const char *cgroup_name(cgroup_t *cgroup);

/* Get the controller bitmask enabled for this cgroup's subtree */
uint64_t cgroup_subtree_control(cgroup_t *cgroup);

/* Whether a controller is usable on this cgroup */
int cgroup_controller_available(cgroup_t *cgroup, uint64_t controller_flag);
int cgroup_pids_available(cgroup_t *cgroup);

/* Whether the cgroup is the root cgroup */
int cgroup_is_root(cgroup_t *cgroup);

/* Control files formatting functions */
int cgroup_show_controllers(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_subtree_control(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_procs(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_threads(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_events(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_stat(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_max_descendants(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_max_depth(cgroup_t *cgroup, char *buf, size_t size);

int cgroup_show_pids_current(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_pids_max(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_pids_events(cgroup_t *cgroup, char *buf, size_t size);

int cgroup_show_memory_current(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_memory_max(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_memory_high(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_memory_low(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_memory_stat(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_memory_events(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_memory_swap_current(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_memory_swap_max(cgroup_t *cgroup, char *buf, size_t size);

int cgroup_show_cpu_max(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_cpu_weight(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_cpu_stat(cgroup_t *cgroup, char *buf, size_t size);

int cgroup_show_io_max(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_io_weight(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_io_stat(cgroup_t *cgroup, char *buf, size_t size);

int cgroup_show_cpuset_cpus(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_cpuset_mems(cgroup_t *cgroup, char *buf, size_t size);

/* Format the full path of a cgroup, relative to the root hierarchy */
int cgroup_format_path(cgroup_t *cgroup, char *buf, size_t size);

/* Format the /proc/cgroups file for the unified hierarchy */
int cgroup_format_proc_cgroups(char *buf, size_t size);

#endif // INCLUDE_CGROUP_H_
