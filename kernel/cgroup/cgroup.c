/*
 *
 *      cgroup.c
 *      Control group (cgroup) unified hierarchy implementation with PID controller
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cgroup/cgroup.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/glist/circular_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <sync/spin_lock.h>

typedef struct cgroup {
        char        *name;
        cgroup_t    *parent;
        clist_t      children;
        ilist_node_t tasks;
        uint64_t     subtree_control;
        uint64_t     pids_max;
        uint64_t     pids_current;
        uint64_t     pids_events_max;
        uint32_t     refcount;
        int          dying;
} cgroup_t;

static cgroup_t   root_cgroup;
static spinlock_t cgroup_lock;
static int        cgroup_ready;
static uint64_t   registered_controllers;

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

static int is_descendant(cgroup_t *cgroup, cgroup_t *ancestor)
{
    for (; cgroup; cgroup = cgroup->parent)
        if (cgroup == ancestor) return 1;
    return 0;
}

static int pids_available_locked(cgroup_t *cg)
{
    return cg == &root_cgroup || (cg->parent->subtree_control & CGROUP_CONTROLLER_PIDS);
}

static void record_pids_max_event_locked(cgroup_t *cg)
{
    cg->pids_events_max++;
}

static int charge_locked(cgroup_t *cgroup)
{
    for (cgroup_t *cg = cgroup; cg; cg = cg->parent) {
        if (pids_available_locked(cg) && cg->pids_max != CGROUP_PIDS_MAX && cg->pids_current >= cg->pids_max) {
            record_pids_max_event_locked(cg);
            return -EAGAIN;
        }
    }
    for (cgroup_t *cg = cgroup; cg; cg = cg->parent) cg->pids_current++;
    return EOK;
}

static void uncharge_locked(cgroup_t *cgroup)
{
    for (cgroup_t *cg = cgroup; cg; cg = cg->parent)
        if (cg->pids_current) cg->pids_current--;
}

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

static int attach_task_locked(cgroup_t *target, task_t *task)
{
    cgroup_t *old = task->cgroup;

    if (!old) return -ESRCH;
    if (old == target) return EOK;

    for (cgroup_t *cg = target; cg && !is_descendant(old, cg); cg = cg->parent) {
        if (pids_available_locked(cg) && cg->pids_max != CGROUP_PIDS_MAX && cg->pids_current >= cg->pids_max) {
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

void cgroup_init(void)
{
#if CONFIG_CGROUP
    memset(&root_cgroup, 0, sizeof(root_cgroup));
    root_cgroup.name     = strdup("");
    root_cgroup.pids_max = CGROUP_PIDS_MAX;
    root_cgroup.refcount = 1;
    ilist_init(&root_cgroup.tasks);
    cgroup_lock.lock = 0;
    cgroup_ready     = 1;
    if (cgroup_register_controller("pids", CGROUP_CONTROLLER_PIDS) != EOK) {
        plogk("cgroup: failed to register pids controller\n");
        return;
    }
    plogk("cgroup: unified hierarchy initialized with pids controller\n");
#endif
}

int cgroup_register_controller(const char *name, uint64_t id)
{
    if (!name || !name[0] || id == 0 || (id & (id - 1))) return -EINVAL;
    if (!streq(name, "pids") || id != CGROUP_CONTROLLER_PIDS) return -EOPNOTSUPP;
    spin_lock(&cgroup_lock);
    if (registered_controllers & id) {
        spin_unlock(&cgroup_lock);
        return -EEXIST;
    }
    registered_controllers |= id;
    spin_unlock(&cgroup_lock);
    return EOK;
}

cgroup_t *cgroup_root(void)
{
    return cgroup_ready ? &root_cgroup : NULL;
}

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

int cgroup_attach_task(cgroup_t *target, task_t *task)
{
    int status;

    if (!target || !task) return -EINVAL;
    spin_lock(&cgroup_lock);
    status = attach_task_locked(target, task);
    spin_unlock(&cgroup_lock);
    return status;
}

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
    cg->parent   = parent;
    cg->pids_max = CGROUP_PIDS_MAX;
    cg->refcount = 1;
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
        if ((op != '+' && op != '-') || i + 4 > size || memcmp(value + i, "pids", 4)) {
            spin_unlock(&cgroup_lock);
            return -EINVAL;
        }
        i += 4;
        if (i < size && value[i] != ' ' && value[i] != '\t' && value[i] != '\n') {
            spin_unlock(&cgroup_lock);
            return -EINVAL;
        }
        if (op == '+') {
            uint64_t available = cg == &root_cgroup ? registered_controllers : cg->parent->subtree_control;
            if (!(available & CGROUP_CONTROLLER_PIDS)) {
                spin_unlock(&cgroup_lock);
                return -ENOENT;
            }
            if (cg != &root_cgroup && !ilist_is_empty(&cg->tasks)) {
                spin_unlock(&cgroup_lock);
                return -EBUSY;
            }
            next |= CGROUP_CONTROLLER_PIDS;
        } else {
            for (clist_t n = cg->children; n; n = n->next) {
                cgroup_t *child = n->data;
                if (child->subtree_control & CGROUP_CONTROLLER_PIDS) {
                    spin_unlock(&cgroup_lock);
                    return -EBUSY;
                }
            }
            next &= ~CGROUP_CONTROLLER_PIDS;
        }
    }
    cg->subtree_control = next;
    spin_unlock(&cgroup_lock);
    return EOK;
}

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

cgroup_t *cgroup_parent(cgroup_t *cg)
{
    return cg ? cg->parent : NULL;
}
const char *cgroup_name(cgroup_t *cg)
{
    return cg ? cg->name : NULL;
}
uint64_t cgroup_subtree_control(cgroup_t *cg)
{
    return cg ? cg->subtree_control : 0;
}

int cgroup_pids_available(cgroup_t *cg)
{
    int available;
    if (!cg) return 0;
    spin_lock(&cgroup_lock);
    available = !cg->dying && pids_available_locked(cg);
    spin_unlock(&cgroup_lock);
    return available;
}

int cgroup_is_root(cgroup_t *cg)
{
    return cg == &root_cgroup;
}

int cgroup_show_controllers(cgroup_t *cg, char *buf, size_t size)
{
    uint64_t available = cg == &root_cgroup ? registered_controllers : cg->parent->subtree_control;
    return snprintf(buf, size, "%s", available & CGROUP_CONTROLLER_PIDS ? "pids\n" : "");
}

int cgroup_show_subtree_control(cgroup_t *cg, char *buf, size_t size)
{
    return snprintf(buf, size, "%s", cg->subtree_control & CGROUP_CONTROLLER_PIDS ? "pids\n" : "");
}

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

int cgroup_show_events(cgroup_t *cg, char *buf, size_t size)
{
    int populated;
    spin_lock(&cgroup_lock);
    populated = cg->pids_current != 0;
    spin_unlock(&cgroup_lock);
    return snprintf(buf, size, "populated %d\n", populated);
}

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

static uint64_t cgroup_count_locked(cgroup_t *cg)
{
    uint64_t count = 1;
    for (clist_t child = cg->children; child; child = child->next) count += cgroup_count_locked(child->data);
    return count;
}

int cgroup_format_proc_cgroups(char *buf, size_t size)
{
    if (!buf || !size) return -EINVAL;
    spin_lock(&cgroup_lock);
    uint64_t count       = cgroup_ready ? cgroup_count_locked(&root_cgroup) : 0;
    uint64_t controllers = registered_controllers;
    spin_unlock(&cgroup_lock);

    int length
        = snprintf(buf, size, "#subsys_name\thierarchy\tnum_cgroups\tenabled\n%s", (controllers & CGROUP_CONTROLLER_PIDS) ? "pids\t0\t" : "");
    if (!(controllers & CGROUP_CONTROLLER_PIDS) || length < 0 || (size_t)length >= size) return length;
    int tail = snprintf(buf + length, size - (size_t)length, "%llu\t1\n", count);
    if (tail < 0) return tail;
    return length + tail;
}
