/*
 *
 *      drm_modeset_lock.h
 *      DRM modeset locking (acquire-context based)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux drm_modeset_lock API (include/drm/drm_modeset_lock.h).
 *  Implemented on top of the kernel spinlock primitive: each lock tracks an
 *  owning acquire context so atomic helpers can perform deadlock-aware
 *  backoff (-EDEADLK). Recursive acquisition by the same context succeeds.
 *
 */

#ifndef INCLUDE_DRM_DRM_MODESET_LOCK_H_
#define INCLUDE_DRM_DRM_MODESET_LOCK_H_

#include <intrusive_list.h>
#include <spin_lock.h>
#include <stdbool.h>
#include <stdint.h>

struct drm_device;

struct drm_modeset_lock {
    spinlock_t                   mutex; // serializes ownership transitions
    struct drm_modeset_acquire_ctx *ctx; // owning context, NULL when free
    ilist_node_t                 link;  // node in ctx->locked list
};

#define DRM_MODESET_LOCK_INIT(lock) \
    do {                            \
        (lock)->mutex.lock = 0;     \
        (lock)->ctx      = NULL;    \
    } while (0)

struct drm_modeset_acquire_ctx {
    spinlock_t             ctx_lock;        // protects this ctx's bookkeeping
    struct drm_modeset_lock *contended_lock; // lock that caused last -EDEADLK
    ilist_node_t           locked;          // head of held drm_modeset_lock.link
    bool                   trylock_only;
    bool                   interruptible;
    int                    num_locks;
};

/* Initialize a modeset lock. */
void drm_modeset_lock_init(struct drm_modeset_lock *lock);

/* Acquire @lock under @ctx. Returns 0, -EDEADLK, or -EINTR (interruptible). */
int drm_modeset_lock(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx);

int drm_modeset_lock_interruptible(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx);

/* Release @lock. */
void drm_modeset_unlock(struct drm_modeset_lock *lock);

/* Acquire without an acquire ctx (simple lock/unlock). Returns 0 or -EBUSY. */
int drm_modeset_lock_single_interruptible(struct drm_modeset_lock *lock);

/* True if @lock is currently held. */
bool drm_modeset_is_locked(struct drm_modeset_lock *lock);

/* Initialize an acquire context. @flags reserved (0). */
void drm_modeset_acquire_init(struct drm_modeset_acquire_ctx *ctx, uint32_t flags);

/* Finalize: drop any locks still held. */
void drm_modeset_acquire_fini(struct drm_modeset_acquire_ctx *ctx);

/* Drop all locks held by @ctx (used after -EDEADLK before retry). */
int drm_modeset_drop_locks(struct drm_modeset_acquire_ctx *ctx);

/* Resolve a -EDEADLK: drop locks, lock the contended lock, return 0. */
int drm_modeset_backoff(struct drm_modeset_acquire_ctx *ctx);

/* Lock all KMS modeset locks of @dev under @ctx. Returns 0 or -EDEADLK/-EINTR. */
int drm_modeset_lock_all_ctx(struct drm_device *dev, struct drm_modeset_acquire_ctx *ctx);

#endif /* INCLUDE_DRM_DRM_MODESET_LOCK_H_ */
