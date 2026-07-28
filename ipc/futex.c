/*
 *
 *      futex.c
 *      Fast userspace mutex implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <ipc/futex.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/rt_mutex.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#ifndef FUTEX_HASH_BITS
#    define FUTEX_HASH_BITS 8
#endif
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)

#define FUTEX_TICKS_PER_SEC 100
#define FUTEX_NSEC_PER_TICK (1000000000ULL / FUTEX_TICKS_PER_SEC)

/* The syscall clock layer may provide the realtime clock in scheduler ticks. */
#ifndef FUTEX_REALTIME_TICKS
__attribute__((weak)) uint64_t futex_realtime_ticks(void) { return sched_ticks(); }
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

/* ------------------------------------------------------------------ */
/*  Type definitions                                                    */
/* ------------------------------------------------------------------ */

typedef struct futex_entry {
        uintptr_t           key;
        uint32_t            bitset; /* bitset mask for FUTEX_WAIT_BITSET */
        wait_queue_t        wq;
        struct futex_entry *next;
        rt_mutex_t         *pi_mutex; /* non-NULL for PI futex entries */
} futex_entry_t;

typedef struct futex_bucket {
        futex_entry_t *head;
        spinlock_t     lock;
} futex_bucket_t;

/* ------------------------------------------------------------------ */
/*  Static state                                                        */
/* ------------------------------------------------------------------ */

static futex_bucket_t futex_hash[FUTEX_HASH_SIZE];

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Hash a user address into a bucket index.
 */
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
    for (futex_entry_t *entry = bucket->head; entry; entry = entry->next) {
        if (entry->key == key) return entry;
    }
    return NULL;
}

/*
 * Find or create an entry for uaddr in the given bucket.
 * Must be called with the bucket lock held.
 * Returns NULL on allocation failure.
 * If the entry already exists, its bitset is OR-ed with the new bitset
 * so that all waiters on the same futex can be woken by a matching wake.
 */
static futex_entry_t *futex_find_or_create(futex_bucket_t *bucket, uint32_t *uaddr, uint32_t bitset)
{
    futex_entry_t *entry = futex_find(bucket, uaddr);
    if (entry) {
        entry->bitset |= bitset;
        return entry;
    }

    entry = (futex_entry_t *)malloc(sizeof(futex_entry_t));
    if (!entry) return NULL;

    entry->key      = (uintptr_t)uaddr;
    entry->bitset   = bitset;
    entry->pi_mutex = NULL;
    wait_queue_init(&entry->wq);
    entry->next  = bucket->head;
    bucket->head = entry;

    return entry;
}

static futex_entry_t *futex_find_waiter(futex_bucket_t *bucket, uint32_t *uaddr, uint32_t bitset)
{
    uintptr_t key = (uintptr_t)uaddr;

    for (futex_entry_t *entry = bucket->head; entry; entry = entry->next) {
        if (entry->key == key && entry->bitset == bitset && !entry->pi_mutex) return entry;
    }
    return NULL;
}

/* Separate queues per bitset make WAKE_BITSET selection exact. */
static futex_entry_t *futex_create_waiter(futex_bucket_t *bucket, uint32_t *uaddr, uint32_t bitset)
{
    futex_entry_t *entry = futex_find_waiter(bucket, uaddr, bitset);
    if (entry) return entry;

    entry = (futex_entry_t *)malloc(sizeof(futex_entry_t));
    if (!entry) return NULL;

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

/* ------------------------------------------------------------------ */
/*  FUTEX_WAIT / FUTEX_WAIT_BITSET                                     */
/* ------------------------------------------------------------------ */

/*
 * Wait on a futex.  If *uaddr != val, return -EAGAIN immediately.
 * Otherwise block until woken by futex_wake or until the timeout
 * expires.
 *
 * bitset is the mask of bits that must match for wakeup;
 * FUTEX_BITSET_MATCH_ANY (0xffffffff) matches any wake.
 */
static int futex_wait(uint32_t *uaddr, uint32_t val, uint64_t timeout, uint32_t bitset, int absolute, int realtime)
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

/* ------------------------------------------------------------------ */
/*  FUTEX_WAKE / FUTEX_WAKE_BITSET                                     */
/* ------------------------------------------------------------------ */

/*
 * Wake up to nr_wake waiters on the futex at uaddr.
 * Only wake tasks whose bitset matches the wake bitset.
 * Returns the number of tasks actually woken.
 */
int futex_wake(uint32_t *uaddr, int nr_wake, uint32_t bitset)
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
     * Do NOT free empty entries here — a futex_wait caller that was
     * just woken may still hold a stale entry pointer and will access
     * it after re-acquiring the bucket lock.  Cleanup is the
     * responsibility of the final waiter (futex_try_cleanup in
     * futex_wait).
     */

    spin_unlock(&bucket->lock);

    return woken;
}

/* ------------------------------------------------------------------ */
/*  FUTEX_REQUEUE / FUTEX_CMP_REQUEUE                                  */
/* ------------------------------------------------------------------ */

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

    /*
     * Lock both buckets in address order to avoid deadlock.
     */
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
    for (entry1 = bucket1->head; entry1; entry1 = entry1->next) {
        if (entry1->key == (uintptr_t)uaddr) break;
    }

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

/* ------------------------------------------------------------------ */
/*  FUTEX_WAKE_OP                                                      */
/* ------------------------------------------------------------------ */

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

    /*
     * Lock both buckets in address order to avoid deadlock.
     */
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

/* ------------------------------------------------------------------ */
/*  PI futex helpers                                                    */
/* ------------------------------------------------------------------ */

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
        if (!entry->pi_mutex) return NULL;
        rt_mutex_init(entry->pi_mutex, uaddr);
    }

    return entry->pi_mutex;
}

/*
 * FUTEX_LOCK_PI: acquire a PI mutex.
 * Userspace fastpath: cmpxchg(*uaddr, 0, tid) → success.
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
 * Userspace fastpath: cmpxchg(*uaddr, tid, 0) → success if no waiters.
 * Kernel slowpath (this function): wake the highest-priority waiter.
 */
static int futex_unlock_pi(uint32_t *uaddr)
{
    task_t *self = current_task();
    if (!self) return -ESRCH;

    futex_bucket_t *bucket = &futex_hash[futex_hash_index(uaddr)];

    spin_lock(&bucket->lock);

    futex_entry_t *entry;
    for (entry = bucket->head; entry; entry = entry->next) {
        if (entry->key == (uintptr_t)uaddr) break;
    }

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
        if (!ilist_is_empty(&pi_mutex->wq.tasks)) { new_val |= FUTEX_WAITERS; }
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

/*
 * FUTEX_TRYLOCK_PI: non-blocking attempt to acquire a PI mutex.
 */
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

    for (entry1 = bucket1->head; entry1; entry1 = entry1->next) {
        if (entry1->key == (uintptr_t)uaddr) break;
    }

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
            futex_try_cleanup(bucket1, entry1);
            if (bucket1 != bucket2) spin_unlock(&bucket2->lock);
            spin_unlock(&bucket1->lock);
            return woken > 0 ? woken : -ENOMEM;
        }

        if (!entry2->pi_mutex) {
            entry2->pi_mutex = malloc(sizeof(rt_mutex_t));
            if (!entry2->pi_mutex) {
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

/* ------------------------------------------------------------------ */
/*  sys_futex                                                           */
/* ------------------------------------------------------------------ */

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

    if (cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_WAIT_REQUEUE_PI || cmd == FUTEX_LOCK_PI2)
        allowed_flags |= FUTEX_CLOCK_REALTIME;
    if (flags & ~allowed_flags) return -EINVAL;

    switch (cmd) {
        case FUTEX_WAIT : {
            /* Validate user address */
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;

            return futex_wait(uaddr, val, timeout, FUTEX_BITSET_MATCH_ANY, 0, 0);
        }

        case FUTEX_WAIT_BITSET : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;

            return futex_wait(uaddr, val, timeout, val3, 1, (flags & FUTEX_CLOCK_REALTIME) != 0);
        }

        case FUTEX_WAKE : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;

            return futex_wake(uaddr, (int)val, FUTEX_BITSET_MATCH_ANY);
        }

        case FUTEX_WAKE_BITSET : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;

            return futex_wake(uaddr, (int)val, val3);
        }

        case FUTEX_REQUEUE : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 0) != 0) return -EFAULT;

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
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 0) != 0) return -EFAULT;

            /*
         * val   = nr_wake
         * val3  = expected value at uaddr2
         * timeout = nr_requeue
         */
            return futex_requeue(uaddr, (int)val, (int)timeout, uaddr2, val3, 1);
        }

        case FUTEX_WAKE_OP : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 1) != 0) return -EFAULT;

            /*
         * val   = nr_wake
         * val3  = encoded operation
         * timeout = nr_wake2
         */
            return futex_wake_op(uaddr, (int)val, (int)timeout, uaddr2, val3);
        }

        case FUTEX_FD :
            /* File descriptor association not implemented */
            return -ENOSYS;

        case FUTEX_LOCK_PI : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) != 0) return -EFAULT;

            return futex_lock_pi(uaddr);
        }

        case FUTEX_UNLOCK_PI : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) != 0) return -EFAULT;

            return futex_unlock_pi(uaddr);
        }

        case FUTEX_TRYLOCK_PI : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) != 0) return -EFAULT;

            return futex_trylock_pi(uaddr);
        }

        case FUTEX_CMP_REQUEUE_PI : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 1) != 0) return -EFAULT;

            return futex_cmp_requeue_pi(uaddr, (int)val, (int)timeout, uaddr2, val3);
        }

        case FUTEX_WAIT_REQUEUE_PI : {
            if (!uaddr || !uaddr2) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 0) != 0) return -EFAULT;
            if (user_access_ok(uaddr2, sizeof(uint32_t), 1) != 0) return -EFAULT;

            return futex_cmp_requeue_pi(uaddr, 0, (int)val, uaddr2, val3);
        }

        case FUTEX_LOCK_PI2 : {
            if (!uaddr) return -EFAULT;
            if (user_access_ok(uaddr, sizeof(uint32_t), 1) != 0) return -EFAULT;

            return futex_lock_pi(uaddr);
        }

        default :
            return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/*  futex_init                                                          */
/* ------------------------------------------------------------------ */

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
