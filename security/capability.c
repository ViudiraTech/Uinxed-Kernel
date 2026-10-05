/*
 *
 *      capability.c
 *      Capability sets used by service credential setup
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <process/process.h>
#include <process/namespace.h>
#include <security/capability.h>

#define SECBIT_NOROOT               1U
#define SECBIT_NO_SETUID_FIXUP      4U
#define SECBIT_KEEP_CAPS            16U
#define SECBIT_KEEP_CAPS_LOCKED     32U
#define SECBIT_NO_CAP_AMBIENT_RAISE 64U

/* Caller holds cap_lock. Initial tasks use the existing root credential model. */
static void capability_init_locked(task_t *task)
{
    /*
     * Every user ID transition goes through capability_uid_change(), which
     * applies the full real/effective/saved rules; re-deriving them here from
     * the effective ID alone would drop capabilities that rule set out to keep.
     */
    if (task->caps_initialized) return;
    bool root           = task->process && task->process->uid == 0;
    task->cap_effective = task->cap_permitted = root ? CAP_SUPPORTED_MASK : 0;
    task->cap_inheritable = task->cap_ambient = 0;
    task->cap_bounding                        = CAP_SUPPORTED_MASK;
    task->cap_uid                             = task->process ? task->process->uid : 0;
    task->caps_initialized                    = true;
}

void capability_get(task_t *task, uint64_t *effective, uint64_t *permitted, uint64_t *inheritable)
{
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    capability_init_locked(task);
    *effective   = task->cap_effective;
    *permitted   = task->cap_permitted;
    *inheritable = task->cap_inheritable;
    spin_unlock_irqrestore(&task->cap_lock, flags);
}

void capability_get_status(task_t *task, capability_status_t *status)
{
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    capability_init_locked(task);
    status->effective   = task->cap_effective;
    status->permitted   = task->cap_permitted;
    status->inheritable = task->cap_inheritable;
    status->bounding    = task->cap_bounding;
    status->ambient     = task->cap_ambient;
    spin_unlock_irqrestore(&task->cap_lock, flags);
}

static bool capability_effective(task_t *task, unsigned capability)
{
    if (!task || capability > CAP_LAST_SUPPORTED) return false;
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    capability_init_locked(task);
    bool present = (task->cap_effective & (1ULL << capability)) != 0;
    spin_unlock_irqrestore(&task->cap_lock, flags);
    return present;
}

/* Capabilities in a child user namespace never confer authority in its parent. */
bool capability_ns(task_t *task, user_namespace_t *target, unsigned capability)
{
    if (!task || !target || capability > CAP_LAST_SUPPORTED) return false;
    user_namespace_t *own = task->nsproxy ? task->nsproxy->user_ns : &init_user_ns;
    for (user_namespace_t *ns = target; ns; ns = ns->parent) {
        if (ns == own) return capability_effective(task, capability);
        if (ns->parent == own && task->process && task->process->uid == ns->owner_uid) return true;
    }
    return false;
}

bool capability_has(task_t *task, unsigned capability)
{
    return capability_ns(task, &init_user_ns, capability);
}

void capability_enter_userns(task_t *task)
{
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    task->cap_effective = task->cap_permitted = task->cap_bounding = CAP_SUPPORTED_MASK;
    task->cap_inheritable = task->cap_ambient = 0;
    task->securebits = 0;
    task->caps_initialized = true;
    spin_unlock_irqrestore(&task->cap_lock, flags);
}

/* Effective capabilities must be permitted; capset cannot grow permitted. */
int capability_set(task_t *task, uint64_t effective, uint64_t permitted, uint64_t inheritable)
{
    effective &= CAP_SUPPORTED_MASK;
    permitted &= CAP_SUPPORTED_MASK;
    inheritable &= CAP_SUPPORTED_MASK;
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    capability_init_locked(task);
    uint64_t can_inherit = task->cap_inheritable | task->cap_permitted;
    if (task->cap_effective & (1ULL << CAP_SETPCAP)) can_inherit |= task->cap_bounding;
    int result = 0;
    if ((effective & ~permitted) || (permitted & ~task->cap_permitted) || (inheritable & ~can_inherit) || ((inheritable & ~task->cap_inheritable) & ~task->cap_bounding)) {
        result = -EPERM;
    } else {
        task->cap_effective   = effective;
        task->cap_permitted   = permitted;
        task->cap_inheritable = inheritable;
        task->cap_ambient &= permitted & inheritable;
    }
    spin_unlock_irqrestore(&task->cap_lock, flags);
    return result;
}

void capability_inherit(task_t *child, task_t *parent)
{
    if (!parent || !parent->process) return;
    uint64_t flags = spin_lock_irqsave(&parent->cap_lock);
    capability_init_locked(parent);
    child->cap_effective    = parent->cap_effective;
    child->cap_permitted    = parent->cap_permitted;
    child->cap_inheritable  = parent->cap_inheritable;
    child->cap_bounding     = parent->cap_bounding;
    child->cap_ambient      = parent->cap_ambient;
    child->securebits       = parent->securebits;
    child->cap_uid          = parent->cap_uid;
    child->caps_initialized = true;
    spin_unlock_irqrestore(&parent->cap_lock, flags);
}

/*
 * capabilities(7), "Effect of user ID changes on capabilities": the permitted
 * set is only dropped once every one of the real, effective and saved IDs has
 * become nonzero.  A transition that merely raises the effective ID above zero
 * - seteuid(2) on a process whose real ID is still 0, as systemd's
 * connect_journal_socket() does - clears the effective and ambient sets but
 * must leave the permitted set intact, otherwise the matching setuid(0) would
 * have nothing to restore the effective set from.
 */
void capability_uid_change(task_t *task, const uid_set_t *old_ids, const uid_set_t *new_ids)
{
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    capability_init_locked(task);
    uint32_t root_id = user_ns_map_id(task->nsproxy ? task->nsproxy->user_ns : &init_user_ns, 0, false);
    if (!(task->securebits & SECBIT_NO_SETUID_FIXUP)) {
        bool was_root         = old_ids->real == root_id || old_ids->effective == root_id || old_ids->saved == root_id;
        bool now_unprivileged = new_ids->real != root_id && new_ids->effective != root_id && new_ids->saved != root_id;
        if (was_root && now_unprivileged) {
            if (!(task->securebits & SECBIT_KEEP_CAPS)) task->cap_permitted = 0;
            task->cap_effective = 0;
            task->cap_ambient   = 0;
        } else if (old_ids->effective == root_id && new_ids->effective != root_id) {
            task->cap_effective = 0;
        }
        if (old_ids->effective != root_id && new_ids->effective == root_id) task->cap_effective = task->cap_permitted;
    }
    task->cap_uid = new_ids->effective;
    spin_unlock_irqrestore(&task->cap_lock, flags);
}

/* No file capabilities are implemented; ordinary exec preserves only ambient. */
void capability_exec(task_t *task)
{
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    capability_init_locked(task);
    if (task->process->uid == user_ns_map_id(task->nsproxy ? task->nsproxy->user_ns : &init_user_ns, 0, false) && !(task->securebits & SECBIT_NOROOT)) {
        task->cap_permitted = task->cap_bounding | task->cap_inheritable;
        task->cap_effective = task->cap_permitted;
    } else {
        task->cap_permitted = task->cap_effective = task->cap_ambient;
    }
    task->securebits &= (uint8_t)~SECBIT_KEEP_CAPS;
    spin_unlock_irqrestore(&task->cap_lock, flags);
}

/* Linux PR_{GET,SET}_KEEPCAPS, bounding sets and ambient capabilities. */
int64_t capability_prctl(task_t *task, uint64_t option, uint64_t operation, uint64_t capability, uint64_t arg4, uint64_t arg5)
{
    if (option == 47 && (arg4 || arg5)) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&task->cap_lock);
    capability_init_locked(task);
    int64_t result = 0;
    if (option == 7) {
        result = (task->securebits & SECBIT_KEEP_CAPS) != 0;
    } else if (option == 8) {
        if (operation > 1)
            result = -EINVAL;
        else if (task->securebits & SECBIT_KEEP_CAPS_LOCKED)
            result = -EPERM;
        else if (operation)
            task->securebits |= SECBIT_KEEP_CAPS;
        else
            task->securebits &= (uint8_t)~SECBIT_KEEP_CAPS;
    } else if (option == 23 || option == 24) {
        if (operation > CAP_LAST_SUPPORTED)
            result = -EINVAL;
        else if (option == 23)
            result = (task->cap_bounding & (1ULL << operation)) != 0;
        else if (!(task->cap_effective & (1ULL << CAP_SETPCAP)))
            result = -EPERM;
        else
            task->cap_bounding &= ~(1ULL << operation);
    } else if (option == 27) {
        result = task->securebits;
    } else if (option == 28) {
        if (operation & ~0xffULL)
            result = -EINVAL;
        else if (operation != task->securebits && !(task->cap_effective & (1ULL << CAP_SETPCAP)))
            result = -EPERM;
        else {
            for (unsigned bit = 0; bit < 8; bit += 2)
                if ((task->securebits & (2U << bit)) && ((task->securebits ^ operation) & (3U << bit))) result = -EPERM;
            if (!result) task->securebits = (uint8_t)operation;
        }
    } else if (option == 47) {
        if (operation == 4 && !capability)
            task->cap_ambient = 0;
        else if (capability > CAP_LAST_SUPPORTED)
            result = -EINVAL;
        else {
            uint64_t bit = 1ULL << capability;
            if (operation == 1)
                result = (task->cap_ambient & bit) != 0;
            else if (operation == 2) {
                if (!(task->cap_permitted & task->cap_inheritable & bit) || (task->securebits & SECBIT_NO_CAP_AMBIENT_RAISE))
                    result = -EPERM;
                else
                    task->cap_ambient |= bit;
            } else if (operation == 3)
                task->cap_ambient &= ~bit;
            else
                result = -EINVAL;
        }
    } else
        result = -EINVAL;
    spin_unlock_irqrestore(&task->cap_lock, flags);
    return result;
}
