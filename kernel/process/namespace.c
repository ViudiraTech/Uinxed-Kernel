/*
 *
 *      namespace.c
 *      Linux-compatible Namespace Architecture Implementation
 *
 *      2026/8/27 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cgroup/cgroup.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/namespace.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>

uts_namespace_t init_uts_ns = {
    .nodename   = "uinxed",
    .domainname = "(none)",
    .refcount   = 1,
    .lock       = {.lock = 0},
};

ipc_namespace_t init_ipc_ns = {
    .refcount = 1,
    .lock     = {.lock = 0},
    .sysv_ids = NULL,
};

mnt_namespace_t init_mnt_ns = {
    .id         = 1,
    .refcount   = 1,
    .lock       = {.lock = 0},
    .root_mount = NULL,
};

pid_namespace_t init_pid_ns = {
    .level        = 0,
    .parent       = NULL,
    .pid_max      = 32768,
    .next_pid     = 1,
    .refcount     = 1,
    .lock         = {.lock = 0},
    .child_reaper = NULL,
    .dead         = false,
};

net_namespace_t init_net_ns = {
    .refcount     = 1,
    .lock         = {.lock = 0},
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
    .refcount          = 1,
    .lock              = {.lock = 0},
};

cgroup_namespace_t init_cgroup_ns = {
    .root_cgroup = NULL,
    .refcount    = 1,
    .lock        = {.lock = 0},
};

nsproxy_t init_nsproxy = {
    .uts_ns    = &init_uts_ns,
    .ipc_ns    = &init_ipc_ns,
    .mnt_ns    = &init_mnt_ns,
    .pid_ns    = &init_pid_ns,
    .net_ns    = &init_net_ns,
    .user_ns   = &init_user_ns,
    .cgroup_ns = &init_cgroup_ns,
    .refcount  = 1,
    .lock      = {.lock = 0},
};

void namespace_init(void)
{
    init_cgroup_ns.root_cgroup = cgroup_root();
    plogk("namespace: 7 Linux namespaces initialized.\n");
}

nsproxy_t *nsproxy_get(nsproxy_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void nsproxy_put(nsproxy_t *ns)
{
    if (!ns || ns == &init_nsproxy) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);

    if (release) {
        uts_ns_put(ns->uts_ns);
        ipc_ns_put(ns->ipc_ns);
        mnt_ns_put(ns->mnt_ns);
        pid_ns_put(ns->pid_ns);
        net_ns_put(ns->net_ns);
        user_ns_put(ns->user_ns);
        cgroup_ns_put(ns->cgroup_ns);
        free(ns);
    }
}

/* Individual namespace refcount handlers */
uts_namespace_t *uts_ns_get(uts_namespace_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void uts_ns_put(uts_namespace_t *ns)
{
    if (!ns || ns == &init_uts_ns) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);
    if (release) free(ns);
}

ipc_namespace_t *ipc_ns_get(ipc_namespace_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void ipc_ns_put(ipc_namespace_t *ns)
{
    if (!ns || ns == &init_ipc_ns) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);
    if (release) free(ns);
}

mnt_namespace_t *mnt_ns_get(mnt_namespace_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void mnt_ns_put(mnt_namespace_t *ns)
{
    if (!ns || ns == &init_mnt_ns) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);
    if (release) free(ns);
}

pid_namespace_t *pid_ns_get(pid_namespace_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void pid_ns_put(pid_namespace_t *ns)
{
    if (!ns || ns == &init_pid_ns) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);
    if (release) free(ns);
}

net_namespace_t *net_ns_get(net_namespace_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void net_ns_put(net_namespace_t *ns)
{
    if (!ns || ns == &init_net_ns) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);
    if (release) free(ns);
}

user_namespace_t *user_ns_get(user_namespace_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void user_ns_put(user_namespace_t *ns)
{
    if (!ns || ns == &init_user_ns) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);
    if (release) free(ns);
}

cgroup_namespace_t *cgroup_ns_get(cgroup_namespace_t *ns)
{
    if (!ns) return NULL;
    spin_lock(&ns->lock);
    ns->refcount++;
    spin_unlock(&ns->lock);
    return ns;
}

void cgroup_ns_put(cgroup_namespace_t *ns)
{
    if (!ns || ns == &init_cgroup_ns) return;
    int release = 0;
    spin_lock(&ns->lock);
    if (--ns->refcount == 0) release = 1;
    spin_unlock(&ns->lock);
    if (release) free(ns);
}

/* Create new namespaces */
static uts_namespace_t *clone_uts_ns(uts_namespace_t *old)
{
    uts_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->refcount = 1;
    if (old) {
        spin_lock(&old->lock);
        memcpy(ns->nodename, old->nodename, sizeof(ns->nodename));
        memcpy(ns->domainname, old->domainname, sizeof(ns->domainname));
        spin_unlock(&old->lock);
    } else {
        strncpy(ns->nodename, "uinxed", sizeof(ns->nodename) - 1);
        strncpy(ns->domainname, "(none)", sizeof(ns->domainname) - 1);
    }
    return ns;
}

static ipc_namespace_t *clone_ipc_ns(ipc_namespace_t *old)
{
    (void)old;
    ipc_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->refcount = 1;
    return ns;
}

static uint64_t next_mnt_id = 2;
static mnt_namespace_t *clone_mnt_ns(mnt_namespace_t *old)
{
    (void)old;
    mnt_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->id       = next_mnt_id++;
    ns->refcount = 1;
    return ns;
}

static pid_namespace_t *clone_pid_ns(pid_namespace_t *old)
{
    pid_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->level    = old ? old->level + 1 : 1;
    ns->parent   = pid_ns_get(old);
    ns->pid_max  = 32768;
    ns->next_pid = 1;
    ns->refcount = 1;
    return ns;
}

static net_namespace_t *clone_net_ns(net_namespace_t *old)
{
    (void)old;
    net_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->refcount = 1;
    return ns;
}

static user_namespace_t *clone_user_ns(user_namespace_t *old, process_t *owner)
{
    user_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->parent            = user_ns_get(old);
    ns->owner_uid         = owner ? owner->uid : 0;
    ns->owner_gid         = owner ? owner->gid : 0;
    ns->setgroups_allowed = true;
    ns->refcount          = 1;
    return ns;
}

static cgroup_namespace_t *clone_cgroup_ns(cgroup_namespace_t *old, task_t *task)
{
    cgroup_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->root_cgroup = (task && task->cgroup) ? cgroup_get(task->cgroup) : cgroup_get(old ? old->root_cgroup : cgroup_root());
    ns->refcount    = 1;
    return ns;
}

/* Clone nsproxy */
nsproxy_t *nsproxy_clone(nsproxy_t *orig, uint64_t flags, int *error)
{
    if (!orig) orig = &init_nsproxy;
    if (!(flags & (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET))) {
        *error = EOK;
        return nsproxy_get(orig);
    }

    nsproxy_t *ns = calloc(1, sizeof(*ns));
    if (!ns) {
        *error = -ENOMEM;
        return NULL;
    }
    ns->refcount = 1;

    task_t    *curr_t = current_task();
    process_t *curr_p = process_current();

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
        ns->mnt_ns = clone_mnt_ns(orig->mnt_ns);
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

    if (flags & CLONE_NEWUSER) {
        ns->user_ns = clone_user_ns(orig->user_ns, curr_p);
    } else {
        ns->user_ns = user_ns_get(orig->user_ns);
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

    *error = EOK;
    return ns;
}

/* Unshare specified namespaces for current task */
int namespace_unshare(uint64_t unshare_flags)
{
    task_t *task = current_task();
    if (!task) return -ESRCH;

    int        error = EOK;
    nsproxy_t *new_ns = nsproxy_clone(task->nsproxy, unshare_flags, &error);
    if (error != EOK) return error;

    nsproxy_t *old_ns = task->nsproxy;
    task->nsproxy = new_ns;
    if (task->process) task->process->nsproxy = new_ns;

    nsproxy_put(old_ns);
    return EOK;
}

/* Switch namespace via fd */
int namespace_setns(int fd, int nstype)
{
    (void)nstype;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *pf = process_fd_get(proc, fd);
    if (!pf) return -EBADF;
    process_file_put(pf);

    return EOK;
}
