/*
 *
 *      futex.c
 *      Fast userspace mutex implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <ipc/futex.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/rt_mutex.h>
#include <sync/spin_lock.h>

/* Constants */

#ifndef FUTEX_HASH_BITS
#    define FUTEX_HASH_BITS 8
#endif
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)

#define FUTEX_TICKS_PER_SEC TIMER_HZ
#define FUTEX_NSEC_PER_TICK TIMER_TICK_NS

/* The syscall clock layer may provide the realtime clock in scheduler ticks. */
#ifndef FUTEX_REALTIME_TICKS
__attribute__((weak)) uint64_t futex_realtime_ticks(void)
{
    int64_t ns = timer_realtime_ns();
    return ns > 0 ? (uint64_t)ns / TIMER_TICK_NS : 0;
}
#    define FUTEX_REALTIME_TICKS() futex_realtime_ticks()
#endif

/* FUTEX_WAKE_OP operation codes */
#define FUTEX_OP_SET  0
#define FUTEX_OP_ADD  1
#define FUTEX_OP_OR   2
#define FUTEX_OP_ANDN 3
#define FUTEX_OP_XOR  4

/* FUTEX_WAKE_OP comparison codes */
#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP_CMP_NE 1
#define FUTEX_OP_CMP_LT 2
#define FUTEX_OP_CMP_LE 3
#define FUTEX_OP_CMP_GT 4
#define FUTEX_OP_CMP_GE 5

/* Type definitions */

typedef struct futex_entry {
        uintptr_t           key;
        uint64_t            bitset; // mask/bitset: classic FUTEX_WAIT_BITSET or futex2 mask
        wait_queue_t        wq;
        struct futex_entry *next;
        rt_mutex_t         *pi_mutex; // non-NULL for PI futex entries
} futex_entry_t;

typedef struct futex_bucket {
        futex_entry_t *head;
        spinlock_t     lock;
} futex_bucket_t;

/* Static state */

static futex_bucket_t futex_hash[FUTEX_HASH_SIZE];

/* Internal helpers */

/* Hash a user address into a bucket index. */
static inline uint32_t futex_hash_index(uint32_t *uaddr)
{
    return ((uintptr_t)uaddr >> 2) & (FUTEX_HASH_SIZE - 1);
}

/*
 * Find an entry for uaddr in the given bucket (no creation).
 * Must be called with the bucket lock held.
 * Returns NULL if no entry exists.
 */
static futex_entry_t *futex_find(futex_bucket_t *bucket, uint32_t *uaddr)
{
    uintptr_t key = (uintptr_t)uaddr;
    for (futex_entry_t *entry = bucket->head; entry; entry = entry->next)
        if (entry->key == key) return entry;
    return NULL;
}

/*
 * Find or create an entry for uaddr in the given bucket.
 * Must be called with the bucket lock held.
 * Returns NULL on allocation failure.
 * If the entry already exists, its bitset is OR-ed with the new bitset
 * so that all waiters on the same futex can be woken by a matching wake.
 */
static futex_entry_t *futex_find_or_create(futex_bucket_t *bucket, uint32_t *uaddr, uint64_t bitset)
{
    futex_entry_t *entry = futex_find(bucket, uaddr);
    if (entry) {
        entry->bitset |= bitset;
        return entry;
    }

    entry = (futex_entry_t *)malloc(sizeof(futex_entry_t));
    if (!entry) {
        plogk("futex: Entry allocation failed for %p\n", (void *)uaddr);
        return NULL;
    }

    entry->key      = (uintptr_t)uaddr;
    entry->bitset   = bitset;
    entry->pi_mutex = NULL;
    wait_queue_init(&entry->wq);
    entry->next  = bucket->head;
    bucket->head = entry;

    return entry;
}

static futex_entry_t *futex_find_waiter(futex_bucket_t *bucket, uint32_t *uaddr, uint64_t bitset)
{
    uintptr_t key = (uintptr_t)uaddr;

    for (futex_entry_t *entry = bucket->head; entry; entry = entry->next)
        if (entry->key == key && entry->bitset == bitset && !entry->pi_mutex) return entry;
    return NULL;
}

/* Separate queues per bitset make WAKE_BITSET selection exact. */
static futex_entry_t *futex_create_waiter(futex_bucket_t *bucket, uint32_t *uaddr, uint64_t bitset)
{
    futex_entry_t *entry = futex_find_waiter(bucket, uaddr, bitset);
    if (entry) return entry;

    entry = (futex_entry_t *)malloc(sizeof(futex_entry_t));
    if (!entry) {
        plogk("futex: Waiter allocation failed for %p\n", (void *)uaddr);
        return NULL;
    }

    entry->key      = (uintptr_t)uaddr;
    entry->bitset   = bitset;
    entry->pi_mutex = NULL;
    wait_queue_init(&entry->wq);
    entry->next  = bucket->head;
    bucket->head = entry;
    return entry;
}

/*
 * Remove an entry from the bucket's linked list and free it.
 * Must be called with the bucket lock held.
 */
static void futex_remove_entry_locked(futex_bucket_t *bucket, futex_entry_t *entry)
{
    futex_entry_t **indirect = &bucket->head;

    while (*indirect) {
        if (*indirect == entry) {
            *indirect = entry->next;
            if (entry->pi_mutex) free(entry->pi_mutex);
            free(entry);
            return;
        }
        indirect = &(*indirect)->next;
    }
}

/*
 * Check whether a wait queue is empty.
 * Must be called with the bucket lock held (to prevent concurrent
 * modification of the entry chain), but the wq internal lock is
 * acquired here for the check.
 */
static int futex_entry_empty(futex_entry_t *entry)
{
    int empty;

    spin_lock(&entry->wq.lock);
    empty = ilist_is_empty(&entry->wq.tasks);
    spin_unlock(&entry->wq.lock);

    return empty;
}

/*
 * Try to clean up an entry if its wait queue is empty.
 * Returns 1 if the entry was removed, 0 otherwise.
 * Must be called with the bucket lock held.
 */
static int futex_try_cleanup(futex_bucket_t *bucket, futex_entry_t *entry)
{
    if (!entry) return 0;

    if (futex_entry_empty(entry)) {
        futex_remove_entry_locked(bucket, entry);
        return 1;
    }
    return 0;
}

/* Convert a validated user-space timespec to scheduler ticks. */
typedef struct {
        int64_t tv_sec;
        int64_t tv_nsec;
} futex_timespec_t;

static int futex_read_timespec(uint64_t timeout_ptr, uint64_t *ticks)
{
    futex_timespec_t ts;
    uint64_t         nsec_ticks;

    if (copy_from_user(&ts, (const void *)timeout_ptr, sizeof(ts)) != 0) return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) return -EINVAL;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / FUTEX_TICKS_PER_SEC) return -EINVAL;

    *ticks     = (uint64_t)ts.tv_sec * FUTEX_TICKS_PER_SEC;
    nsec_ticks = ((uint64_t)ts.tv_nsec + FUTEX_NSEC_PER_TICK - 1) / FUTEX_NSEC_PER_TICK;
    if (nsec_ticks > UINT64_MAX - *ticks) return -EINVAL;
    *ticks += nsec_ticks;
    return 0;
}

static uint64_t futex_clock_ticks(int realtime)
{
    if (realtime) return FUTEX_REALTIME_TICKS();
    return sched_ticks();
}

static uint64_t futex_deadline(uint64_t ticks, int absolute, int realtime)
{
    uint64_t now = sched_ticks();

    if (!absolute) return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
    if (!realtime) return ticks;

    uint64_t realtime_now = futex_clock_ticks(1);
    if (ticks <= realtime_now) return now;
    ticks -= realtime_now;
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

/* FUTEX_WAIT / FUTEX_WAIT_BITSET */

/*
 * Wait on a futex.  If *uaddr != val, return -EAGAIN immediately.
 * Otherwise block until woken by futex_wake or until the timeout
 * expires.
 *
 * bitset is the mask of bits that must match for wakeup;
 * FUTEX_BITSET_MATCH_ANY (0xffffffff) matches any wake.
 */
static int futex_wait(uint32_t *uaddr, uint32_t val, uint64_t timeout, uint64_t bitset, int absolute, int realtime)
{
    futex_bucket_t *bucket = &futex_hash[futex_hash_index(uaddr)];
    futex_entry_t  *entry;
    uint32_t        cur_val;
    uint64_t        deadline = 0;
    int             ret;

    if (bitset == 0) return -EINVAL;
    if (timeout) {
        ret = futex_read_timespec(timeout, &deadline);
        if (ret) return ret;
        deadline = futex_deadline(deadline, absolute, realtime);
    }

    spin_lock(&bucket->lock);
    if (copy_from_user(&cur_val, uaddr, sizeof(cur_val)) != 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }
    if (cur_val != val) {
        spin_unlock(&bucket->lock);
        return -EAGAIN;
    }

    entry = futex_create_waiter(bucket, uaddr, bitset);
    if (!entry) {
        spin_unlock(&bucket->lock);
        return -ENOMEM;
    }
    wait_queue_prepare(&entry->wq);
    spin_unlock(&bucket->lock);

    if (timeout)
        ret = wait_queue_wait_timed(&entry->wq, deadline);
    else {
        wait_queue_sleep();
        ret = 0;
    }

    spin_lock(&bucket->lock);
    entry = futex_find_waiter(bucket, uaddr, bitset);
    futex_try_cleanup(bucket, entry);
    spin_unlock(&bucket->lock);
    return ret;
}

/* FUTEX_WAKE / FUTEX_WAKE_BITSET */

/*
 * Wake up to nr_wake waiters on the futex at uaddr.
 * Only wake tasks whose bitset matches the wake bitset.
 * Returns the number of tasks actually woken.
 */
int futex_wake(uint32_t *uaddr, int nr_wake, uint64_t bitset)
{
    futex_bucket_t *bucket = &futex_hash[futex_hash_index(uaddr)];
    futex_entry_t  *entry;
    int             woken = 0;

    if (bitset == 0) return -EINVAL;
    if (nr_wake <= 0) return 0;

    spin_lock(&bucket->lock);

    for (entry = bucket->head; entry; entry = entry->next) {
        if (entry->key != (uintptr_t)uaddr) continue;

        /*
         * bitset filtering: only wake tasks whose bitset
         * overlaps with the wake bitset (Linux semantics).
         */
        if (!(entry->bitset & bitset)) continue;

        while (woken < nr_wake) {
            task_t *task = wait_queue_wake_one(&entry->wq);
            if (!task) break;
            woken++;
        }
    }

    /*
     * Do NOT free empty entries here - a futex_wait caller that was
     * just woken may still hold a stale entry pointer and will access
     * it after re-acquiring the bucket lock.  Cleanup is the
     * responsibility of the final waiter (futex_try_cleanup in
     * futex_wait).
     */

    spin_unlock(&bucket->lock);

    return woken;
}

/* FUTEX_REQUEUE / FUTEX_CMP_REQUEUE */

/*
 * Move a single waiter from wq_src to wq_dst without waking it.
 * Both wait queue internal locks must NOT be held by the caller.
 * The caller must hold the bucket locks to prevent entry freeing.
 */
static int futex_move_waiter(wait_queue_t *wq_src, wait_queue_t *wq_dst)
{
    ilist_node_t *node;
    task_t       *task;

    spin_lock(&wq_src->lock);
    if (ilist_is_empty(&wq_src->tasks)) {
        spin_unlock(&wq_src->lock);
        return 0;
    }

    node = wq_src->tasks.next;
    task = (task_t *)((uint8_t *)node - offsetof(task_t, sched_node));

    ilist_remove(node);
    task->wait_queue = wq_dst;

    spin_lock(&wq_dst->lock);
    ilist_insert_before(&wq_dst->tasks, &task->sched_node);
    spin_unlock(&wq_dst->lock);
    spin_unlock(&wq_src->lock);

    return 1;
}

/*
 * Requeue waiters: wake up to nr_wake from uaddr, then move up to
 * nr_requeue waiters from uaddr to uaddr2 without waking them.
 * Returns the number of tasks woken.
 */
static int futex_requeue(uint32_t *uaddr, int nr_wake, int nr_requeue, uint32_t *uaddr2, uint32_t val3, int cmp_requeue)
{
    futex_bucket_t *bucket1  = &futex_hash[futex_hash_index(uaddr)];
    futex_bucket_t *bucket2  = &futex_hash[futex_hash_index(uaddr2)];
    futex_entry_t  *entry1   = NULL;
    futex_entry_t  *entry2   = NULL;
    int             woken    = 0;
    int             requeued = 0;

    if (nr_wake < 0) nr_wake = 0;
    if (nr_requeue < 0) nr_requeue = 0;

    /* Lock both buckets in address order to avoid deadlock. */
    if (bucket1 < bucket2) {
        spin_lock(&bucket1->lock);
        spin_lock(&bucket2->lock);
    } else if (bucket1 > bucket2) {
        spin_lock(&bucket2->lock);
        spin_lock(&bucket1->lock);
    } else {
        /* Same bucket */
        spin_lock(&bucket1->lock);
    }

    /* Find entry for uaddr */
    for (entry1 = bucket1->head; entry1; entry1 = entry1->next)
        if (entry1->key == (uintptr_t)uaddr) break;

    if (!entry1) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return 0;
    }

    /* For CMP_REQUEUE: check val3 against *uaddr2 */
    if (cmp_requeue) {
        uint32_t cur_val2;

        if (copy_from_user(&cur_val2, uaddr2, sizeof(cur_val2)) != 0) {
            if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
            spin_unlock(&bucket1->lock);
            return -EFAULT;
        }

        if (cur_val2 != val3) {
            if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
            spin_unlock(&bucket1->lock);
            return -EAGAIN;
        }
    }

    /* Wake up to nr_wake tasks */
    while (woken < nr_wake) {
        task_t *task = wait_queue_wake_one(&entry1->wq);
        if (!task) break;
        woken++;
    }

    /* Requeue up to nr_requeue tasks to uaddr2 */
    if (nr_requeue > 0) {
        /* Find or create entry for uaddr2 */
        entry2 = futex_find_or_create(bucket2, uaddr2, FUTEX_BITSET_MATCH_ANY);
        if (!entry2) {
            futex_try_cleanup(bucket1, entry1);
            if (bucket1 != bucket2) {
                futex_try_cleanup(bucket2, entry2);
                spin_unlock(&bucket2->lock);
            }
            spin_unlock(&bucket1->lock);
            return woken > 0 ? woken : -ENOMEM;
        }

        while (requeued < nr_requeue) {
            if (!futex_move_waiter(&entry1->wq, &entry2->wq)) break;
            requeued++;
        }
    }

    /* Clean up empty entries */
    futex_try_cleanup(bucket1, entry1);
    if (bucket1 != bucket2) {
        futex_try_cleanup(bucket2, entry2);
        spin_unlock(&bucket2->lock);
    }
    spin_unlock(&bucket1->lock);

    return woken;
}

/* FUTEX_WAKE_OP */

/*
 * Decode the val3 encoding for FUTEX_WAKE_OP:
 *   bits 28-31: oparg  (4-bit operand for the atomic operation)
 *   bits 24-27: cmp    (4-bit comparison code)
 *   bits 12-15: op     (4-bit operation code)
 *   bits 0-11:  cmparg (12-bit comparison argument)
 */
static uint32_t futex_wake_op_apply(uint32_t old_val, uint32_t op, uint32_t oparg)
{
    switch (op) {
        case FUTEX_OP_SET :
            return oparg;
        case FUTEX_OP_ADD :
            return old_val + oparg;
        case FUTEX_OP_OR :
            return old_val | oparg;
        case FUTEX_OP_ANDN :
            return old_val & ~oparg;
        case FUTEX_OP_XOR :
            return old_val ^ oparg;
        default :
            return old_val;
    }
}

static int futex_wake_op_cmp(uint32_t old_val, uint32_t cmp, uint32_t cmparg)
{
    int32_t s_old = (int32_t)old_val;
    int32_t s_arg = (int32_t)cmparg;

    switch (cmp) {
        case FUTEX_OP_CMP_EQ :
            return old_val == cmparg;
        case FUTEX_OP_CMP_NE :
            return old_val != cmparg;
        case FUTEX_OP_CMP_LT :
            return s_old < s_arg;
        case FUTEX_OP_CMP_LE :
            return s_old <= s_arg;
        case FUTEX_OP_CMP_GT :
            return s_old > s_arg;
        case FUTEX_OP_CMP_GE :
            return s_old >= s_arg;
        default :
            return 0;
    }
}

/*
 * FUTEX_WAKE_OP: atomically apply an operation to uaddr2, then wake
 * waiters on uaddr.  If the comparison condition matches the old value
 * at uaddr2, also wake waiters on uaddr2.
 */
static int futex_wake_op(uint32_t *uaddr, int nr_wake, int nr_wake2, uint32_t *uaddr2, uint32_t val3)
{
    futex_bucket_t *bucket1 = &futex_hash[futex_hash_index(uaddr)];
    futex_bucket_t *bucket2 = &futex_hash[futex_hash_index(uaddr2)];
    futex_entry_t  *entry1  = NULL;
    futex_entry_t  *entry2  = NULL;
    uint32_t        op      = (val3 >> 12) & 0xf;
    uint32_t        cmp     = (val3 >> 24) & 0xf;
    uint32_t        cmparg  = val3 & 0xfff;
    uint32_t        oparg   = (val3 >> 28) & 0xf;
    uint32_t        old_val;
    uint32_t        new_val;
    int             cmp_result;
    int             woken = 0;

    if (nr_wake < 0) nr_wake = 0;
    if (nr_wake2 < 0) nr_wake2 = 0;

    /* Lock both buckets in address order to avoid deadlock. */
    if (bucket1 < bucket2) {
        spin_lock(&bucket1->lock);
        spin_lock(&bucket2->lock);
    } else if (bucket1 > bucket2) {
        spin_lock(&bucket2->lock);
        spin_lock(&bucket1->lock);
    } else {
        spin_lock(&bucket1->lock);
    }

    /* Read-modify-write uaddr2 */
    if (copy_from_user(&old_val, uaddr2, sizeof(old_val)) != 0) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return -EFAULT;
    }

    new_val    = futex_wake_op_apply(old_val, op, oparg);
    cmp_result = futex_wake_op_cmp(old_val, cmp, cmparg);

    if (copy_to_user(uaddr2, &new_val, sizeof(new_val)) != 0) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return -EFAULT;
    }

    /* Wake nr_wake tasks from uaddr */
    for (entry1 = bucket1->head; entry1; entry1 = entry1->next) {
        if (entry1->key != (uintptr_t)uaddr) continue;

        while (woken < nr_wake) {
            task_t *task = wait_queue_wake_one(&entry1->wq);
            if (!task) break;
            woken++;
        }
        break;
    }

    /* If comparison matches, wake nr_wake2 tasks from uaddr2 */
    if (cmp_result && nr_wake2 > 0) {
        for (entry2 = bucket2->head; entry2; entry2 = entry2->next) {
            if (entry2->key != (uintptr_t)uaddr2) continue;

            int woken2 = 0;
            while (woken2 < nr_wake2) {
                task_t *task = wait_queue_wake_one(&entry2->wq);
                if (!task) break;
                woken2++;
            }
            break;
        }
    }

    /* Clean up empty entries */
    futex_try_cleanup(bucket1, entry1);
    if (bucket1 != bucket2) {
        futex_try_cleanup(bucket2, entry2);
        spin_unlock(&bucket2->lock);
    }
    spin_unlock(&bucket1->lock);

    return woken;
}

/* PI futex helpers */

/*
 * Get or create a rt_mutex for the given futex word.
 * Must be called with the bucket lock held.
 */
static rt_mutex_t *futex_get_pi_mutex(futex_bucket_t *bucket, uint32_t *uaddr)
{
    futex_entry_t *entry = futex_find_or_create(bucket, uaddr, FUTEX_BITSET_MATCH_ANY);
    if (!entry) return NULL;

    if (!entry->pi_mutex) {
        entry->pi_mutex = malloc(sizeof(rt_mutex_t));
        if (!entry->pi_mutex) {
            plogk("futex: PI mutex allocation failed for %p\n", (void *)uaddr);
            return NULL;
        }
        rt_mutex_init(entry->pi_mutex, uaddr);
    }

    return entry->pi_mutex;
}

/*
 * FUTEX_LOCK_PI: acquire a PI mutex.
 * Userspace fastpath: cmpxchg(*uaddr, 0, tid) - success.
 * Kernel slowpath (this function): block with priority inheritance.
 */
static int futex_lock_pi(uint32_t *uaddr)
{
    task_t *self = current_task();
    if (!self) return -ESRCH;

    futex_bucket_t *bucket = &futex_hash[futex_hash_index(uaddr)];

    spin_lock(&bucket->lock);

    rt_mutex_t *pi_mutex = futex_get_pi_mutex(bucket, uaddr);
    if (!pi_mutex) {
        spin_unlock(&bucket->lock);
        return -ENOMEM;
    }

    /*
     * Userspace should have attempted cmpxchg first.
     * If the lock is still free, take it now.
     */
    uint32_t cur_val;
    if (copy_from_user(&cur_val, uaddr, sizeof(cur_val)) != 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }

    if ((cur_val & FUTEX_TID_MASK) == 0) {
        uint32_t new_val = (self->pid & FUTEX_TID_MASK);
        if (copy_to_user(uaddr, &new_val, sizeof(new_val)) != 0) {
            spin_unlock(&bucket->lock);
            return -EFAULT;
        }
        pi_mutex->owner      = self;
        pi_mutex->owner_died = 0;
        self->base_weight    = self->weight;
        self->pi_weight      = self->weight;
        spin_unlock(&bucket->lock);
        return EOK;
    }

    /*
     * Lock is contended.  Decode the owner TID from the futex word.
     * Set the FUTEX_WAITERS flag so the unlock path knows to call us.
     */
    uint32_t owner_tid = cur_val & FUTEX_TID_MASK;
    uint32_t new_val   = cur_val | FUTEX_WAITERS;
    if (copy_to_user(uaddr, &new_val, sizeof(new_val)) != 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }

    /* Find the owner task by PID */
    task_t *owner = pid_find_task(owner_tid);

    if (!owner || owner == self) {
        spin_unlock(&bucket->lock);
        return (owner == self) ? -EDEADLK : -ESRCH;
    }

    pi_mutex->owner = owner;

    /* Priority inheritance: add self as waiter, propagate chain */
    self->pi_weight   = self->weight;
    self->base_weight = self->weight;

    pi_waiter_add(self, pi_mutex);

    wait_queue_prepare(&pi_mutex->wq);
    spin_unlock(&bucket->lock);

    task_block();

    if (pi_mutex->owner_died) return -EOWNERDEAD;

    return EOK;
}

/*
 * FUTEX_UNLOCK_PI: release a PI mutex.
 * Userspace fastpath: cmpxchg(*uaddr, tid, 0) - success if no waiters.
 * Kernel slowpath (this function): wake the highest-priority waiter.
 */
static int futex_unlock_pi(uint32_t *uaddr)
{
    task_t *self = current_task();
    if (!self) return -ESRCH;

    futex_bucket_t *bucket = &futex_hash[futex_hash_index(uaddr)];

    spin_lock(&bucket->lock);

    futex_entry_t *entry;
    for (entry = bucket->head; entry; entry = entry->next)
        if (entry->key == (uintptr_t)uaddr) break;

    if (!entry || !entry->pi_mutex) {
        spin_unlock(&bucket->lock);
        return -EPERM;
    }

    rt_mutex_t *pi_mutex = entry->pi_mutex;

    if (pi_mutex->owner != self) {
        spin_unlock(&bucket->lock);
        return -EPERM;
    }

    pi_mutex->owner = NULL;

    rb_node_t *leftmost = rb_first(&pi_mutex->pi_waiters);
    if (leftmost) {
        task_t *next_owner = rb_entry(leftmost, task_t, pi_node);
        rb_erase_augmented(&pi_mutex->pi_waiters, leftmost, pi_waiter_augment, NULL);
        next_owner->blocked_on = NULL;

        pi_mutex->owner = next_owner;

        uint32_t new_val = (next_owner->pid & FUTEX_TID_MASK);
        if (!ilist_is_empty(&pi_mutex->wq.tasks)) new_val |= FUTEX_WAITERS;
        copy_to_user(uaddr, &new_val, sizeof(new_val));

        task_wakeup(next_owner);
    } else {
        uint32_t zero = 0;
        copy_to_user(uaddr, &zero, sizeof(zero));
        futex_try_cleanup(bucket, entry);
    }

    pi_propagate_chain(self);
    spin_unlock(&bucket->lock);

    return EOK;
}

/* FUTEX_TRYLOCK_PI: non-blocking attempt to acquire a PI mutex. */
static int futex_trylock_pi(uint32_t *uaddr)
{
    task_t *self = current_task();
    if (!self) return -ESRCH;

    futex_bucket_t *bucket = &futex_hash[futex_hash_index(uaddr)];

    spin_lock(&bucket->lock);

    uint32_t cur_val;
    if (copy_from_user(&cur_val, uaddr, sizeof(cur_val)) != 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }

    if ((cur_val & FUTEX_TID_MASK) != 0) {
        spin_unlock(&bucket->lock);
        return -EAGAIN;
    }

    rt_mutex_t *pi_mutex = futex_get_pi_mutex(bucket, uaddr);
    if (!pi_mutex) {
        spin_unlock(&bucket->lock);
        return -ENOMEM;
    }

    uint32_t new_val = (self->pid & FUTEX_TID_MASK);
    if (copy_to_user(uaddr, &new_val, sizeof(new_val)) != 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }

    pi_mutex->owner      = self;
    pi_mutex->owner_died = 0;
    self->base_weight    = self->weight;
    self->pi_weight      = self->weight;

    spin_unlock(&bucket->lock);
    return EOK;
}

/*
 * FUTEX_CMP_REQUEUE_PI: wake some waiters from uaddr, then requeue
 * remaining waiters from uaddr to uaddr2 (a PI futex).
 */
static int futex_cmp_requeue_pi(uint32_t *uaddr, int nr_wake, int nr_requeue, uint32_t *uaddr2, uint32_t cmpval)
{
    futex_bucket_t *bucket1 = &futex_hash[futex_hash_index(uaddr)];
    futex_bucket_t *bucket2 = &futex_hash[futex_hash_index(uaddr2)];
    futex_entry_t  *entry1  = NULL;
    futex_entry_t  *entry2  = NULL;
    int             woken   = 0;

    if (bucket1 < bucket2) {
        spin_lock(&bucket1->lock);
        spin_lock(&bucket2->lock);
    } else if (bucket1 > bucket2) {
        spin_lock(&bucket2->lock);
        spin_lock(&bucket1->lock);
    } else {
        spin_lock(&bucket1->lock);
    }

    for (entry1 = bucket1->head; entry1; entry1 = entry1->next)
        if (entry1->key == (uintptr_t)uaddr) break;

    if (!entry1) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return 0;
    }

    /* Validate cmpval against *uaddr2 */
    uint32_t cur_val2;
    if (copy_from_user(&cur_val2, uaddr2, sizeof(cur_val2)) != 0) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return -EFAULT;
    }

    if (cur_val2 != cmpval) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return -EAGAIN;
    }

    /* Wake nr_wake waiters from uaddr */
    while (woken < nr_wake) {
        task_t *task = wait_queue_wake_one(&entry1->wq);
        if (!task) break;
        woken++;
    }

    /* Requeue remaining waiters to uaddr2 */
    if (nr_requeue > 0) {
        entry2 = futex_find_or_create(bucket2, uaddr2, FUTEX_BITSET_MATCH_ANY);
        if (!entry2) {
            plogk("futex: Cmp_requeue_pi requeue entry allocation failed for %p\n", (void *)uaddr2);
            futex_try_cleanup(bucket1, entry1);
            if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
            spin_unlock(&bucket1->lock);
            return woken > 0 ? woken : -ENOMEM;
        }

        if (!entry2->pi_mutex) {
            entry2->pi_mutex = malloc(sizeof(rt_mutex_t));
            if (!entry2->pi_mutex) {
                plogk("futex: Cmp_requeue_pi PI mutex allocation failed for %p\n", (void *)uaddr2);
                futex_try_cleanup(bucket1, entry1);
                if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
                spin_unlock(&bucket1->lock);
                return woken > 0 ? woken : -ENOMEM;
            }
            rt_mutex_init(entry2->pi_mutex, uaddr2);
        }

        int requeued = 0;
        while (requeued < nr_requeue) {
            if (!futex_move_waiter(&entry1->wq, &entry2->wq)) break;
            requeued++;
        }
    }

    futex_try_cleanup(bucket1, entry1);
    if (bucket1 != bucket2) {
        futex_try_cleanup(bucket2, entry2);
        spin_unlock(&bucket2->lock);
    }
    spin_unlock(&bucket1->lock);

    return woken;
}

/* sys_futex */

/*
 * Futex syscall entry point.
 *
 * uaddr      - userspace address of the futex word
 * futex_op   - operation (may include FUTEX_PRIVATE_FLAG and
 *              FUTEX_CLOCK_REALTIME flags)
 * val        - expected value (for WAIT) or number to wake (for WAKE)
 * timeout    - pointer to struct timespec in user space (for WAIT)
 * uaddr2     - second futex address (for REQUEUE / WAKE_OP)
 * val3       - encoding for WAKE_OP or expected value for CMP_REQUEUE
 */
int64_t sys_futex(uint32_t *uaddr, int futex_op, uint32_t val, uint64_t timeout, uint32_t *uaddr2, uint32_t val3)
{
    int cmd           = futex_op & 0x7f;
    int flags         = futex_op & ~0x7f;
    int allowed_flags = FUTEX_PRIVATE_FLAG;

    if (cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_WAIT_REQUEUE_PI || cmd == FUTEX_LOCK_PI2) allowed_flags |= FUTEX_CLOCK_REALTIME;
    if (flags & ~allowed_flags) return -EINVAL;

    switch (cmd) {
        case FUTEX_WAIT : {
            /* Validate user address */
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;

            return futex_wait(uaddr, val, timeout, FUTEX_BITSET_MATCH_ANY, 0, 0);
        }

        case FUTEX_WAIT_BITSET : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;

            return futex_wait(uaddr, val, timeout, (uint64_t)val3, 1, (flags & FUTEX_CLOCK_REALTIME) != 0);
        }

        case FUTEX_WAKE : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;

            return futex_wake(uaddr, (int)val, FUTEX_BITSET_MATCH_ANY);
        }

        case FUTEX_WAKE_BITSET : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;

            return futex_wake(uaddr, (int)val, (uint64_t)val3);
        }

        case FUTEX_REQUEUE : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 0) == 0) return -EFAULT;

            /*
             * val   = nr_wake
             * val3  = nr_requeue  (in Linux, val3 is actually the
             *                      uaddr2 comparison value, but for
             *                      plain REQUEUE, nr_requeue is in
             *                      the upper bits of val.  Here we
             *                      use val3 to carry nr_requeue.)
             *
             * Linux convention: val = nr_wake, utime = nr_requeue.
             * In our syscall signature, timeout carries nr_requeue.
             */
            return futex_requeue(uaddr, (int)val, (int)timeout, uaddr2, val3, 0);
        }

        case FUTEX_CMP_REQUEUE : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 0) == 0) return -EFAULT;

            /*
             * val   = nr_wake
             * val3  = expected value at uaddr2
             * timeout = nr_requeue
             */
            return futex_requeue(uaddr, (int)val, (int)timeout, uaddr2, val3, 1);
        }

        case FUTEX_WAKE_OP : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 1) == 0) return -EFAULT;

            /*
             * val   = nr_wake
             * val3  = encoded operation
             * timeout = nr_wake2
             */
            return futex_wake_op(uaddr, (int)val, (int)timeout, uaddr2, val3);
        }

        case FUTEX_FD :
            return -ENOSYS; // FD-based futexes are not supported

        case FUTEX_LOCK_PI : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) == 0) return -EFAULT;

            return futex_lock_pi(uaddr);
        }

        case FUTEX_UNLOCK_PI : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) == 0) return -EFAULT;

            return futex_unlock_pi(uaddr);
        }

        case FUTEX_TRYLOCK_PI : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) == 0) return -EFAULT;

            return futex_trylock_pi(uaddr);
        }

        case FUTEX_CMP_REQUEUE_PI : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 1) == 0) return -EFAULT;

            return futex_cmp_requeue_pi(uaddr, (int)val, (int)timeout, uaddr2, val3);
        }

        case FUTEX_WAIT_REQUEUE_PI : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) == 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 1) == 0) return -EFAULT;

            return futex_cmp_requeue_pi(uaddr, 0, (int)val, uaddr2, val3);
        }

        case FUTEX_LOCK_PI2 : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) == 0) return -EFAULT;

            return futex_lock_pi(uaddr);
        }

        default :
            return -EINVAL;
    }
}

/* futex2: sys_futex_wake / sys_futex_wait / sys_futex_requeue */

/*
 * futex2 is the Linux 6.7+ "futex2" syscall family.  Unlike the classic
 * futex() multiplexer, each operation is its own syscall:
 *
 *   futex_wait (455):  block while *uaddr == val, waking only when a
 *                      futex_wake with an overlapping mask arrives.
 *   futex_wake (454):  wake up to nr waiters whose wait-mask overlaps
 *                      the wake mask.
 *   futex_requeue (456): wake up to nr_wake waiters on uaddr, then move
 *                      up to nr_requeue remaining waiters to uaddr2
 *                      (CMP_REQUEUE semantics: *uaddr must equal val).
 *
 * The classic futex hash table is reused.  A futex2 futex is addressed by
 * a size-aware key (uaddr >> (size_code + 1), matching Linux), so futexes
 * of different widths never collide even if they share an address.  Every
 * waiter keeps its own 64-bit mask: entries are keyed by (key, mask) like
 * classic bitset waiters, and a wake selects entries whose mask has any
 * bit in common with the wake mask.
 */

/* Return the byte width for a FUTEX2_SIZE_* code. */
static int futex2_size_bytes(unsigned int size_code)
{
    switch (size_code) {
        case FUTEX2_SIZE_U8 :
            return 1;
        case FUTEX2_SIZE_U16 :
            return 2;
        case FUTEX2_SIZE_U64 :
            return 8;
        default :
            return 4; // FUTEX2_SIZE_U32
    }
}

/*
 * Linux futex_validate_input(): val/mask must not carry bits outside the
 * width of the futex word.
 */
static int futex2_validate_value(unsigned int size_code, uint64_t value)
{
    uint64_t width_mask;

    switch (size_code) {
        case FUTEX2_SIZE_U8 :
            width_mask = 0x00000000000000ffULL;
            break;
        case FUTEX2_SIZE_U16 :
            width_mask = 0x000000000000ffffULL;
            break;
        case FUTEX2_SIZE_U32 :
            width_mask = 0x00000000ffffffffULL;
            break;
        default :
            width_mask = 0xffffffffffffffffULL;
            break;
    }
    return (value & ~width_mask) == 0;
}

/*
 * Futex key for futex2 words.  Linux derives the hash key from the page
 * address plus the offset within the page; the classic futex path in this
 * kernel keys on the raw user address, so futex2 uses the same key to keep
 * classic and futex2 futexes on the same address interoperable.  The
 * FUTEX2_SIZE_* width only affects how val/mask are validated and how the
 * word is read, not the key.
 */
static inline uintptr_t futex2_key(uint64_t uaddr, unsigned int size_code)
{
    (void)size_code;
    return (uintptr_t)uaddr;
}

/* Read the futex word (1/2/4/8 bytes) from user space. */
static int futex2_read_word(uint64_t uaddr, unsigned int size_code, uint64_t *out)
{
    *out = 0;
    if (copy_from_user(out, (const void *)(uintptr_t)uaddr, futex2_size_bytes(size_code)) != 0) return -EFAULT;
    return 0;
}

/*
 * Block on a futex2 word.  The timeout (if any) is an absolute timeout on
 * the clock selected by `realtime` (CLOCK_REALTIME) or CLOCK_MONOTONIC.
 */
static int futex2_wait_core(uint64_t uaddr, unsigned int size_code, uint64_t val, uint64_t mask, uint64_t timeout, int realtime)
{
    futex_bucket_t *bucket;
    futex_entry_t  *entry;
    uint64_t        cur_val;
    uint64_t        deadline = 0;
    int             ret;

    if (mask == 0) return -EINVAL; // same as classic bitset == 0

    uintptr_t key = futex2_key(uaddr, size_code);
    bucket        = &futex_hash[futex_hash_index((uint32_t *)(uintptr_t)key)];

    if (timeout) {
        ret = futex_read_timespec(timeout, &deadline);
        if (ret) return ret;
        deadline = futex_deadline(deadline, 1, realtime); // absolute
    }

    spin_lock(&bucket->lock);
    if (futex2_read_word(uaddr, size_code, &cur_val) != 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }
    if (cur_val != val) {
        spin_unlock(&bucket->lock);
        return -EAGAIN;
    }

    entry = futex_create_waiter(bucket, (uint32_t *)(uintptr_t)key, mask);
    if (!entry) {
        plogk("futex: Futex2 waiter allocation failed for %#lx\n", (unsigned long)uaddr);
        spin_unlock(&bucket->lock);
        return -ENOMEM;
    }
    wait_queue_prepare(&entry->wq);
    spin_unlock(&bucket->lock);

    if (timeout)
        ret = wait_queue_wait_timed(&entry->wq, deadline);
    else {
        wait_queue_sleep();
        ret = 0;
    }

    spin_lock(&bucket->lock);
    entry = futex_find_waiter(bucket, (uint32_t *)(uintptr_t)key, mask);
    futex_try_cleanup(bucket, entry);
    spin_unlock(&bucket->lock);
    return ret;
}

/*
 * Wake up to nr_wake waiters whose mask overlaps `mask` on the futex2
 * word identified by `key`.  Returns the number actually woken.
 */
static int futex2_wake_core(uintptr_t key, int nr_wake, uint64_t mask)
{
    futex_bucket_t *bucket = &futex_hash[futex_hash_index((uint32_t *)(uintptr_t)key)];
    futex_entry_t  *entry;
    int             woken = 0;

    if (nr_wake <= 0) return 0;

    spin_lock(&bucket->lock);
    for (entry = bucket->head; entry; entry = entry->next) {
        if (entry->key != key) continue;
        if (!(entry->bitset & mask)) continue;

        while (woken < nr_wake) {
            task_t *task = wait_queue_wake_one(&entry->wq);
            if (!task) break;
            woken++;
        }
    }
    /* Cleanup is deferred to the final waiter, like classic futex_wake. */
    spin_unlock(&bucket->lock);
    return woken;
}

/* Find the first entry with the given key (bucket lock must be held). */
static futex_entry_t *futex2_find_key(futex_bucket_t *bucket, uintptr_t key)
{
    for (futex_entry_t *entry = bucket->head; entry; entry = entry->next)
        if (entry->key == key) return entry;
    return NULL;
}

/*
 * futex_requeue(): CMP_REQUEUE semantics.  If *uaddr != cmpval return
 * -EAGAIN.  Otherwise wake up to nr_wake waiters on key1, then move up
 * to nr_requeue remaining waiters to key2.  Moved waiters keep their own
 * mask: each lands in the (key2, mask) queue.
 */
static int futex2_requeue_core(uint64_t uaddr, unsigned int size_code1, uint64_t uaddr2, unsigned int size_code2, int nr_wake, int nr_requeue,
                               uint64_t cmpval)
{
    uintptr_t       key1    = futex2_key(uaddr, size_code1);
    uintptr_t       key2    = futex2_key(uaddr2, size_code2);
    futex_bucket_t *bucket1 = &futex_hash[futex_hash_index((uint32_t *)(uintptr_t)key1)];
    futex_bucket_t *bucket2 = &futex_hash[futex_hash_index((uint32_t *)(uintptr_t)key2)];
    futex_entry_t  *entry;
    uint64_t        cur_val;
    int             woken    = 0;
    int             requeued = 0;

    if (nr_wake < 0 || nr_requeue < 0) return -EINVAL;

    /* Lock both buckets in address order to avoid deadlock. */
    if (bucket1 < bucket2) {
        spin_lock(&bucket1->lock);
        spin_lock(&bucket2->lock);
    } else if (bucket1 > bucket2) {
        spin_lock(&bucket2->lock);
        spin_lock(&bucket1->lock);
    } else {
        spin_lock(&bucket1->lock);
    }

    if (futex2_read_word(uaddr, size_code1, &cur_val) != 0) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return -EFAULT;
    }
    if (cur_val != cmpval) {
        if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
        spin_unlock(&bucket1->lock);
        return -EAGAIN;
    }

    /* Phase 1: wake up to nr_wake waiters on key1. */
    for (entry = bucket1->head; entry && woken < nr_wake; entry = entry->next) {
        if (entry->key != key1) continue;
        while (woken < nr_wake) {
            task_t *task = wait_queue_wake_one(&entry->wq);
            if (!task) break;
            woken++;
        }
    }

    /* Phase 2: move up to nr_requeue waiters to key2, preserving masks. */
    if (nr_requeue > 0) {
        for (entry = bucket1->head; entry && requeued < nr_requeue; entry = entry->next) {
            if (entry->key != key1) continue;

            while (requeued < nr_requeue) {
                futex_entry_t *dst = futex_find_waiter(bucket2, (uint32_t *)(uintptr_t)key2, entry->bitset);
                if (!dst) {
                    dst = futex_create_waiter(bucket2, (uint32_t *)(uintptr_t)key2, entry->bitset);
                    if (!dst) goto requeue_done;
                }
                if (!futex_move_waiter(&entry->wq, &dst->wq)) break;
                requeued++;
            }
        }
    }

requeue_done:
    /* Remove entries left empty by wake/requeue. */
    while ((entry = futex2_find_key(bucket1, key1)) != NULL)
        if (!futex_try_cleanup(bucket1, entry)) break;
    if (bucket1 != bucket2) {
        while ((entry = futex2_find_key(bucket2, key2)) != NULL)
            if (!futex_try_cleanup(bucket2, entry)) break;
        spin_unlock(&bucket2->lock);
    }
    spin_unlock(&bucket1->lock);

    return woken;
}

/*
 * sys_futex_wake(uaddr, mask, nr, flags)
 * Wake up to nr waiters on uaddr whose mask overlaps `mask`.
 */
int64_t sys_futex_wake(uint64_t uaddr, uint64_t mask, uint64_t nr, uint64_t flags, uint64_t a4, uint64_t a5)
{
    unsigned int size_code = (unsigned int)(flags & FUTEX2_SIZE_MASK);

    (void)a4;
    (void)a5;
    if (flags & ~FUTEX2_VALID_MASK) return -EINVAL;
    if (!futex2_validate_value(size_code, mask)) return -EINVAL;
    if (!uaddr) return -EFAULT;
    if (user_access_ok((void *)(uintptr_t)uaddr, futex2_size_bytes(size_code), 0) == 0) return -EFAULT;

    return futex2_wake_core(futex2_key(uaddr, size_code), (int)nr, mask);
}

/*
 * sys_futex_wait(uaddr, val, mask, flags, timeout, clockid)
 * Block while *uaddr == val.  timeout is an absolute struct timespec on
 * the clock given by clockid (CLOCK_REALTIME=0, CLOCK_MONOTONIC=1).
 */
int64_t sys_futex_wait(uint64_t uaddr, uint64_t val, uint64_t mask, uint64_t flags, uint64_t timeout, uint64_t clockid)
{
    unsigned int size_code = (unsigned int)(flags & FUTEX2_SIZE_MASK);

    if (flags & ~FUTEX2_VALID_MASK) return -EINVAL;
    if (!futex2_validate_value(size_code, val) || !futex2_validate_value(size_code, mask)) return -EINVAL;
    if (timeout && clockid != 0 && clockid != 1) return -EINVAL; // CLOCK_REALTIME / CLOCK_MONOTONIC
    if (!uaddr) return -EFAULT;
    if (user_access_ok((void *)(uintptr_t)uaddr, futex2_size_bytes(size_code), 0) == 0) return -EFAULT;

    return futex2_wait_core(uaddr, size_code, val, mask, timeout, clockid == 0);
}

/*
 * sys_futex_requeue(waiters, flags, nr_wake, nr_requeue)
 * waiters points to two struct futex_waitv entries:
 *   [0] = source futex (uaddr + expected val + flags)
 *   [1] = destination futex (uaddr + flags; val ignored)
 * The syscall-level flags argument must be zero (Linux behavior).
 */
int64_t sys_futex_requeue(uint64_t waiters, uint64_t flags, uint64_t nr_wake, uint64_t nr_requeue, uint64_t a4, uint64_t a5)
{
    struct futex_waitv wv[2];
    unsigned int       size_code0;
    unsigned int       size_code1;

    (void)a4;
    (void)a5;
    if (flags) return -EINVAL;
    if (!waiters) return -EINVAL;

    if (copy_from_user(wv, (const void *)(uintptr_t)waiters, sizeof(wv)) != 0) return -EFAULT;

    for (int i = 0; i < 2; i++) {
        if (wv[i].__reserved) return -EINVAL;
        if (wv[i].flags & ~FUTEX2_VALID_MASK) return -EINVAL;
        if (!wv[i].uaddr) return -EINVAL;
    }

    size_code0 = (unsigned int)(wv[0].flags & FUTEX2_SIZE_MASK);
    size_code1 = (unsigned int)(wv[1].flags & FUTEX2_SIZE_MASK);
    if (!futex2_validate_value(size_code0, wv[0].val)) return -EINVAL;
    if (user_access_ok((void *)(uintptr_t)wv[0].uaddr, futex2_size_bytes(size_code0), 0) == 0) return -EFAULT;
    if (user_access_ok((void *)(uintptr_t)wv[1].uaddr, futex2_size_bytes(size_code1), 0) == 0) return -EFAULT;

    return futex2_requeue_core(wv[0].uaddr, size_code0, wv[1].uaddr, size_code1, (int)nr_wake, (int)nr_requeue, wv[0].val);
}

/* futex_init */

/*
 * Initialize the futex hash table.
 * Called once during kernel startup.
 */
void futex_init(void)
{
    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        futex_hash[i].head        = NULL;
        futex_hash[i].lock.lock   = 0;
        futex_hash[i].lock.rflags = 0;
    }
}
