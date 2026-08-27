/*
 *
 *      namespace.h
 *      Linux-compatible Namespace Architecture
 *
 *      2026/8/27 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PROCESS_NAMESPACE_H_
#define INCLUDE_PROCESS_NAMESPACE_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdbool.h>
#include <sync/spin_lock.h>

struct task;
struct process;
struct cgroup;

/* Namespace clone / unshare flags */
#define CLONE_NEWNS      0x00020000ULL /* Mount namespace */
#define CLONE_NEWCGROUP  0x02000000ULL /* Cgroup namespace */
#define CLONE_NEWUTS     0x04000000ULL /* UTS namespace */
#define CLONE_NEWIPC     0x08000000ULL /* IPC namespace */
#define CLONE_NEWUSER    0x10000000ULL /* User namespace */
#define CLONE_NEWPID     0x20000000ULL /* PID namespace */
#define CLONE_NEWNET     0x40000000ULL /* Network namespace */

/* 1. UTS Namespace */
typedef struct uts_namespace {
        char       nodename[65];
        char       domainname[65];
        uint32_t   refcount;
        spinlock_t lock;
} uts_namespace_t;

/* 2. IPC Namespace */
typedef struct ipc_namespace {
        uint32_t   refcount;
        spinlock_t lock;
        void      *sysv_ids;
} ipc_namespace_t;

/* 3. Mount Namespace */
typedef struct mnt_namespace {
        uint64_t   id;
        uint32_t   refcount;
        spinlock_t lock;
        void      *root_mount;
} mnt_namespace_t;

/* 4. PID Namespace */
typedef struct pid_namespace {
        uint64_t               level;
        struct pid_namespace  *parent;
        uint64_t               pid_max;
        uint64_t               next_pid;
        uint32_t               refcount;
        spinlock_t             lock;
        struct process        *child_reaper;
        bool                   dead;
} pid_namespace_t;

/* 5. Network Namespace */
typedef struct net_namespace {
        uint32_t   refcount;
        spinlock_t lock;
        void      *loopback_dev;
} net_namespace_t;

/* 6. User Namespace */
#define UID_GID_MAP_MAX 5

typedef struct uid_gid_extent {
        uint32_t first;
        uint32_t lower_first;
        uint32_t count;
} uid_gid_extent_t;

typedef struct user_namespace {
        struct user_namespace *parent;
        uint32_t               owner_uid;
        uint32_t               owner_gid;
        uint32_t               uid_extent_count;
        uid_gid_extent_t       uid_map[UID_GID_MAP_MAX];
        uint32_t               gid_extent_count;
        uid_gid_extent_t       gid_map[UID_GID_MAP_MAX];
        bool                   setgroups_allowed;
        uint32_t               refcount;
        spinlock_t             lock;
} user_namespace_t;

/* 7. Cgroup Namespace */
typedef struct cgroup_namespace {
        struct cgroup *root_cgroup;
        uint32_t       refcount;
        spinlock_t     lock;
} cgroup_namespace_t;

/* Namespace Proxy grouping all 7 namespaces */
typedef struct nsproxy {
        uts_namespace_t    *uts_ns;
        ipc_namespace_t    *ipc_ns;
        mnt_namespace_t    *mnt_ns;
        pid_namespace_t    *pid_ns;
        net_namespace_t    *net_ns;
        user_namespace_t   *user_ns;
        cgroup_namespace_t *cgroup_ns;
        uint32_t            refcount;
        spinlock_t          lock;
} nsproxy_t;

extern nsproxy_t init_nsproxy;
extern uts_namespace_t init_uts_ns;
extern ipc_namespace_t init_ipc_ns;
extern mnt_namespace_t init_mnt_ns;
extern pid_namespace_t init_pid_ns;
extern net_namespace_t init_net_ns;
extern user_namespace_t init_user_ns;
extern cgroup_namespace_t init_cgroup_ns;

/* Initialize namespace subsystem */
void namespace_init(void);

/* Allocate an init nsproxy */
nsproxy_t *nsproxy_get(nsproxy_t *ns);
void       nsproxy_put(nsproxy_t *ns);

/* Clone nsproxy based on clone_flags */
nsproxy_t *nsproxy_clone(nsproxy_t *orig, uint64_t flags, int *error);

/* Unshare specified namespaces for current task */
int namespace_unshare(uint64_t unshare_flags);

/* Switch namespace via fd */
int namespace_setns(int fd, int nstype);

/* Individual namespace helpers */
uts_namespace_t *uts_ns_get(uts_namespace_t *ns);
void             uts_ns_put(uts_namespace_t *ns);

ipc_namespace_t *ipc_ns_get(ipc_namespace_t *ns);
void             ipc_ns_put(ipc_namespace_t *ns);

mnt_namespace_t *mnt_ns_get(mnt_namespace_t *ns);
void             mnt_ns_put(mnt_namespace_t *ns);

pid_namespace_t *pid_ns_get(pid_namespace_t *ns);
void             pid_ns_put(pid_namespace_t *ns);

net_namespace_t *net_ns_get(net_namespace_t *ns);
void             net_ns_put(net_namespace_t *ns);

user_namespace_t *user_ns_get(user_namespace_t *ns);
void              user_ns_put(user_namespace_t *ns);

cgroup_namespace_t *cgroup_ns_get(cgroup_namespace_t *ns);
void                cgroup_ns_put(cgroup_namespace_t *ns);

#endif // INCLUDE_PROCESS_NAMESPACE_H_
