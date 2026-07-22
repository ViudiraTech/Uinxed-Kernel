/*
 *
 *      drm_modeset_lock.c
 *      DRM modeset locking (acquire-context based)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux drm_modeset_lock API (drivers/gpu/drm/drm_modeset_lock.c).
 *
 *  Each drm_modeset_lock wraps a spinlock that is held across the whole
 *  ownership interval (acquire-to-release) and records the owning acquire
 *  context. An acquire context tracks every lock it holds so that atomic
 *  helpers can perform deadlock-aware backoff: when a lock is already owned by
 *  another context, drm_modeset_lock() returns -EDEADLK instead of blocking;
 *  the caller then drops its held locks via drm_modeset_backoff(), acquires the
 *  contended lock, and retries the original acquisition sequence.
 *
 *  Lock ordering: a lock's mutex (lock->mutex) is the outer lock and the
 *  context bookkeeping lock (ctx->ctx_lock) is the innermost lock. ctx->ctx_lock
 *  is never held while acquiring any lock->mutex, and it is never held across a
 *  call that itself acquires it, so the acquisition graph stays acyclic. An
 *  acquire context is owned by a single thread, so its held-lock list is not
 *  modified concurrently; ctx->ctx_lock is nonetheless taken for every
 *  bookkeeping update to keep the contract explicit and future-proof.
 *
 */

#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_modeset_lock.h>
#include <kernel/errno.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* Lock primitives                                                    */
/* ------------------------------------------------------------------ */

/* Initialize @lock as free and ready for use. The list link is made a valid
 * empty sentinel so drm_modeset_unlock() can remove it safely even if it was
 * never inserted into a context's held-lock list. */
void drm_modeset_lock_init(struct drm_modeset_lock *lock)
{
    lock->mutex.lock   = 0;
    lock->mutex.rflags = 0;
    lock->ctx          = NULL;
    ilist_init(&lock->link);
}

/*
 * Acquire @lock on behalf of acquire context @ctx.
 *
 *  - @ctx == NULL: take the underlying mutex directly with no deadlock
 *    tracking. The lock is released with drm_modeset_unlock().
 *  - @lock is already owned by @ctx: recursive acquisition, succeed at once.
 *  - @lock is free: take the mutex, mark @ctx as owner and link the lock into
 *    the context's held-lock list.
 *  - @lock is owned by another context: record the contended lock in @ctx and
 *    return -EDEADLK so the caller can drm_modeset_backoff() and retry.
 *
 * The mutex is held across the whole ownership interval. Returns 0 on success
 * or -EDEADLK on contention (only possible when @ctx != NULL).
 */
int drm_modeset_lock(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx)
{
    if (ctx == NULL) {
        spin_lock(&lock->mutex);
        lock->ctx = NULL;
        return 0;
    }

    /* Recursive acquisition by the same context is a no-op. */
    if (lock->ctx == ctx) return 0;

    if (lock->ctx == NULL) {
        spin_lock(&lock->mutex);
        /*
         * The mutex serializes ownership transitions: once we hold it, the
         * previous owner (if any) has already cleared ->ctx in its unlock path,
         * so it is safe to claim the lock for this context.
         */
        spin_lock(&ctx->ctx_lock);
        lock->ctx = ctx;
        ilist_insert_after(&ctx->locked, &lock->link);
        ctx->num_locks++;
        spin_unlock(&ctx->ctx_lock);
        return 0;
    }

    /* Held by a different context: record the contention and let the caller back off. */
    spin_lock(&ctx->ctx_lock);
    ctx->contended_lock = lock;
    spin_unlock(&ctx->ctx_lock);
    return -EDEADLK;
}

/*
 * Interruptible variant of drm_modeset_lock(). This kernel does not yet have a
 * signal/interrupt framework, so the operation cannot be interrupted and the
 * result is identical to drm_modeset_lock(). Returns 0, -EDEADLK, or (in the
 * future) -EINTR.
 */
int drm_modeset_lock_interruptible(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx)
{
    return drm_modeset_lock(lock, ctx);
}

/*
 * Release @lock. If it was tracked by an acquire context, detach it from that
 * context's held-lock list and clear the ownership marker before dropping the
 * mutex. Bare locks (acquired without a context) simply drop the mutex.
 */
void drm_modeset_unlock(struct drm_modeset_lock *lock)
{
    struct drm_modeset_acquire_ctx *ctx = lock->ctx;

    if (ctx != NULL) {
        spin_lock(&ctx->ctx_lock);
        lock->ctx = NULL;
        ilist_remove(&lock->link);
        spin_unlock(&ctx->ctx_lock);
        spin_unlock(&lock->mutex);
    } else {
        spin_unlock(&lock->mutex);
    }
}

/*
 * Try to acquire @lock without an acquire context. This is a non-blocking
 * attempt: it succeeds only if the lock is currently free. Returns 0 on
 * success or -EBUSY if the lock is already held. No interrupt framework is
 * present, so -EINTR is never produced.
 *
 * The test-and-acquire uses the same atomic exchange primitive as spin_lock()
 * so it is race-free and never busy-waits. On success the saved rflags are
 * stored in the lock so a subsequent drm_modeset_unlock() restores them.
 */
int drm_modeset_lock_single_interruptible(struct drm_modeset_lock *lock)
{
    uint64_t rflags;
    uint64_t desired = 1;

    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    __asm__ volatile("lock xchg %[desired], %[lock];" : [lock] "+m"(lock->mutex.lock), [desired] "+r"(desired)::"memory");

    if (desired != 0) {
        /* Already held: nothing was claimed, just restore interrupt state. */
        __asm__ volatile("push %0; popfq" ::"r"(rflags));
        return -EBUSY;
    }

    lock->mutex.rflags = rflags;
    lock->ctx          = NULL;
    return 0;
}

/* Return true if @lock is currently held, either by an acquire context or as a
 * bare spinlock. */
bool drm_modeset_is_locked(struct drm_modeset_lock *lock)
{
    return lock->ctx != NULL || lock->mutex.lock != 0;
}

/* ------------------------------------------------------------------ */
/* Acquire context                                                    */
/* ------------------------------------------------------------------ */

/*
 * Initialize acquire context @ctx. @flags is reserved; bit 0 selects
 * interruptible mode (currently advisory only, since no interrupt framework
 * exists). The context starts with no held locks and no pending contention.
 */
void drm_modeset_acquire_init(struct drm_modeset_acquire_ctx *ctx, uint32_t flags)
{
    ctx->ctx_lock.lock   = 0;
    ctx->ctx_lock.rflags = 0;
    ilist_init(&ctx->locked);
    ctx->contended_lock = NULL;
    ctx->trylock_only   = false;
    ctx->interruptible  = (flags & 0x1U) ? true : false;
    ctx->num_locks      = 0;
}

/* Finalize @ctx: drop any locks still held. After this the context may be
 * reused by another drm_modeset_acquire_init() or freed. */
void drm_modeset_acquire_fini(struct drm_modeset_acquire_ctx *ctx)
{
    drm_modeset_drop_locks(ctx);
}

/*
 * Drop every lock currently held by @ctx, iterating the held-lock list in
 * reverse acquisition order, then reset the lock count to zero. The
 * contended_lock field is left untouched so drm_modeset_backoff() can still
 * resolve a pending -EDEADLK after this call. Returns 0.
 *
 * The traversal does not hold ctx->ctx_lock across drm_modeset_unlock() (which
 * takes that lock itself): an acquire context is owned by a single thread, so
 * its held-lock list is not modified concurrently.
 */
int drm_modeset_drop_locks(struct drm_modeset_acquire_ctx *ctx)
{
    ilist_node_t *node = ctx->locked.prev;

    while (node != &ctx->locked) {
        ilist_node_t            *prev = node->prev;
        struct drm_modeset_lock *lock = (struct drm_modeset_lock *)((uintptr_t)node - offsetof(struct drm_modeset_lock, link));

        drm_modeset_unlock(lock);
        node = prev;
    }

    spin_lock(&ctx->ctx_lock);
    ctx->num_locks = 0;
    spin_unlock(&ctx->ctx_lock);
    return 0;
}

/*
 * Resolve a -EDEADLK reported by drm_modeset_lock(): drop every lock held by
 * @ctx, then acquire the contended lock (blocking until its current owner
 * releases it) so the caller can retry the original acquisition sequence with
 * that lock already in hand. Returns 0 on success.
 */
int drm_modeset_backoff(struct drm_modeset_acquire_ctx *ctx)
{
    struct drm_modeset_lock *contended = ctx->contended_lock;

    if (contended == NULL) return 0;

    /* Drop everything we currently hold; this preserves contended_lock. */
    drm_modeset_drop_locks(ctx);

    /*
     * Acquire the contended lock for this context. spin_lock blocks until the
     * other owner releases it (clearing ->ctx), after which we claim it. The
     * mutex is held until a subsequent drm_modeset_unlock().
     */
    spin_lock(&contended->mutex);
    spin_lock(&ctx->ctx_lock);
    contended->ctx = ctx;
    ilist_insert_after(&ctx->locked, &contended->link);
    ctx->num_locks++;
    ctx->contended_lock = NULL;
    spin_unlock(&ctx->ctx_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Device-wide locking                                                */
/* ------------------------------------------------------------------ */

/*
 * Acquire all KMS modeset locks of @dev under @ctx: the global mode_config
 * mutex plus every CRTC mutex. If any CRTC acquisition returns -EDEADLK, the
 * global mutex is released and -EDEADLK is returned so the caller can back off
 * and retry the whole sequence cleanly.
 *
 * Note: drm_mode_config.mutex is a plain spinlock_t in this codebase (not a
 * drm_modeset_lock), so it is taken directly with spin_lock() and is therefore
 * not tracked by @ctx; the caller must release it explicitly once the
 * transaction is complete. It is released on the error path so that retrying
 * after a backoff never self-deadlocks on it.
 */
int drm_modeset_lock_all_ctx(struct drm_device *dev, struct drm_modeset_acquire_ctx *ctx)
{
    ilist_node_t *node;
    int           ret;

    spin_lock(&dev->mode_config.mutex);

    for (node = dev->mode_config.crtc_list.next; node != &dev->mode_config.crtc_list; node = node->next) {
        struct drm_crtc *crtc = (struct drm_crtc *)((uintptr_t)node - offsetof(struct drm_crtc, head));

        ret = drm_modeset_lock(&crtc->mutex, ctx);
        if (ret) {
            spin_unlock(&dev->mode_config.mutex);
            return ret;
        }
    }

    return 0;
}
