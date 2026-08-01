/*
 *
 *      cgroup.h
 *      Control group (cgroup) unified hierarchy header
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CGROUP_H_
#define INCLUDE_CGROUP_H_

#include <libs/glist/intrusive_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

struct task;
typedef struct cgroup cgroup_t;

#define CGROUP_CONTROLLER_PIDS (1ULL << 0)
#define CGROUP_PIDS_MAX        UINT64_MAX

void      cgroup_init(void);
cgroup_t *cgroup_root(void);
cgroup_t *cgroup_get(cgroup_t *cgroup);
void      cgroup_put(cgroup_t *cgroup);
int       cgroup_register_controller(const char *name, uint64_t id);

int  cgroup_task_fork(struct task *task, struct task *parent);
void cgroup_task_exit(struct task *task);
int  cgroup_attach_task(cgroup_t *cgroup, struct task *task);

int cgroup_create(cgroup_t *parent, const char *name, cgroup_t **result);
int cgroup_destroy(cgroup_t *cgroup);
int cgroup_set_subtree_control(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_set_pids_max(cgroup_t *cgroup, const char *value, size_t size);
int cgroup_move_pid(cgroup_t *cgroup, const char *value, size_t size);

cgroup_t   *cgroup_parent(cgroup_t *cgroup);
const char *cgroup_name(cgroup_t *cgroup);
uint64_t    cgroup_subtree_control(cgroup_t *cgroup);
int         cgroup_pids_available(cgroup_t *cgroup);
int         cgroup_is_root(cgroup_t *cgroup);

int cgroup_show_controllers(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_subtree_control(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_procs(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_events(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_pids_current(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_pids_max(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_show_pids_events(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_format_path(cgroup_t *cgroup, char *buf, size_t size);
int cgroup_format_proc_cgroups(char *buf, size_t size);

#endif
