/*
 *
 *      namespace.c
 *      Namespace Architecture Implementation
 *
 *      2026/8/27 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cgroup/cgroup.h>
#include <fs/core/vfs.h>
#include <ipc/sysv_ipc.h>
#include <ipc/posix_mq.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/namespace.h>
#include <process/process.h>
#include <process/sched.h>
#include <security/capability.h>

uts_namespace_t init_uts_ns = {
    .nodename   = "(none)",
    .domainname = "(none)",
    .ns         = {.id = 4026531838ULL, .owner = &init_user_ns, .refcount = 1, .lock = {.lock = 0}},
};

ipc_namespace_t init_ipc_ns = {
    .ns       = {.id = 4026531839ULL, .owner = &init_user_ns, .refcount = 1, .lock = {.lock = 0}},
    .sysv_ids = NULL,
};

mnt_namespace_t init_mnt_ns = {
    .id         = 1,
    .ns         = {.id = 4026531840ULL, .owner = &init_user_ns, .refcount = 1, .lock = {.lock = 0}},
    .root_mount = NULL,
};

pid_namespace_t init_pid_ns = {
    .level        = 0,
    .parent       = NULL,
    .pid_max      = 32768,
    .next_pid     = 1,
    .ns           = {.id = 4026531841ULL, .owner = &init_user_ns, .refcount = 1, .lock = {.lock = 0}},
    .child_reaper = NULL,
    .dead         = false,
};

net_namespace_t init_net_ns = {
    .ns           = {.id = 4026531842ULL, .owner = &init_user_ns, .refcount = 1, .lock = {.lock = 0}},
    .loopback_dev = NULL,
};

user_namespace_t init_user_ns = {
    .parent            = NULL,
    .owner_uid         = 0,
    .owner_gid         = 0,
    .uid_extent_count  = 1,
    .uid_map           = {{.first = 0, .lower_first = 0, .count = 4294967295U}},
    .gid_extent_count  = 1,
    .gid_map           = {{.first = 0, .lower_first = 0, .count = 4294967295U}},
    .setgroups_allowed = true,
    .ns                = {.id = 4026531843ULL, .owner = &init_user_ns, .refcount = 1, .lock = {.lock = 0}},
};

cgroup_namespace_t init_cgroup_ns = {
    .root_cgroup = NULL,
    .ns          = {.id = 4026531844ULL, .owner = &init_user_ns, .refcount = 1, .lock = {.lock = 0}},
};

nsproxy_t init_nsproxy = {
    .uts_ns    = &init_uts_ns,
    .ipc_ns    = &init_ipc_ns,
    .mnt_ns    = &init_mnt_ns,
    .pid_ns    = &init_pid_ns,
    .net_ns    = &init_net_ns,
    .user_ns   = &init_user_ns,
    .cgroup_ns = &init_cgroup_ns,
    .ns        = {.refcount = 1, .lock = {.lock = 0}},
};

static uint64_t next_mnt_id = 2;
static uint64_t next_ns_id = 4026531850ULL;

/* Namespace init. */
void namespace_init(void)
{
    init_cgroup_ns.root_cgroup = cgroup_root();
    plogk("namespace: 7 Linux namespaces initialized.\n");
}

/* Return the UTS namespace of the current process, or the initial one outside process context. */
uts_namespace_t *uts_namespace_current(void)
{
    task_t *task = current_task();
    if (task && task->nsproxy && task->nsproxy->uts_ns) return task->nsproxy->uts_ns;
    return &init_uts_ns;
}

/* Kernel workers and early bootstrap use the initial namespace explicitly. */
ipc_namespace_t *ipc_namespace_current(void)
{
    task_t *task = current_task();
    return task && task->nsproxy ? task->nsproxy->ipc_ns : &init_ipc_ns;
}

net_namespace_t *net_namespace_current(void)
{
    task_t *task = current_task();
    return task && task->nsproxy ? task->nsproxy->net_ns : &init_net_ns;
}

user_namespace_t *user_namespace_current(void)
{
    task_t *task = current_task();
    return task && task->nsproxy ? task->nsproxy->user_ns : &init_user_ns;
}

mnt_namespace_t *mnt_namespace_current(void)
{
    task_t *task = current_task();
    return task && task->nsproxy ? task->nsproxy->mnt_ns : &init_mnt_ns;
}

/* Legacy global privilege checks must not treat container UID 0 as host root. */
bool namespace_initial_root(const process_t *proc)
{
    return proc && proc->uid == 0 && (!proc->nsproxy || proc->nsproxy->user_ns == &init_user_ns);
}

/* Convert a namespace ID to the kernel's initial-user-namespace ID. */
uint32_t user_ns_map_id(user_namespace_t *ns, uint32_t id, bool gid)
{
    if (!ns || ns == &init_user_ns) return id;
    spin_lock(&ns->ns.lock);
    uint32_t count = gid ? ns->gid_extent_count : ns->uid_extent_count;
    uid_gid_extent_t *map = gid ? ns->gid_map : ns->uid_map;
    uint32_t result = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (id >= map[i].first && id - map[i].first < map[i].count) {
            result = map[i].lower_first + id - map[i].first;
            break;
        }
    }
    spin_unlock(&ns->ns.lock);
    return result == UINT32_MAX ? result : user_ns_map_id(ns->parent, result, gid);
}

/* Convert a kernel ID for presentation, using overflow IDs for unmapped owners. */
static uint32_t user_ns_unmap_raw(user_namespace_t *ns, uint32_t id, bool gid)
{
    if (!ns || ns == &init_user_ns) return id;
    uint32_t parent_id = user_ns_unmap_raw(ns->parent, id, gid);
    if (parent_id == UINT32_MAX) return UINT32_MAX;
    spin_lock(&ns->ns.lock);
    uint32_t count = gid ? ns->gid_extent_count : ns->uid_extent_count;
    uid_gid_extent_t *map = gid ? ns->gid_map : ns->uid_map;
    uint32_t result = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (parent_id >= map[i].lower_first && parent_id - map[i].lower_first < map[i].count) {
            result = map[i].first + parent_id - map[i].lower_first;
            break;
        }
    }
    spin_unlock(&ns->ns.lock);
    return result;
}

uint32_t user_ns_unmap_id(user_namespace_t *ns, uint32_t id, bool gid)
{
    uint32_t result = user_ns_unmap_raw(ns, id, gid);
    return result == UINT32_MAX ? 65534 : result;
}

/* Take a reference on a namespace. */
static void ns_ref_get(ns_common_t *common)
{
    spin_lock(&common->lock);
    common->refcount++;
    spin_unlock(&common->lock);
}

/* Drop a reference on a namespace; true when the last one is gone. */
static bool ns_ref_put(ns_common_t *common)
{
    bool release = false;
    spin_lock(&common->lock);
    if (--common->refcount == 0) release = true;
    spin_unlock(&common->lock);
    return release;
}

/* Nsproxy get. */
nsproxy_t *nsproxy_get(nsproxy_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Nsproxy put. */
void nsproxy_put(nsproxy_t *ns)
{
    if (!ns || ns == &init_nsproxy) return;
    if (!ns_ref_put(&ns->ns)) return;

    uts_ns_put(ns->uts_ns);
    ipc_ns_put(ns->ipc_ns);
    mnt_ns_put(ns->mnt_ns);
    pid_ns_put(ns->pid_ns);
    net_ns_put(ns->net_ns);
    user_ns_put(ns->user_ns);
    cgroup_ns_put(ns->cgroup_ns);
    free(ns);
}

/* Individual namespace refcount handlers */

/* Uts ns get. */
uts_namespace_t *uts_ns_get(uts_namespace_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Uts ns put. */
void uts_ns_put(uts_namespace_t *ns)
{
    if (!ns || ns == &init_uts_ns) return;
    if (ns_ref_put(&ns->ns)) { user_ns_put(ns->ns.owner); free(ns); }
}

/* IPC ns get. */
ipc_namespace_t *ipc_ns_get(ipc_namespace_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* IPC ns put. */
void ipc_ns_put(ipc_namespace_t *ns)
{
    if (!ns || ns == &init_ipc_ns) return;
    if (ns_ref_put(&ns->ns)) {
        sysv_ipc_namespace_destroy(ns);
        posix_mq_namespace_destroy(ns);
        user_ns_put(ns->ns.owner);
        free(ns);
    }
}

/* Mnt ns get. */
mnt_namespace_t *mnt_ns_get(mnt_namespace_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Mnt ns put. */
void mnt_ns_put(mnt_namespace_t *ns)
{
    if (!ns || ns == &init_mnt_ns) return;
    if (ns_ref_put(&ns->ns)) {
        vfs_mntns_destroy(ns);
        user_ns_put(ns->ns.owner);
        free(ns);
    }
}

/* Pid ns get. */
pid_namespace_t *pid_ns_get(pid_namespace_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Pid ns put. */
void pid_ns_put(pid_namespace_t *ns)
{
    if (!ns || ns == &init_pid_ns) return;
    if (ns_ref_put(&ns->ns)) { pid_ns_put(ns->parent); user_ns_put(ns->ns.owner); free(ns); }
}

/* Network helper: ns get. */
net_namespace_t *net_ns_get(net_namespace_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Network helper: ns put. */
void net_ns_put(net_namespace_t *ns)
{
    if (!ns || ns == &init_net_ns) return;
    if (ns_ref_put(&ns->ns)) { user_ns_put(ns->ns.owner); free(ns); }
}

/* User ns get. */
user_namespace_t *user_ns_get(user_namespace_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* User ns put. */
void user_ns_put(user_namespace_t *ns)
{
    if (!ns || ns == &init_user_ns) return;
    if (ns_ref_put(&ns->ns)) { user_ns_put(ns->parent); free(ns); }
}

/* cgroup ns get. */
cgroup_namespace_t *cgroup_ns_get(cgroup_namespace_t *ns)
{
    if (!ns) return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* cgroup ns put. */
void cgroup_ns_put(cgroup_namespace_t *ns)
{
    if (!ns || ns == &init_cgroup_ns) return;
    if (ns_ref_put(&ns->ns)) { cgroup_put(ns->root_cgroup); user_ns_put(ns->ns.owner); free(ns); }
}

/* Create new namespaces */
static uts_namespace_t *clone_uts_ns(uts_namespace_t *old)
{
    uts_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->ns.refcount = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);
    if (old) {
        spin_lock(&old->ns.lock);
        memcpy(ns->nodename, old->nodename, sizeof(ns->nodename));
        memcpy(ns->domainname, old->domainname, sizeof(ns->domainname));
        spin_unlock(&old->ns.lock);
    } else {
        strncpy(ns->nodename, "(none)", sizeof(ns->nodename) - 1);
        strncpy(ns->domainname, "(none)", sizeof(ns->domainname) - 1);
    }
    return ns;
}

/* Clone a ipc ns namespace. */
static ipc_namespace_t *clone_ipc_ns(ipc_namespace_t *old)
{
    (void)old;
    ipc_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->ns.refcount = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);
    return ns;
}

/* Clone a mnt ns namespace. */
static mnt_namespace_t *clone_mnt_ns(mnt_namespace_t *old, user_namespace_t *owner)
{
    mnt_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->id          = next_mnt_id++;
    ns->ns.refcount = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);
    ns->ns.owner = user_ns_get(owner);
    /* A new mount namespace copies the current one; an empty table would hide
     * every mount, /proc and /sys included. */
    if (vfs_mntns_clone(old, ns) != EOK) {
        user_ns_put(ns->ns.owner);
        free(ns);
        return NULL;
    }
    return ns;
}

/* Clone a pid ns namespace. */
static pid_namespace_t *clone_pid_ns(pid_namespace_t *old)
{
    /*
     * Nesting is capped at creation, as Linux's create_pid_namespace() does:
     * every pid_numbers/tgid_numbers/pgid_numbers array is PID_NS_MAX_LEVEL + 1
     * entries and is indexed by a namespace's level, so a deeper namespace would
     * index past the end of them.
     */
    if (old && old->level >= PID_NS_MAX_LEVEL) return NULL;
    pid_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->level       = old ? old->level + 1 : 1;
    ns->parent      = pid_ns_get(old);
    ns->pid_max     = 32768;
    ns->next_pid    = 1;
    ns->ns.refcount = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);
    return ns;
}

/* Clone a net ns namespace. */
static net_namespace_t *clone_net_ns(net_namespace_t *old)
{
    (void)old;
    net_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->ns.refcount = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);
    return ns;
}

/* Clone a user ns namespace. */
static user_namespace_t *clone_user_ns(user_namespace_t *old, process_t *owner)
{
    user_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->parent            = user_ns_get(old);
    ns->ns.owner          = old;
    ns->owner_uid         = owner ? owner->uid : 0;
    ns->owner_gid         = owner ? owner->gid : 0;
    ns->setgroups_allowed = true;
    ns->ns.refcount       = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);
    return ns;
}

/* Clone a cgroup ns namespace. */
static cgroup_namespace_t *clone_cgroup_ns(cgroup_namespace_t *old, task_t *task)
{
    cgroup_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->root_cgroup = (task && task->cgroup) ? cgroup_get(task->cgroup) : cgroup_get(old ? old->root_cgroup : cgroup_root());
    ns->ns.refcount = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);
    return ns;
}

/* Clone nsproxy */
nsproxy_t *nsproxy_clone(nsproxy_t *orig, uint64_t flags, int *error)
{
    if (!orig) orig = &init_nsproxy;
    uint64_t mask = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET;
    if ((flags & mask & ~CLONE_NEWUSER) && !(flags & CLONE_NEWUSER)
        && !capability_ns(current_task(), orig->user_ns, CAP_SYS_ADMIN)) {
        *error = -EPERM;
        return NULL;
    }
    if (!(flags & (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET))) {
        *error = EOK;
        return nsproxy_get(orig);
    }

    nsproxy_t *ns = calloc(1, sizeof(*ns));
    if (!ns) {
        *error = -ENOMEM;
        return NULL;
    }
    ns->ns.refcount = 1;
    ns->ns.id = __atomic_fetch_add(&next_ns_id, 1, __ATOMIC_RELAXED);

    task_t    *curr_t = current_task();
    process_t *curr_p = process_current();
    ns->user_ns = flags & CLONE_NEWUSER ? clone_user_ns(orig->user_ns, curr_p) : user_ns_get(orig->user_ns);
    if (!ns->user_ns) { free(ns); *error = -ENOMEM; return NULL; }

    if (flags & CLONE_NEWUTS) {
        ns->uts_ns = clone_uts_ns(orig->uts_ns);
    } else {
        ns->uts_ns = uts_ns_get(orig->uts_ns);
    }

    if (flags & CLONE_NEWIPC) {
        ns->ipc_ns = clone_ipc_ns(orig->ipc_ns);
    } else {
        ns->ipc_ns = ipc_ns_get(orig->ipc_ns);
    }

    if (flags & CLONE_NEWNS) {
        ns->mnt_ns = clone_mnt_ns(orig->mnt_ns, ns->user_ns);
    } else {
        ns->mnt_ns = mnt_ns_get(orig->mnt_ns);
    }

    if (flags & CLONE_NEWPID) {
        ns->pid_ns = clone_pid_ns(orig->pid_ns);
    } else {
        ns->pid_ns = pid_ns_get(orig->pid_ns);
    }

    if (flags & CLONE_NEWNET) {
        ns->net_ns = clone_net_ns(orig->net_ns);
    } else {
        ns->net_ns = net_ns_get(orig->net_ns);
    }


    if (flags & CLONE_NEWCGROUP) {
        ns->cgroup_ns = clone_cgroup_ns(orig->cgroup_ns, curr_t);
    } else {
        ns->cgroup_ns = cgroup_ns_get(orig->cgroup_ns);
    }

    if (!ns->uts_ns || !ns->ipc_ns || !ns->mnt_ns || !ns->pid_ns || !ns->net_ns || !ns->user_ns || !ns->cgroup_ns) {
        nsproxy_put(ns);
        *error = -ENOMEM;
        return NULL;
    }

    if (flags & CLONE_NEWUTS) ns->uts_ns->ns.owner = user_ns_get(ns->user_ns);
    if (flags & CLONE_NEWIPC) ns->ipc_ns->ns.owner = user_ns_get(ns->user_ns);
    if (flags & CLONE_NEWPID) ns->pid_ns->ns.owner = user_ns_get(ns->user_ns);
    if (flags & CLONE_NEWNET) ns->net_ns->ns.owner = user_ns_get(ns->user_ns);
    if (flags & CLONE_NEWCGROUP) ns->cgroup_ns->ns.owner = user_ns_get(ns->user_ns);
    *error = EOK;
    return ns;
}

/* Unshare specified namespaces for current task */
int namespace_unshare(uint64_t unshare_flags)
{
    task_t *task = current_task();
    if (!task) return -ESRCH;

    uint64_t supported = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM;
    if (unshare_flags & ~supported) return -EINVAL;
    if (task->process && task->process->thread_count != 1) return -EINVAL;
    int        error  = EOK;
    nsproxy_t *new_ns = nsproxy_clone(task->nsproxy, unshare_flags, &error);
    if (error != EOK) return error;

    nsproxy_t *old_ns = task->nsproxy;
    task->nsproxy     = new_ns;
    if (task->process) task->process->nsproxy = new_ns;
    if (unshare_flags & CLONE_NEWUSER) capability_enter_userns(task);

    nsproxy_put(old_ns);
    return EOK;
}
