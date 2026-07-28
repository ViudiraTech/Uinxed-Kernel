/*
 *
 *      drm_idr.h
 *      Integer ID allocator (radix-free hash-backed IDR)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Faithful subset of the Linux IDR API used by the DRM subsystem for
 *  mode-object and GEM-handle IDs. Backed by an open-addressing hash
 *  table for O(1) lookup/insert/remove; IDs are allocated monotonically
 *  above a caller-supplied lower bound.
 *
 */

#ifndef INCLUDE_DRM_DRM_IDR_H_
#define INCLUDE_DRM_DRM_IDR_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

struct drm_idr_entry {
        uint32_t id;
        void    *ptr;
};

struct drm_idr {
        spinlock_t            lock;     // protects buckets and counter
        uint32_t              next_id;  // monotonic hint
        struct drm_idr_entry *table;    // open-addressing bucket array
        uint32_t              capacity; // power-of-two bucket count
        uint32_t              count;    // live entries
};

#define DRM_IDR_INVALID 0U

/* Initialize an empty IDR. */
void drm_idr_init(struct drm_idr *idr);

/* Release all IDR storage. Entries are not freed (callers own them). */
void drm_idr_destroy(struct drm_idr *idr);

/*
 * Allocate a new id in [start, end) bound to @ptr.
 * Returns 0 and stores the id in *@id_out, or a negative errno on failure.
 */
int drm_idr_alloc(struct drm_idr *idr, void *ptr, uint32_t start, uint32_t end, uint32_t *id_out);

/* Allocate the specific @id; returns 0 or -EEXIST/-ENOMEM. */
int drm_idr_alloc_exact(struct drm_idr *idr, void *ptr, uint32_t id);

/* Look up the pointer bound to @id, or NULL if none. */
void *drm_idr_find(struct drm_idr *idr, uint32_t id);

/* Remove @id; returns the pointer that was bound or NULL. */
void *drm_idr_remove(struct drm_idr *idr, uint32_t id);

/* Replace the pointer bound to @id; returns the old pointer or NULL. */
void *drm_idr_replace(struct drm_idr *idr, void *ptr, uint32_t id);

/* Iterate every entry: fn returns 0 to continue, non-zero to stop. */
int drm_idr_for_each(struct drm_idr *idr, int (*fn)(uint32_t id, void *ptr, void *data), void *data);

#endif /* INCLUDE_DRM_DRM_IDR_H_ */
