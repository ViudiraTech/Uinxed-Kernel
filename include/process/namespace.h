/*
 *
 *      namespace.h
 *      Namespace Architecture
 *
 *      2026/8/27 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NAMESPACE_H_
#define INCLUDE_NAMESPACE_H_

#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>
#include <syscall/syscall.h>

#define UID_GID_MAP_MAX 5 // Max entries in a uid/gid map

struct task;
struct process;
struct cgroup;
struct user_namespace;

/* Refcount and lock shared by every namespace. */
typedef struct ns_common {
        uint64_t   id;
        struct user_namespace *owner;
        uint32_t   refcount;
        spinlock_t lock;
} ns_common_t;

/* UTS Namespace */
typedef struct uts_namespace {
        char        nodename[65];
        char        domainname[65];
        ns_common_t ns;
} uts_namespace_t;

/* IPC Namespace */
typedef struct ipc_namespace {
        ns_common_t ns;
        void       *sysv_ids;
} ipc_namespace_t;

/* Mount Namespace */
typedef struct mnt_namespace {
        uint64_t    id;
        ns_common_t ns;
        void       *root_mount;
} mnt_namespace_t;

/* PID Namespace */
typedef struct pid_namespace {
        uint64_t              level;
        struct pid_namespace *parent;
        uint64_t              pid_max;
        uint64_t              next_pid;
        uint32_t             *pid_refs; // task, process-group and session ID reservations
        ns_common_t           ns;
        struct process       *child_reaper;
        bool                  dead;
} pid_namespace_t;

/* Network Namespace */
typedef struct net_namespace {
        ns_common_t ns;
        void       *loopback_dev;
} net_namespace_t;

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
        ns_common_t            ns;
} user_namespace_t;

/* Cgroup Namespace */
typedef struct cgroup_namespace {
        struct cgroup *root_cgroup;
        ns_common_t    ns;
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
        ns_common_t         ns;
} nsproxy_t;

extern nsproxy_t          init_nsproxy;
extern uts_namespace_t    init_uts_ns;
extern ipc_namespace_t    init_ipc_ns;
extern mnt_namespace_t    init_mnt_ns;
extern pid_namespace_t    init_pid_ns;
extern net_namespace_t    init_net_ns;
extern user_namespace_t   init_user_ns;
extern cgroup_namespace_t init_cgroup_ns;

/* Initialize namespace subsystem */
void namespace_init(void);

/* Return the UTS namespace of the current process, or the initial one outside process context */
uts_namespace_t *uts_namespace_current(void);
ipc_namespace_t *ipc_namespace_current(void);
net_namespace_t *net_namespace_current(void);
user_namespace_t *user_namespace_current(void);
mnt_namespace_t *mnt_namespace_current(void);
uint32_t user_ns_map_id(user_namespace_t *ns, uint32_t id, bool gid);
uint32_t user_ns_unmap_id(user_namespace_t *ns, uint32_t id, bool gid);
bool namespace_initial_root(const struct process *proc);

/* Allocate an init nsproxy */
nsproxy_t *nsproxy_get(nsproxy_t *ns);
void       nsproxy_put(nsproxy_t *ns);

/* Clone nsproxy based on clone_flags */
nsproxy_t *nsproxy_clone(nsproxy_t *orig, uint64_t flags, int *error);

/* Unshare specified namespaces for current task */
int namespace_unshare(uint64_t unshare_flags);

/* Switch namespace via fd */
int namespace_setns(int fd, int nstype);
struct vfs_node;
void namespace_fs_init(void);
struct vfs_node *namespace_open_handle(struct process *proc, const char *name);
uint64_t namespace_object_id(struct process *proc, const char *name);

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

#endif // INCLUDE_NAMESPACE_H_
