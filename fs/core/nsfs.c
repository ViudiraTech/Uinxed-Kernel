/* Namespace handles pin the actual namespace object independently of a PID. */
#include <fs/core/vfs.h>
#include <fs/core/vfs_stub.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/namespace.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <security/capability.h>

static int nsfs_id;
typedef struct {
    int type;
    nsproxy_t *proxy;
} nsfs_handle_t;

static int namespace_type(const char *name)
{
    if (!strcmp(name, "mnt")) return CLONE_NEWNS;
    if (!strcmp(name, "uts")) return CLONE_NEWUTS;
    if (!strcmp(name, "ipc")) return CLONE_NEWIPC;
    if (!strcmp(name, "pid")) return CLONE_NEWPID;
    if (!strcmp(name, "net")) return CLONE_NEWNET;
    if (!strcmp(name, "user")) return CLONE_NEWUSER;
    if (!strcmp(name, "cgroup")) return CLONE_NEWCGROUP;
    return 0;
}

static ns_common_t *namespace_common(nsproxy_t *p, int type)
{
    if (!p) return NULL;
    switch (type) {
        case CLONE_NEWNS: return &p->mnt_ns->ns;
        case CLONE_NEWUTS: return &p->uts_ns->ns;
        case CLONE_NEWIPC: return &p->ipc_ns->ns;
        case CLONE_NEWPID: return &p->pid_ns->ns;
        case CLONE_NEWNET: return &p->net_ns->ns;
        case CLONE_NEWUSER: return &p->user_ns->ns;
        case CLONE_NEWCGROUP: return &p->cgroup_ns->ns;
        default: return NULL;
    }
}

uint64_t namespace_object_id(process_t *proc, const char *name)
{
    ns_common_t *common = namespace_common(proc ? proc->nsproxy : NULL, namespace_type(name));
    return common ? common->id : 0;
}

static int nsfs_stat(void *file, vfs_node_t node)
{
    nsfs_handle_t *h = file;
    ns_common_t *common = h ? namespace_common(h->proxy, h->type) : NULL;
    if (!common) return -EINVAL;
    node->inode = common->id;
    node->mode = node->permissions = 0444;
    node->type = file_none;
    node->size = 0;
    return EOK;
}

static int nsfs_free(void *file)
{
    nsfs_handle_t *h = file;
    if (!h) return EOK;
    nsproxy_put(h->proxy);
    free(h);
    return EOK;
}

static int nsfs_ioctl(void *file, size_t cmd, void *arg)
{
    nsfs_handle_t *h = file;
    if (!h) return -EINVAL;
    if (cmd == 0xb703) return h->type; // NS_GET_NSTYPE
    if (cmd == 0xb704 && h->type == CLONE_NEWUSER) { // NS_GET_OWNER_UID
        uint32_t uid = user_ns_unmap_id(user_namespace_current(), h->proxy->user_ns->owner_uid, false);
        return copy_to_user(arg, &uid, sizeof(uid)) ? -EFAULT : EOK;
    }
    return -ENOTTY;
}

static struct vfs_callback nsfs_callbacks = {
    .open = vfs_stub_open,
    .close = vfs_stub_close,
    .free = nsfs_free,
    .stat = nsfs_stat,
    .read = vfs_stub_read,
    .write = vfs_stub_write,
    .ioctl = nsfs_ioctl,
    .poll = vfs_poll_ready,
};

void namespace_fs_init(void)
{
    nsfs_id = vfs_regist_fs_flags(NULL, &nsfs_callbacks, VFS_FS_NODEV);
    if (nsfs_id > 0) (void)vfs_set_filesystem_magic((uint16_t)nsfs_id, 0x6e736673U);
}

vfs_node_t namespace_open_handle(process_t *proc, const char *name)
{
    int type = namespace_type(name);
    if (!proc || !proc->nsproxy || !type || nsfs_id <= 0) return NULL;
    nsfs_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->type = type;
    h->proxy = nsproxy_get(proc->nsproxy);
    vfs_node_t node = vfs_node_alloc(NULL, "[namespace]");
    if (!node) { nsfs_free(h); return NULL; }
    node->handle = h;
    node->fsid = nsfs_id;
    node->flags |= VFS_NODE_NOCACHE;
    node->refcount = 1;
    (void)nsfs_stat(h, node);
    return node;
}

/* setns replaces only the requested namespace, preserving unrelated identities. */
int namespace_setns(int fd, int nstype)
{
    task_t *task = current_task();
    process_t *proc = process_current();
    if (!task || !proc) return -ESRCH;
    process_file_t *pf = process_fd_get(proc, fd);
    if (!pf) return -EBADF;
    if (!pf->node || pf->node->fsid != nsfs_id) { process_file_put(pf); return -EINVAL; }
    nsfs_handle_t *h = pf->node->handle;
    ns_common_t *common = h ? namespace_common(h->proxy, h->type) : NULL;
    if (!common || (nstype && nstype != h->type)) { process_file_put(pf); return -EINVAL; }
    /* Entering a namespace needs CAP_SYS_ADMIN in its owner user namespace. */
    user_namespace_t *owner = h->type == CLONE_NEWUSER ? h->proxy->user_ns : common->owner;
    if (!owner) owner = &init_user_ns;
    if (!capability_ns(task, owner, CAP_SYS_ADMIN)) { process_file_put(pf); return -EPERM; }
    if (proc->thread_count != 1) { process_file_put(pf); return -EINVAL; }
    if (h->type == CLONE_NEWUSER) {
        user_namespace_t *own = user_namespace_current();
        user_namespace_t *target = h->proxy->user_ns;
        bool descendant = false;
        for (user_namespace_t *ns = target; ns; ns = ns->parent) if (ns == own) descendant = true;
        if (own == target || !descendant || !capability_ns(task, target, CAP_SYS_ADMIN)) { process_file_put(pf); return -EPERM; }
    }
    nsproxy_t *p = calloc(1, sizeof(*p));
    if (!p) { process_file_put(pf); return -ENOMEM; }
    nsproxy_t *old = task->nsproxy;
    p->ns.refcount = 1;
    p->uts_ns = uts_ns_get(h->type == CLONE_NEWUTS ? h->proxy->uts_ns : old->uts_ns);
    p->ipc_ns = ipc_ns_get(h->type == CLONE_NEWIPC ? h->proxy->ipc_ns : old->ipc_ns);
    p->mnt_ns = mnt_ns_get(h->type == CLONE_NEWNS ? h->proxy->mnt_ns : old->mnt_ns);
    p->pid_ns = pid_ns_get(h->type == CLONE_NEWPID ? h->proxy->pid_ns : old->pid_ns);
    p->net_ns = net_ns_get(h->type == CLONE_NEWNET ? h->proxy->net_ns : old->net_ns);
    p->user_ns = user_ns_get(h->type == CLONE_NEWUSER ? h->proxy->user_ns : old->user_ns);
    p->cgroup_ns = cgroup_ns_get(h->type == CLONE_NEWCGROUP ? h->proxy->cgroup_ns : old->cgroup_ns);
    bool entering_user = h->type == CLONE_NEWUSER;
    task->nsproxy = proc->nsproxy = p;
    process_file_put(pf);
    if (entering_user) capability_enter_userns(task);
    nsproxy_put(old);
    return EOK;
}
