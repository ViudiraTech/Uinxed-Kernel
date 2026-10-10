#ifndef INCLUDE_PID_NAMESPACE_H_
#define INCLUDE_PID_NAMESPACE_H_

#include <process/namespace.h>
#include <process/process.h>

pid_namespace_t *pid_ns_current(void);
bool pid_ns_is_descendant(pid_namespace_t *ns, pid_namespace_t *ancestor);
uint64_t task_pid_nr_ns(const task_t *task, pid_namespace_t *ns);
uint64_t task_tgid_nr_ns(const task_t *task, pid_namespace_t *ns);
uint64_t task_pid_nr(const task_t *task);
uint64_t task_tgid_nr(const task_t *task);
/* Return zero when the ID is invisible / does not exist. */
uint64_t pid_global_nr_ns(pid_namespace_t *ns, uint64_t visible_pid);
uint64_t pid_nr_ns(pid_namespace_t *ns, uint64_t global_pid);
task_t *pid_find_task_ns_get(pid_namespace_t *ns, uint64_t visible_pid);
/* Only an unpublished task can be moved. */
int pid_task_set_namespace(task_t *task, pid_namespace_t *ns);
/* Call after installing the child's nsproxy, before exposing its IDs / publishing it. */
int namespace_fork_child(process_t *child, uint64_t flags);
process_t *process_find_ns_get(pid_namespace_t *ns, pid_t visible_pid);
task_t *process_task_find_ns_get(pid_namespace_t *ns, pid_t visible_tid, process_t **owner);
bool process_is_pid_ns_init(const process_t *proc);
uint64_t process_pgid_nr_ns(const process_t *proc, pid_namespace_t *ns);
uint64_t process_sid_nr_ns(const process_t *proc, pid_namespace_t *ns);
void pid_ns_numbers_get(pid_namespace_t *ns, const uint32_t *numbers);
void pid_ns_numbers_put(pid_namespace_t *ns, const uint32_t *numbers);
void pid_namespace_destroy(pid_namespace_t *ns);

#endif
