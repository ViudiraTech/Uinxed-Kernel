/*
 *
 *      filelock.c
 *      Byte-range file locks: POSIX record locks and open file description locks
 *
 *      2026/11/12 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

/*
 * Every locked inode owns a context holding its granted locks.  A lock is
 * owned either by a process (POSIX record locks) or by an open file
 * description (open file description locks, "OFD locks"):
 *
 *   - Locks of one owner never overlap, and neighbouring locks of the same
 *     type are coalesced, so a new request only has to touch the locks it
 *     overlaps or abuts.
 *   - Locks of different owners conflict when their ranges overlap and at
 *     least one of them is a write lock.  A POSIX lock and an OFD lock always
 *     belong to different owners, so they conflict even inside one process,
 *     and two OFD locks conflict even when they were taken by one thread.
 *   - POSIX locks go away when the owning process closes any descriptor of
 *     the inode, or exits.  OFD locks go away when the last reference to the
 *     open file description is dropped.
 *   - Only POSIX requests take part in deadlock detection: an OFD lock is not
 *     tied to a thread of execution, so there is nobody to ask to back off.
 *
 * Locking order: bucket lock, then context lock, then the blocked-request lock.
 * A context is looked up under its bucket lock, which also protects its
 * reference count; the count covers every operation in flight and every granted
 * lock, and the context is freed when it reaches zero.
 */

#include <fs/core/filelock.h>
#include <libs/std/errno.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#define FILELOCK_BUCKETS 64

typedef struct file_lock {
        struct file_lock *next;
        const void       *owner;
        int64_t           start;
        int64_t           end;
        int32_t           pid;
        uint8_t           type;
        bool              ofd;
} file_lock_t;

typedef struct filelock_ctx {
        struct filelock_ctx *hash_next;
        filelock_key_t       key;
        file_lock_t         *locks;   // granted locks, protected by lock
        spinlock_t           lock;    // protects locks and waiters
        wait_queue_t         wait;    // requests blocked on this inode
        uint32_t             waiters; // requests that are, or are about to be, asleep on wait
        uint32_t             refs;    // operations in flight plus granted locks, protected by the bucket lock
} filelock_ctx_t;

typedef struct filelock_bucket {
        spinlock_t      lock;
        filelock_ctx_t *head;
} filelock_bucket_t;

/* A POSIX request that is asleep behind a lock held by another owner. */
typedef struct filelock_waiter {
        struct filelock_waiter *next;
        const void             *owner;
        const void             *blocker;
} filelock_waiter_t;

static filelock_bucket_t  filelock_table[FILELOCK_BUCKETS];
static uint32_t           filelock_ctx_count;
static spinlock_t         filelock_blocked_lock;
static filelock_waiter_t *filelock_blocked;

/* Build the lock identity of an open node. */
void filelock_key_of(filelock_key_t *key, vfs_node_t node)
{
    vfs_node_t root = node->root ? node->root : node;

    key->mount    = (uintptr_t)root;
    key->mount_id = root->mount_id;
    key->ino      = node->inode;
    key->dev      = node->dev;
    key->fsid     = node->fsid;

    /* Nodes without an inode number (pipes, sockets) are only ever themselves. */
    if (!node->inode) {
        key->mount    = (uintptr_t)node;
        key->mount_id = 0;
    }
}

/* Bucket index of a lock identity. */
static filelock_bucket_t *filelock_bucket(const filelock_key_t *key)
{
    uint64_t hash = key->ino * 11400714819323198485ULL;
    hash ^= (uint64_t)key->mount + (key->mount_id << 17);
    hash ^= (uint64_t)key->dev * 0x9e3779b97f4a7c15ULL;
    hash ^= (uint64_t)key->fsid << 48;
    hash ^= hash >> 31;
    return &filelock_table[hash % FILELOCK_BUCKETS];
}

/* Whether two lock identities name the same inode. */
static bool filelock_key_equal(const filelock_key_t *left, const filelock_key_t *right)
{
    return left->mount == right->mount && left->mount_id == right->mount_id && left->ino == right->ino && left->dev == right->dev && left->fsid == right->fsid;
}

/* Take a reference on the context of an inode, creating it when asked to. */
static filelock_ctx_t *filelock_ctx_acquire(const filelock_key_t *key, bool create)
{
    filelock_ctx_t *fresh = NULL;
    if (create) {
        fresh = calloc(1, sizeof(*fresh));
        if (!fresh) return NULL;
    }

    filelock_bucket_t *bucket = filelock_bucket(key);
    spin_lock(&bucket->lock);
    for (filelock_ctx_t *ctx = bucket->head; ctx; ctx = ctx->hash_next) {
        if (!filelock_key_equal(&ctx->key, key)) continue;
        ctx->refs++;
        spin_unlock(&bucket->lock);
        free(fresh);
        return ctx;
    }
    if (!fresh) {
        spin_unlock(&bucket->lock);
        return NULL;
    }

    fresh->key  = *key;
    fresh->refs = 1;
    wait_queue_init(&fresh->wait);
    fresh->hash_next = bucket->head;
    bucket->head     = fresh;
    __atomic_add_fetch(&filelock_ctx_count, 1, __ATOMIC_RELAXED);
    spin_unlock(&bucket->lock);
    return fresh;
}

/*
 * Drop the operation's reference and account for the granted locks it created
 * (positive net) or removed (negative net).  The context is freed once nothing
 * refers to it any more.
 */
static void filelock_ctx_settle(filelock_ctx_t *ctx, int64_t net)
{
    filelock_bucket_t *bucket = filelock_bucket(&ctx->key);

    spin_lock(&bucket->lock);
    int64_t refs = (int64_t)ctx->refs + net - 1;
    ctx->refs    = (uint32_t)refs;
    if (refs > 0) {
        spin_unlock(&bucket->lock);
        return;
    }

    for (filelock_ctx_t **link = &bucket->head; *link; link = &(*link)->hash_next) {
        if (*link != ctx) continue;
        *link = ctx->hash_next;
        break;
    }
    __atomic_sub_fetch(&filelock_ctx_count, 1, __ATOMIC_RELAXED);
    spin_unlock(&bucket->lock);
    free(ctx);
}

/* Free a chain of lock records. */
static void filelock_free_chain(file_lock_t *chain)
{
    while (chain) {
        file_lock_t *next = chain->next;
        free(chain);
        chain = next;
    }
}

/* Pop one record off a chain of preallocated records. */
static file_lock_t *filelock_pop(file_lock_t **chain)
{
    file_lock_t *record = *chain;
    if (record) *chain = record->next;
    return record;
}

/* Number of records in a chain. */
static size_t filelock_chain_length(const file_lock_t *chain)
{
    size_t length = 0;
    for (; chain; chain = chain->next) length++;
    return length;
}

/* Whether a lock range overlaps a request range. */
static bool filelock_overlaps(const file_lock_t *lock, int64_t start, int64_t end)
{
    return lock->start <= end && lock->end >= start;
}

/* Whether a lock range overlaps or directly abuts a request range. */
static bool filelock_touches(const file_lock_t *lock, int64_t start, int64_t end)
{
    return (end == FILELOCK_OFFSET_MAX || lock->start <= end + 1) && (start == 0 || lock->end >= start - 1);
}

/* Find the lowest-starting lock of another owner that keeps a request from being granted. */
static const file_lock_t *filelock_find_conflict(const filelock_ctx_t *ctx, const filelock_request_t *request)
{
    const file_lock_t *found = NULL;

    for (const file_lock_t *lock = ctx->locks; lock; lock = lock->next) {
        if (lock->owner == request->owner) continue;
        if (lock->type != FILELOCK_WRLCK && request->type != FILELOCK_WRLCK) continue;
        if (!filelock_overlaps(lock, request->start, request->end)) continue;
        if (!found || lock->start < found->start) found = lock;
    }
    return found;
}

/*
 * Apply a request that nobody conflicts with.  The caller preallocates the
 * records the request may need (one for the new lock and one to split an
 * existing lock in two), so the update never fails half way.  Records that
 * become unused are chained on garbage; net counts the change in granted
 * locks and unblocked says whether other requests may now proceed.
 */
static int filelock_apply(filelock_ctx_t *ctx, const filelock_request_t *request, file_lock_t **spare, file_lock_t **garbage, int64_t *net, bool *unblocked)
{
    bool unlock = request->type == FILELOCK_UNLCK;

    /* One lock can at most be split in two, because the owner's locks do not overlap. */
    size_t needed = unlock ? 0 : 1;
    for (const file_lock_t *lock = ctx->locks; lock; lock = lock->next) {
        if (lock->owner != request->owner) continue;
        if (!unlock && lock->type == request->type) continue;
        if (lock->start < request->start && lock->end > request->end) {
            needed++;
            break;
        }
    }
    if (filelock_chain_length(*spare) < needed) return -ENOLCK;

    int64_t start = request->start;
    int64_t end   = request->end;

    file_lock_t **link = &ctx->locks;
    while (*link) {
        file_lock_t *lock = *link;
        if (lock->owner != request->owner) {
            link = &lock->next;
            continue;
        }

        if (!unlock && lock->type == request->type) {
            /* Same type: the old lock is absorbed into the new one. */
            if (!filelock_touches(lock, request->start, request->end)) {
                link = &lock->next;
                continue;
            }
            if (lock->start < start) start = lock->start;
            if (lock->end > end) end = lock->end;
            *link      = lock->next;
            lock->next = *garbage;
            *garbage   = lock;
            (*net)--;
            continue;
        }

        /* Different type, or an unlock: carve the requested range out of the old lock. */
        if (!filelock_overlaps(lock, request->start, request->end)) {
            link = &lock->next;
            continue;
        }
        *unblocked = true;

        if (lock->start < request->start && lock->end > request->end) {
            file_lock_t *tail = filelock_pop(spare);
            *tail             = *lock;
            tail->start       = request->end + 1;
            tail->next        = lock->next;
            lock->end         = request->start - 1;
            lock->next        = tail;
            (*net)++;
            link = &tail->next;
        } else if (lock->start >= request->start && lock->end <= request->end) {
            *link      = lock->next;
            lock->next = *garbage;
            *garbage   = lock;
            (*net)--;
        } else if (lock->start < request->start) {
            lock->end = request->start - 1;
            link      = &lock->next;
        } else {
            lock->start = request->end + 1;
            link        = &lock->next;
        }
    }

    if (!unlock) {
        file_lock_t *lock = filelock_pop(spare);
        lock->owner       = request->owner;
        lock->start       = start;
        lock->end         = end;
        lock->pid         = request->pid;
        lock->type        = request->type;
        lock->ofd         = request->ofd;
        lock->next        = ctx->locks;
        ctx->locks        = lock;
        (*net)++;
    }
    return EOK;
}

/*
 * Whether blocking owner on blocker closes a cycle of POSIX requests, each
 * waiting for a lock held by the next.  The walk is bounded, as in Linux, so a
 * long chain is simply assumed to be harmless.
 */
static bool filelock_deadlock(const void *owner, const void *blocker)
{
    for (int depth = 0; depth < FILELOCK_MAX_DEADLK; depth++) {
        const filelock_waiter_t *waiter = filelock_blocked;
        while (waiter && waiter->owner != blocker) waiter = waiter->next;
        if (!waiter) return false;
        blocker = waiter->blocker;
        if (blocker == owner) return true;
    }
    return false;
}

/* Register a sleeping POSIX request, unless doing so would deadlock. */
static bool filelock_waiter_add(filelock_waiter_t *waiter, const void *blocker)
{
    spin_lock(&filelock_blocked_lock);
    if (filelock_deadlock(waiter->owner, blocker)) {
        spin_unlock(&filelock_blocked_lock);
        return false;
    }
    waiter->blocker  = blocker;
    waiter->next     = filelock_blocked;
    filelock_blocked = waiter;
    spin_unlock(&filelock_blocked_lock);
    return true;
}

/* Unregister a POSIX request that woke up. */
static void filelock_waiter_remove(filelock_waiter_t *waiter)
{
    spin_lock(&filelock_blocked_lock);
    for (filelock_waiter_t **link = &filelock_blocked; *link; link = &(*link)->next) {
        if (*link != waiter) continue;
        *link = waiter->next;
        break;
    }
    spin_unlock(&filelock_blocked_lock);
}

/* Report the first lock that would block a request. */
int filelock_test(const filelock_key_t *key, const filelock_request_t *request, filelock_conflict_t *conflict)
{
    filelock_ctx_t *ctx = filelock_ctx_acquire(key, false);
    if (!ctx) return 0;

    int found = 0;
    spin_lock(&ctx->lock);
    const file_lock_t *blocker = filelock_find_conflict(ctx, request);
    if (blocker) {
        conflict->start = blocker->start;
        conflict->end   = blocker->end;
        conflict->pid   = blocker->ofd ? -1 : blocker->pid;
        conflict->type  = blocker->type;
        found           = 1;
    }
    spin_unlock(&ctx->lock);

    filelock_ctx_settle(ctx, 0);
    return found;
}

/* Apply a lock or unlock request, sleeping for it when asked to. */
int filelock_set(const filelock_key_t *key, const filelock_request_t *request, bool wait)
{
    bool unlock = request->type == FILELOCK_UNLCK;

    filelock_ctx_t *ctx = filelock_ctx_acquire(key, !unlock);
    if (!ctx) return unlock ? EOK : -ENOLCK;

    /* Allocate every record the update may need before touching the lock list. */
    size_t wanted = 2; // the new lock, plus one half of a lock it splits in two
    if (unlock) wanted = request->start == 0 && request->end == FILELOCK_OFFSET_MAX ? 0 : 1;
    file_lock_t *spare = NULL;
    for (size_t i = 0; i < wanted; i++) {
        file_lock_t *record = calloc(1, sizeof(*record));
        if (!record) {
            filelock_free_chain(spare);
            filelock_ctx_settle(ctx, 0);
            return -ENOLCK;
        }
        record->next = spare;
        spare        = record;
    }

    filelock_waiter_t waiter    = {.owner = request->owner};
    file_lock_t      *garbage   = NULL;
    int64_t           net       = 0;
    bool              unblocked = false;
    bool              wake      = false;
    int               ret       = EOK;

    spin_lock(&ctx->lock);
    for (;;) {
        const file_lock_t *blocker = unlock ? NULL : filelock_find_conflict(ctx, request);
        if (!blocker) {
            ret  = filelock_apply(ctx, request, &spare, &garbage, &net, &unblocked);
            wake = unblocked && ctx->waiters;
            break;
        }
        if (!wait) {
            ret = -EAGAIN;
            break;
        }
        if (signal_has_pending_current()) {
            ret = -EINTR;
            break;
        }
        if (!request->ofd && !filelock_waiter_add(&waiter, blocker->owner)) {
            ret = -EDEADLK;
            break;
        }

        /* Join the wait queue before dropping the lock, so an unlock cannot slip in unseen. */
        ctx->waiters++;
        wait_queue_prepare(&ctx->wait);
        spin_unlock(&ctx->lock);
        wait_queue_sleep();
        spin_lock(&ctx->lock);
        ctx->waiters--;
        if (!request->ofd) filelock_waiter_remove(&waiter);
    }
    spin_unlock(&ctx->lock);

    if (wake) wait_queue_wake_all(&ctx->wait);
    filelock_free_chain(garbage);
    filelock_free_chain(spare);
    filelock_ctx_settle(ctx, net);
    return ret;
}

/* Drop every lock an owner holds on one inode. */
void filelock_release(const filelock_key_t *key, const void *owner)
{
    if (!__atomic_load_n(&filelock_ctx_count, __ATOMIC_RELAXED)) return;

    filelock_ctx_t *ctx = filelock_ctx_acquire(key, false);
    if (!ctx) return;

    file_lock_t *garbage = NULL;
    int64_t      net     = 0;

    spin_lock(&ctx->lock);
    file_lock_t **link = &ctx->locks;
    while (*link) {
        file_lock_t *lock = *link;
        if (lock->owner != owner) {
            link = &lock->next;
            continue;
        }
        *link      = lock->next;
        lock->next = garbage;
        garbage    = lock;
        net--;
    }
    bool wake = net != 0 && ctx->waiters;
    spin_unlock(&ctx->lock);

    if (wake) wait_queue_wake_all(&ctx->wait);
    filelock_free_chain(garbage);
    filelock_ctx_settle(ctx, net);
}
