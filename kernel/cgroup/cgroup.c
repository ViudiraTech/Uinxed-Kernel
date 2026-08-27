/*
 *
 *      cgroup.c
 *      Control group (cgroup) unified hierarchy implementation with multi-controller support
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cgroup/cgroup.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/list/circular_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>

typedef struct cgroup {
        char        *name;
        cgroup_t    *parent;
        clist_t      children;
        ilist_node_t tasks;
        uint64_t     subtree_control;

        /* pids controller */
        uint64_t pids_max;
        uint64_t pids_current;
        uint64_t pids_events_max;

        /* memory controller */
        uint64_t memory_max;
        uint64_t memory_high;
        uint64_t memory_low;
        uint64_t memory_current;
        uint64_t memory_swap_max;
        uint64_t memory_swap_current;

        /* cpu controller */
        uint64_t cpu_quota;
        uint64_t cpu_period;
        uint64_t cpu_weight;

        /* io controller */
        uint64_t io_weight;

        /* cpuset controller */
        char cpuset_cpus[64];
        char cpuset_mems[64];

        /* state */
        int      frozen;
        uint32_t refcount;
        int      dying;
} cgroup_t;

static cgroup_t   root_cgroup;
static spinlock_t cgroup_lock;
static int        cgroup_ready;
static uint64_t   registered_controllers;

/* Parse an unsigned decimal string, rejecting trailing garbage */
static int parse_u64(const char *value, size_t size, uint64_t *result)
{
    uint64_t n = 0;
    size_t   i = 0;

    while (i < size && (value[i] == ' ' || value[i] == '\t')) i++;
    if (i == size || value[i] < '0' || value[i] > '9') return -EINVAL;
    for (; i < size && value[i] >= '0' && value[i] <= '9'; i++) {
        uint64_t digit = (uint64_t)(value[i] - '0');
        if (n > (UINT64_MAX - digit) / 10) return -ERANGE;
        n = n * 10 + digit;
    }
    while (i < size && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n')) i++;
    if (i != size) return -EINVAL;
    *result = n;
    return EOK;
}

/* Return whether the given cgroup is an ancestor of the other */
static int is_descendant(cgroup_t *cgroup, cgroup_t *ancestor)
{
    for (; cgroup; cgroup = cgroup->parent)
        if (cgroup == ancestor) return 1;
    return 0;
}

/* Whether a controller is usable on this cgroup (root is always enabled) */
static int controller_available_locked(cgroup_t *cg, uint64_t flag)
{
    return cg == &root_cgroup || (cg->parent->subtree_control & flag);
}

int cgroup_controller_available(cgroup_t *cg, uint64_t flag)
{
    int available;
    if (!cg) return 0;
    spin_lock(&cgroup_lock);
    available = !cg->dying && controller_available_locked(cg, flag);
    spin_unlock(&cgroup_lock);
    return available;
}

int cgroup_pids_available(cgroup_t *cg)
{
    return cgroup_controller_available(cg, CGROUP_CONTROLLER_PIDS);
}

/* Count one pids.max exhaustion event on this cgroup */
static void record_pids_max_event_locked(cgroup_t *cg)
{
    cg->pids_events_max++;
}

/* Charge one task to the cgroup and its ancestors, enforcing pids.max */
static int charge_locked(cgroup_t *cgroup)
{
    for (cgroup_t *cg = cgroup; cg; cg = cg->parent) {
        if (controller_available_locked(cg, CGROUP_CONTROLLER_PIDS) && cg->pids_max != CGROUP_PIDS_MAX && cg->pids_current >= cg->pids_max) {
            record_pids_max_event_locked(cg);
            return -EAGAIN;
        }
    }
    for (cgroup_t *cg = cgroup; cg; cg = cg->parent) cg->pids_current++;
    return EOK;
}

/* Release one task's charge from the cgroup and its ancestors */
static void uncharge_locked(cgroup_t *cgroup)
{
    for (cgroup_t *cg = cgroup; cg; cg = cg->parent)
        if (cg->pids_current) cg->pids_current--;
}

/* Find a task by PID, descending into child cgroups */
static task_t *find_task_locked(cgroup_t *cg, uint64_t pid)
{
    for (ilist_node_t *node = cg->tasks.next; node != &cg->tasks; node = node->next) {
        task_t *task = (task_t *)((uint8_t *)node - offsetof(task_t, cgroup_node));
        if (task->pid == pid) return task;
    }
    for (clist_t n = cg->children; n; n = n->next) {
        task_t *task = find_task_locked(n->data, pid);
        if (task) return task;
    }
    return NULL;
}

/* Move a task into a cgroup, charging and enforcing pids.max on the way */
static int attach_task_locked(cgroup_t *target, task_t *task)
{
    cgroup_t *old = task->cgroup;

    if (!old) return -ESRCH;
    if (old == target) return EOK;

    for (cgroup_t *cg = target; cg && !is_descendant(old, cg); cg = cg->parent) {
        if (controller_available_locked(cg, CGROUP_CONTROLLER_PIDS) && cg->pids_max != CGROUP_PIDS_MAX && cg->pids_current >= cg->pids_max) {
            record_pids_max_event_locked(cg);
            return -EAGAIN;
        }
    }

    uncharge_locked(old);
    ilist_remove(&task->cgroup_node);
    task->cgroup = target;
    ilist_insert_before(&target->tasks, &task->cgroup_node);
    for (cgroup_t *cg = target; cg; cg = cg->parent) cg->pids_current++;
    return EOK;
}

/* Initialize the cgroup unified hierarchy */
void cgroup_init(void)
{
#if CONFIG_CGROUP
    memset(&root_cgroup, 0, sizeof(root_cgroup));
    root_cgroup.name = strdup("");
    if (!root_cgroup.name) {
        plogk("cgroup: root name alloc failed.\n");
        return;
    }
    root_cgroup.pids_max       = CGROUP_PIDS_MAX;
    root_cgroup.memory_max     = CGROUP_MEMORY_MAX;
    root_cgroup.memory_high    = CGROUP_MEMORY_MAX;
    root_cgroup.memory_swap_max= CGROUP_MEMORY_MAX;
    root_cgroup.cpu_weight     = 100;
    root_cgroup.io_weight      = 100;
    strncpy(root_cgroup.cpuset_cpus, "0-63", sizeof(root_cgroup.cpuset_cpus) - 1);
    strncpy(root_cgroup.cpuset_mems, "0", sizeof(root_cgroup.cpuset_mems) - 1);
    root_cgroup.refcount       = 1;
    ilist_init(&root_cgroup.tasks);
    cgroup_lock.lock = 0;
    cgroup_ready     = 1;

    cgroup_register_controller("pids", CGROUP_CONTROLLER_PIDS);
    cgroup_register_controller("memory", CGROUP_CONTROLLER_MEMORY);
    cgroup_register_controller("cpu", CGROUP_CONTROLLER_CPU);
    cgroup_register_controller("io", CGROUP_CONTROLLER_IO);
    cgroup_register_controller("cpuset", CGROUP_CONTROLLER_CPUSET);

    plogk("cgroup: Unified hierarchy initialized with multi-controller support.\n");
#endif
}

/* Register a controller */
int cgroup_register_controller(const char *name, uint64_t id)
{
    if (!name || !name[0] || id == 0 || (id & (id - 1))) return -EINVAL;
    spin_lock(&cgroup_lock);
    if (registered_controllers & id) {
        spin_unlock(&cgroup_lock);
        return -EEXIST;
    }
    registered_controllers |= id;
    spin_unlock(&cgroup_lock);
    return EOK;
}

/* Return the root cgroup once initialized */
cgroup_t *cgroup_root(void)
{
    return cgroup_ready ? &root_cgroup : NULL;
}

/* Take a reference on a cgroup, refusing to resurrect a dying one */
cgroup_t *cgroup_get(cgroup_t *cg)
{
    if (!cg) return NULL;
    if (cg == &root_cgroup) return cg;
    spin_lock(&cgroup_lock);
    if (cg->dying)
        cg = NULL;
    else
        cg->refcount++;
    spin_unlock(&cgroup_lock);
    return cg;
}

/* Drop a cgroup reference, freeing it once the last dying reference is gone */
void cgroup_put(cgroup_t *cg)
{
    int release = 0;
    if (!cg || cg == &root_cgroup) return;
    spin_lock(&cgroup_lock);
    if (cg->refcount && --cg->refcount == 0 && cg->dying) release = 1;
    spin_unlock(&cgroup_lock);
    if (release) {
        free(cg->name);
        free(cg);
    }
}

/* Attach a newly forked task to its parent's cgroup */
int cgroup_task_fork(task_t *task, task_t *parent)
{
    cgroup_t *target;
    int       status;

    if (!task || !cgroup_ready) return EOK;
    target = parent && parent->cgroup ? parent->cgroup : &root_cgroup;
    spin_lock(&cgroup_lock);
    status = charge_locked(target);
    if (status == EOK) {
        task->cgroup = target;
        ilist_insert_before(&target->tasks, &task->cgroup_node);
    }
    spin_unlock(&cgroup_lock);
    return status;
}

/* Detach a task from its cgroup as it exits */
void cgroup_task_exit(task_t *task)
{
    if (!task || !task->cgroup || !cgroup_ready) return;
    spin_lock(&cgroup_lock);
    if (task->cgroup) {
        uncharge_locked(task->cgroup);
        ilist_remove(&task->cgroup_node);
        task->cgroup = NULL;
    }
    spin_unlock(&cgroup_lock);
}

/* Move an existing task into the target cgroup */
int cgroup_attach_task(cgroup_t *target, task_t *task)
{
    int status;

    if (!target || !task) return -EINVAL;
    spin_lock(&cgroup_lock);
    status = attach_task_locked(target, task);
    spin_unlock(&cgroup_lock);
    return status;
}

/* Create a child cgroup under parent, rejecting duplicate names */
int cgroup_create(cgroup_t *parent, const char *name, cgroup_t **result)
{
    cgroup_t *cg;

    if (!parent || !name || !name[0] || !result || strchr(name, '/')) return -EINVAL;
    cg = calloc(1, sizeof(*cg));
    if (!cg) return -ENOMEM;
    cg->name = strdup(name);
    if (!cg->name) {
        free(cg);
        return -ENOMEM;
    }
    cg->parent          = parent;
    cg->pids_max        = CGROUP_PIDS_MAX;
    cg->memory_max      = CGROUP_MEMORY_MAX;
    cg->memory_high     = CGROUP_MEMORY_MAX;
    cg->memory_swap_max = CGROUP_MEMORY_MAX;
    cg->cpu_weight      = 100;
    cg->io_weight       = 100;
    strncpy(cg->cpuset_cpus, parent->cpuset_cpus, sizeof(cg->cpuset_cpus) - 1);
    strncpy(cg->cpuset_mems, parent->cpuset_mems, sizeof(cg->cpuset_mems) - 1);
    cg->refcount        = 1;
    ilist_init(&cg->tasks);

    spin_lock(&cgroup_lock);
    for (clist_t n = parent->children; n; n = n->next) {
        cgroup_t *child = n->data;
        if (streq(child->name, name)) {
            spin_unlock(&cgroup_lock);
            free(cg->name);
            free(cg);
            return -EEXIST;
        }
    }
    parent->children = clist_append(parent->children, cg);
    if (!clist_search(parent->children, cg)) {
        spin_unlock(&cgroup_lock);
        free(cg->name);
        free(cg);
        return -ENOMEM;
    }
    spin_unlock(&cgroup_lock);
    *result = cg;
    return EOK;
}

/* Destroy an empty cgroup, refusing to remove the root or a busy subtree */
int cgroup_destroy(cgroup_t *cg)
{
    int release;
    if (!cg || cg == &root_cgroup) return -EBUSY;
    spin_lock(&cgroup_lock);
    if (cg->children || !ilist_is_empty(&cg->tasks)) {
        spin_unlock(&cgroup_lock);
        return -EBUSY;
    }
    cg->parent->children = clist_delete(cg->parent->children, cg);
    cg->dying            = 1;
    release              = --cg->refcount == 0;
    spin_unlock(&cgroup_lock);
    if (release) {
        free(cg->name);
        free(cg);
    }
    return EOK;
}

/* Parse controller mask from control operation token like "+pids", "-memory", etc. */
static int parse_controller_flag(const char *name, size_t len, uint64_t *flag)
{
    if (len == 4 && !strncmp(name, "pids", 4)) {
        *flag = CGROUP_CONTROLLER_PIDS;
        return 0;
    }
    if (len == 6 && !strncmp(name, "memory", 6)) {
        *flag = CGROUP_CONTROLLER_MEMORY;
        return 0;
    }
    if (len == 3 && !strncmp(name, "cpu", 3)) {
        *flag = CGROUP_CONTROLLER_CPU;
        return 0;
    }
    if (len == 2 && !strncmp(name, "io", 2)) {
        *flag = CGROUP_CONTROLLER_IO;
        return 0;
    }
    if (len == 6 && !strncmp(name, "cpuset", 6)) {
        *flag = CGROUP_CONTROLLER_CPUSET;
        return 0;
    }
    return -EINVAL;
}

/* Apply a whitespace-separated list of +controller/-controller control operations */
int cgroup_set_subtree_control(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t next;
    size_t   i = 0;

    if (!cg || !value || !size) return -EINVAL;
    spin_lock(&cgroup_lock);
    next = cg->subtree_control;
    while (i < size) {
        char op;
        while (i < size && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n')) i++;
        if (i == size) break;
        op = value[i++];
        if (op != '+' && op != '-') {
            spin_unlock(&cgroup_lock);
            return -EINVAL;
        }
        size_t start = i;
        while (i < size && value[i] != ' ' && value[i] != '\t' && value[i] != '\n') i++;
        if (start == i) {
            spin_unlock(&cgroup_lock);
            return -EINVAL;
        }
        uint64_t ctrl_flag = 0;
        if (parse_controller_flag(value + start, i - start, &ctrl_flag) != 0) {
            spin_unlock(&cgroup_lock);
            return -EINVAL;
        }

        if (op == '+') {
            uint64_t available = cg == &root_cgroup ? registered_controllers : cg->parent->subtree_control;
            if (!(available & ctrl_flag)) {
                spin_unlock(&cgroup_lock);
                return -ENOENT;
            }
            if (cg != &root_cgroup && !ilist_is_empty(&cg->tasks)) {
                spin_unlock(&cgroup_lock);
                return -EBUSY;
            }
            next |= ctrl_flag;
        } else {
            for (clist_t n = cg->children; n; n = n->next) {
                cgroup_t *child = n->data;
                if (child->subtree_control & ctrl_flag) {
                    spin_unlock(&cgroup_lock);
                    return -EBUSY;
                }
            }
            next &= ~ctrl_flag;
        }
    }
    cg->subtree_control = next;
    spin_unlock(&cgroup_lock);
    return EOK;
}

/* Helper to kill tasks recursively in a cgroup subtree */
static void kill_subtree_locked(cgroup_t *cg)
{
    for (ilist_node_t *n = cg->tasks.next; n != &cg->tasks; n = n->next) {
        task_t *task = (task_t *)((uint8_t *)n - offsetof(task_t, cgroup_node));
        if (task->process && task->pid > 1) {
            signal_send(task->process, SIGKILL, NULL);
        }
    }
    for (clist_t n = cg->children; n; n = n->next) {
        kill_subtree_locked(n->data);
    }
}

/* Kill all tasks in this cgroup and its descendants */
int cgroup_kill(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t val = 0;
    int status = parse_u64(value, size, &val);
    if (status != EOK) return status;
    if (val != 1) return -EINVAL;

    spin_lock(&cgroup_lock);
    kill_subtree_locked(cg);
    spin_unlock(&cgroup_lock);
    return EOK;
}

/* Freeze or unfreeze a cgroup subtree */
int cgroup_set_freeze(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t val = 0;
    int status = parse_u64(value, size, &val);
    if (status != EOK) return status;
    if (val > 1) return -EINVAL;

    spin_lock(&cgroup_lock);
    cg->frozen = (int)val;
    spin_unlock(&cgroup_lock);
    return EOK;
}

/* Set pids.max */
int cgroup_set_pids_max(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t limit;
    int      status;

    if (!cg || !value || !size || !cgroup_pids_available(cg)) return -EOPNOTSUPP;
    if (size >= 3 && !memcmp(value, "max", 3)) {
        size_t i = 3;
        while (i < size && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n')) i++;
        if (i != size) return -EINVAL;
        limit = CGROUP_PIDS_MAX;
    } else {
        status = parse_u64(value, size, &limit);
        if (status != EOK) return status;
    }
    spin_lock(&cgroup_lock);
    cg->pids_max = limit;
    spin_unlock(&cgroup_lock);
    return EOK;
}

/* Set memory.max */
int cgroup_set_memory_max(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t limit;
    int      status;

    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    if (size >= 3 && !memcmp(value, "max", 3)) {
        limit = CGROUP_MEMORY_MAX;
    } else {
        status = parse_u64(value, size, &limit);
        if (status != EOK) return status;
    }
    spin_lock(&cgroup_lock);
    cg->memory_max = limit;
    spin_unlock(&cgroup_lock);
    return EOK;
}

int cgroup_set_memory_high(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t limit;
    int      status;

    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    if (size >= 3 && !memcmp(value, "max", 3)) {
        limit = CGROUP_MEMORY_MAX;
    } else {
        status = parse_u64(value, size, &limit);
        if (status != EOK) return status;
    }
    spin_lock(&cgroup_lock);
    cg->memory_high = limit;
    spin_unlock(&cgroup_lock);
    return EOK;
}

int cgroup_set_memory_low(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t limit;
    int      status;

    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    status = parse_u64(value, size, &limit);
    if (status != EOK) return status;
    spin_lock(&cgroup_lock);
    cg->memory_low = limit;
    spin_unlock(&cgroup_lock);
    return EOK;
}

int cgroup_set_cpu_max(cgroup_t *cg, const char *value, size_t size)
{
    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_CPU)) return -EOPNOTSUPP;
    uint64_t quota = CGROUP_PIDS_MAX, period = 100000;
    if (size >= 3 && !memcmp(value, "max", 3)) {
        size_t i = 3;
        while (i < size && (value[i] == ' ' || value[i] == '\t')) i++;
        if (i < size) parse_u64(value + i, size - i, &period);
    } else {
        parse_u64(value, size, &quota);
    }
    spin_lock(&cgroup_lock);
    cg->cpu_quota  = quota;
    cg->cpu_period = period;
    spin_unlock(&cgroup_lock);
    return EOK;
}

int cgroup_set_cpu_weight(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t weight;
    int      status;

    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_CPU)) return -EOPNOTSUPP;
    status = parse_u64(value, size, &weight);
    if (status != EOK) return status;
    spin_lock(&cgroup_lock);
    cg->cpu_weight = weight;
    spin_unlock(&cgroup_lock);
    return EOK;
}

int cgroup_set_io_max(cgroup_t *cg, const char *value, size_t size)
{
    (void)value;
    (void)size;
    if (!cg || !cgroup_controller_available(cg, CGROUP_CONTROLLER_IO)) return -EOPNOTSUPP;
    return EOK;
}

int cgroup_set_io_weight(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t weight;
    int      status;

    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_IO)) return -EOPNOTSUPP;
    status = parse_u64(value, size, &weight);
    if (status != EOK) return status;
    spin_lock(&cgroup_lock);
    cg->io_weight = weight;
    spin_unlock(&cgroup_lock);
    return EOK;
}

int cgroup_set_cpuset_cpus(cgroup_t *cg, const char *value, size_t size)
{
    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_CPUSET)) return -EOPNOTSUPP;
    spin_lock(&cgroup_lock);
    size_t len = size < sizeof(cg->cpuset_cpus) - 1 ? size : sizeof(cg->cpuset_cpus) - 1;
    memcpy(cg->cpuset_cpus, value, len);
    cg->cpuset_cpus[len] = '\0';
    spin_unlock(&cgroup_lock);
    return EOK;
}

int cgroup_set_cpuset_mems(cgroup_t *cg, const char *value, size_t size)
{
    if (!cg || !value || !size || !cgroup_controller_available(cg, CGROUP_CONTROLLER_CPUSET)) return -EOPNOTSUPP;
    spin_lock(&cgroup_lock);
    size_t len = size < sizeof(cg->cpuset_mems) - 1 ? size : sizeof(cg->cpuset_mems) - 1;
    memcpy(cg->cpuset_mems, value, len);
    cg->cpuset_mems[len] = '\0';
    spin_unlock(&cgroup_lock);
    return EOK;
}

/* Move the task with the given PID (or current task if 0) into this cgroup */
int cgroup_move_pid(cgroup_t *cg, const char *value, size_t size)
{
    uint64_t pid;
    int      status = parse_u64(value, size, &pid);
    task_t  *task;

    if (status != EOK) return status;
    spin_lock(&cgroup_lock);
    task = pid == 0 ? current_task() : find_task_locked(&root_cgroup, pid);
    if (!task || task->state == TASK_ZOMBIE) {
        status = -ESRCH;
    } else {
        status = attach_task_locked(cg, task);
    }
    spin_unlock(&cgroup_lock);
    return status;
}

/* Return a cgroup's parent */
cgroup_t *cgroup_parent(cgroup_t *cg)
{
    return cg ? cg->parent : NULL;
}

/* Return a cgroup's name */
const char *cgroup_name(cgroup_t *cg)
{
    return cg ? cg->name : NULL;
}

/* Return a cgroup's subtree controller mask */
uint64_t cgroup_subtree_control(cgroup_t *cg)
{
    return cg ? cg->subtree_control : 0;
}

/* Whether the given cgroup is the root */
int cgroup_is_root(cgroup_t *cg)
{
    return cg == &root_cgroup;
}

/* Format the cgroup.controllers file for this cgroup */
int cgroup_show_controllers(cgroup_t *cg, char *buf, size_t size)
{
    uint64_t available = cg == &root_cgroup ? registered_controllers : cg->parent->subtree_control;
    size_t   at        = 0;

    if (available & CGROUP_CONTROLLER_PIDS) at += snprintf(buf + at, size > at ? size - at : 0, "pids ");
    if (available & CGROUP_CONTROLLER_MEMORY) at += snprintf(buf + at, size > at ? size - at : 0, "memory ");
    if (available & CGROUP_CONTROLLER_CPU) at += snprintf(buf + at, size > at ? size - at : 0, "cpu ");
    if (available & CGROUP_CONTROLLER_IO) at += snprintf(buf + at, size > at ? size - at : 0, "io ");
    if (available & CGROUP_CONTROLLER_CPUSET) at += snprintf(buf + at, size > at ? size - at : 0, "cpuset ");
    if (at > 0 && buf[at - 1] == ' ') buf[at - 1] = '\n';
    else if (at == 0 && size > 0) buf[0] = '\0';

    return (int)at;
}

/* Format the cgroup.subtree_control file for this cgroup */
int cgroup_show_subtree_control(cgroup_t *cg, char *buf, size_t size)
{
    uint64_t enabled = cg->subtree_control;
    size_t   at      = 0;

    if (enabled & CGROUP_CONTROLLER_PIDS) at += snprintf(buf + at, size > at ? size - at : 0, "pids ");
    if (enabled & CGROUP_CONTROLLER_MEMORY) at += snprintf(buf + at, size > at ? size - at : 0, "memory ");
    if (enabled & CGROUP_CONTROLLER_CPU) at += snprintf(buf + at, size > at ? size - at : 0, "cpu ");
    if (enabled & CGROUP_CONTROLLER_IO) at += snprintf(buf + at, size > at ? size - at : 0, "io ");
    if (enabled & CGROUP_CONTROLLER_CPUSET) at += snprintf(buf + at, size > at ? size - at : 0, "cpuset ");
    if (at > 0 && buf[at - 1] == ' ') buf[at - 1] = '\n';
    else if (at == 0 && size > 0) buf[0] = '\0';

    return (int)at;
}

/* Format the cgroup.procs file, listing the PIDs of member tasks */
int cgroup_show_procs(cgroup_t *cg, char *buf, size_t size)
{
    size_t at = 0;
    spin_lock(&cgroup_lock);
    for (ilist_node_t *n = cg->tasks.next; n != &cg->tasks; n = n->next) {
        task_t *task = (task_t *)((uint8_t *)n - offsetof(task_t, cgroup_node));
        if (at >= size) break;
        int written = snprintf(buf + at, size - at, "%llu\n", task->pid);
        if (written < 0) break;
        if ((size_t)written >= size - at) {
            at = size;
            break;
        }
        at += (size_t)written;
    }
    spin_unlock(&cgroup_lock);
    return (int)(at < size ? at : size);
}

int cgroup_show_threads(cgroup_t *cg, char *buf, size_t size)
{
    return cgroup_show_procs(cg, buf, size);
}

/* Format the cgroup.events file for this cgroup */
int cgroup_show_events(cgroup_t *cg, char *buf, size_t size)
{
    int populated, frozen;
    spin_lock(&cgroup_lock);
    populated = cg->pids_current != 0;
    frozen    = cg->frozen;
    spin_unlock(&cgroup_lock);
    return snprintf(buf, size, "populated %d\nfrozen %d\n", populated, frozen);
}

int cgroup_show_stat(cgroup_t *cg, char *buf, size_t size)
{
    uint64_t nr_descendants = 0, nr_dying_descendants = 0;
    spin_lock(&cgroup_lock);
    for (clist_t n = cg->children; n; n = n->next) nr_descendants++;
    spin_unlock(&cgroup_lock);
    return snprintf(buf, size, "nr_descendants %llu\nnr_dying_descendants %llu\n", nr_descendants, nr_dying_descendants);
}

int cgroup_show_max_descendants(cgroup_t *cg, char *buf, size_t size)
{
    (void)cg;
    return snprintf(buf, size, "max\n");
}

int cgroup_show_max_depth(cgroup_t *cg, char *buf, size_t size)
{
    (void)cg;
    return snprintf(buf, size, "max\n");
}

/* pids controller formatting */
int cgroup_show_pids_current(cgroup_t *cg, char *buf, size_t size)
{
    uint64_t current;
    if (!cgroup_pids_available(cg)) return -EOPNOTSUPP;
    spin_lock(&cgroup_lock);
    current = cg->pids_current;
    spin_unlock(&cgroup_lock);
    return snprintf(buf, size, "%llu\n", current);
}

int cgroup_show_pids_max(cgroup_t *cg, char *buf, size_t size)
{
    uint64_t limit;
    if (!cgroup_pids_available(cg)) return -EOPNOTSUPP;
    spin_lock(&cgroup_lock);
    limit = cg->pids_max;
    spin_unlock(&cgroup_lock);
    return limit == CGROUP_PIDS_MAX ? snprintf(buf, size, "max\n") : snprintf(buf, size, "%llu\n", limit);
}

int cgroup_show_pids_events(cgroup_t *cg, char *buf, size_t size)
{
    uint64_t events;
    if (!cgroup_pids_available(cg)) return -EOPNOTSUPP;
    spin_lock(&cgroup_lock);
    events = cg->pids_events_max;
    spin_unlock(&cgroup_lock);
    return snprintf(buf, size, "max %llu\n", events);
}

/* memory controller formatting */
int cgroup_show_memory_current(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return snprintf(buf, size, "0\n");
}

int cgroup_show_memory_max(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return cg->memory_max == CGROUP_MEMORY_MAX ? snprintf(buf, size, "max\n") : snprintf(buf, size, "%llu\n", cg->memory_max);
}

int cgroup_show_memory_high(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return cg->memory_high == CGROUP_MEMORY_MAX ? snprintf(buf, size, "max\n") : snprintf(buf, size, "%llu\n", cg->memory_high);
}

int cgroup_show_memory_low(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return snprintf(buf, size, "%llu\n", cg->memory_low);
}

int cgroup_show_memory_stat(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return snprintf(buf, size, "anon 0\nfile 0\nkernel 0\nslab 0\nsock 0\nshmem 0\nfile_mapped 0\nfile_dirty 0\nfile_writeback 0\n");
}

int cgroup_show_memory_events(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return snprintf(buf, size, "low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\n");
}

int cgroup_show_memory_swap_current(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return snprintf(buf, size, "0\n");
}

int cgroup_show_memory_swap_max(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_MEMORY)) return -EOPNOTSUPP;
    return cg->memory_swap_max == CGROUP_MEMORY_MAX ? snprintf(buf, size, "max\n") : snprintf(buf, size, "%llu\n", cg->memory_swap_max);
}

/* cpu controller formatting */
int cgroup_show_cpu_max(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_CPU)) return -EOPNOTSUPP;
    if (cg->cpu_quota == CGROUP_PIDS_MAX) return snprintf(buf, size, "max %llu\n", cg->cpu_period ? cg->cpu_period : 100000ULL);
    return snprintf(buf, size, "%llu %llu\n", cg->cpu_quota, cg->cpu_period ? cg->cpu_period : 100000ULL);
}

int cgroup_show_cpu_weight(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_CPU)) return -EOPNOTSUPP;
    return snprintf(buf, size, "%llu\n", cg->cpu_weight ? cg->cpu_weight : 100ULL);
}

int cgroup_show_cpu_stat(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_CPU)) return -EOPNOTSUPP;
    return snprintf(buf, size, "usage_usec 0\nuser_usec 0\nsystem_usec 0\nnr_periods 0\nnr_throttled 0\nthrottled_usec 0\n");
}

/* io controller formatting */
int cgroup_show_io_max(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_IO)) return -EOPNOTSUPP;
    return snprintf(buf, size, "");
}

int cgroup_show_io_weight(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_IO)) return -EOPNOTSUPP;
    return snprintf(buf, size, "default %llu\n", cg->io_weight ? cg->io_weight : 100ULL);
}

int cgroup_show_io_stat(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_IO)) return -EOPNOTSUPP;
    return snprintf(buf, size, "");
}

/* cpuset controller formatting */
int cgroup_show_cpuset_cpus(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_CPUSET)) return -EOPNOTSUPP;
    return snprintf(buf, size, "%s\n", cg->cpuset_cpus[0] ? cg->cpuset_cpus : "0-63");
}

int cgroup_show_cpuset_mems(cgroup_t *cg, char *buf, size_t size)
{
    if (!cgroup_controller_available(cg, CGROUP_CONTROLLER_CPUSET)) return -EOPNOTSUPP;
    return snprintf(buf, size, "%s\n", cg->cpuset_mems[0] ? cg->cpuset_mems : "0");
}

/* Format the full path of a cgroup, relative to the root hierarchy */
int cgroup_format_path(cgroup_t *cg, char *buf, size_t size)
{
    if (!cg || !buf || size < 2) return -EINVAL;
    spin_lock(&cgroup_lock);
    if (cg->dying) {
        spin_unlock(&cgroup_lock);
        return -ENOENT;
    }

    size_t at = size - 1;
    buf[at]   = '\0';
    for (cgroup_t *current = cg; current && current != &root_cgroup; current = current->parent) {
        size_t length = strlen(current->name);
        if (!length || length + 1 > at) {
            spin_unlock(&cgroup_lock);
            return -ENAMETOOLONG;
        }
        at -= length;
        memcpy(buf + at, current->name, length);
        buf[--at] = '/';
    }
    if (at == size - 1) buf[--at] = '/';
    size_t length = size - at - 1;
    memmove(buf, buf + at, length + 1);
    spin_unlock(&cgroup_lock);
    return (int)length;
}

/* Count the cgroups in this subtree, including the given one */
static uint64_t cgroup_count_locked(cgroup_t *cg)
{
    uint64_t count = 1;
    for (clist_t child = cg->children; child; child = child->next) count += cgroup_count_locked(child->data);
    return count;
}

/* Format the /proc/cgroups file for the unified hierarchy */
int cgroup_format_proc_cgroups(char *buf, size_t size)
{
    if (!buf || !size) return -EINVAL;
    spin_lock(&cgroup_lock);
    uint64_t count       = cgroup_ready ? cgroup_count_locked(&root_cgroup) : 0;
    uint64_t controllers = registered_controllers;
    spin_unlock(&cgroup_lock);

    size_t at = 0;
    at += snprintf(buf + at, size > at ? size - at : 0, "#subsys_name\thierarchy\tnum_cgroups\tenabled\n");
    if (controllers & CGROUP_CONTROLLER_PIDS) at += snprintf(buf + at, size > at ? size - at : 0, "pids\t0\t%llu\t1\n", count);
    if (controllers & CGROUP_CONTROLLER_MEMORY) at += snprintf(buf + at, size > at ? size - at : 0, "memory\t0\t%llu\t1\n", count);
    if (controllers & CGROUP_CONTROLLER_CPU) at += snprintf(buf + at, size > at ? size - at : 0, "cpu\t0\t%llu\t1\n", count);
    if (controllers & CGROUP_CONTROLLER_IO) at += snprintf(buf + at, size > at ? size - at : 0, "io\t0\t%llu\t1\n", count);
    if (controllers & CGROUP_CONTROLLER_CPUSET) at += snprintf(buf + at, size > at ? size - at : 0, "cpuset\t0\t%llu\t1\n", count);

    return (int)at;
}
