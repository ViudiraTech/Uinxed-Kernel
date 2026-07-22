/*
 *
 *      drm_mm.h
 *      DRM range allocator (drm_mm)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux drm_mm API (include/drm/drm_mm.h). Backed by
 *  an augmented red-black tree keyed by node start, with each subtree
 *  tracking its maximum end (`__subtree_last`) so range queries can
 *  short-circuit. Allocation uses the scan helper to walk holes in
 *  [start, end] in best-fit / first-fit / top-down order.
 *
 */

#ifndef INCLUDE_DRM_DRM_MM_H_
#define INCLUDE_DRM_DRM_MM_H_

#include <rbtree.h>
#include <spin_lock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Allocation policy for drm_mm_insert_node_in_range(). */
enum drm_mm_insert_mode {
    DRM_MM_INSERT_DEFAULT = 0,
    DRM_MM_INSERT_LOW     = 1, /* bottom-up search */
    DRM_MM_INSERT_HIGH    = 2, /* top-down search */
    DRM_MM_INSERT_TOPDOWN = DRM_MM_INSERT_HIGH,
    DRM_MM_INSERT_BEST    = 3, /* best-fit search */
    DRM_MM_INSERT_ONCE    = 4, /* only attempt the first candidate hole */
    DRM_MM_INSERT_EVICT   = 5, /* placeholder for color eviction */
};

#define DRM_MM_INSERT_MODE_FLAGS (DRM_MM_INSERT_ONCE | DRM_MM_INSERT_EVICT)

struct drm_mm;

struct drm_mm_node {
    rb_node_t        rb;              /* node in the range interval tree */
    uint64_t         start;           /* inclusive start offset (bytes) */
    uint64_t         size;            /* length in bytes */
    uint64_t         __subtree_last;  /* augmented: max end in subtree */
    bool             allocated;       /* true once inserted into the tree */
    bool             scanned_block;   /* blocked during an active scan */
    unsigned long    color;           /* user-defined marker */
    struct drm_mm   *mm;              /* owning allocator */
};

struct drm_mm_scan {
    uint64_t                  size;
    uint64_t                  alignment;
    uint64_t                  range_start;
    uint64_t                  range_end;
    uint64_t                  hit_start; /* first unused byte in current hole */
    uint64_t                  hit_end;   /* last+1 unused byte in current hole */
    enum drm_mm_insert_mode   mode;
    unsigned long             color;
    bool                      check_range;
    bool                      once;
};

struct drm_mm {
    rb_root_t      interval_tree;
    spinlock_t     lock;
    uint64_t       start;          /* allocator range start */
    uint64_t       size;           /* allocator range size */
    uint64_t       alignment;
    unsigned long  scan_active;    /* >0 while a scan is in progress */
};

#define drm_mm_initialized(mm) ((mm)->size != 0)

static inline uint64_t drm_mm_node_end(const struct drm_mm_node *node)
{
    return node->start + node->size;
}

static inline bool drm_mm_node_allocated(const struct drm_mm_node *node)
{
    return node->allocated;
}

/* Initialize allocator over [start, start+size). */
void drm_mm_init(struct drm_mm *mm, uint64_t start, uint64_t size);

/* Tear down allocator (nodes must be removed first). */
void drm_mm_clean(struct drm_mm *mm);

/* True if no nodes are currently allocated. */
bool drm_mm_clean_check(const struct drm_mm *mm);

/*
 * Insert @node of @size honoring @alignment and mode, restricted to
 * [range_start, range_end). Returns 0 or -ENOSPC/-EINVAL.
 */
int drm_mm_insert_node_in_range(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size, uint64_t alignment,
                                uint64_t range_start, uint64_t range_end, enum drm_mm_insert_mode mode);

/* Insert over the full allocator range with default alignment. */
int drm_mm_insert_node(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size);

/* Remove an allocated node. */
void drm_mm_remove_node(struct drm_mm_node *node);

/* Replace @old with @new, preserving range and color. */
void drm_mm_replace_node(struct drm_mm_node *old, struct drm_mm_node *new_node);

/* Initialize a scan for an allocation of @size over the whole allocator. */
void drm_mm_init_scan(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                      enum drm_mm_insert_mode mode);

/* Initialize a scan constrained to [range_start, range_end). */
void drm_mm_init_scan_with_range(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                                 uint64_t range_start, uint64_t range_end, enum drm_mm_insert_mode mode);

/*
 * Block @node during a scan. Returns true if @node could have been an
 * allocation target and was therefore marked scanned; false otherwise.
 */
bool drm_mm_scan_add_block(struct drm_mm_scan *scan, struct drm_mm_node *node);

/* Un-block @node previously blocked by drm_mm_scan_add_block(). */
void drm_mm_scan_remove_block(struct drm_mm_scan *scan, struct drm_mm_node *node);

/* Finalize a scan: returns true if the candidate hole was used. */
bool drm_mm_scan_color_evict(struct drm_mm_scan *scan);

/* Lowest allocated node, or NULL if the tree is empty. */
struct drm_mm_node *drm_mm_first(const struct drm_mm *mm);

/* In-order successor of @node, or NULL. */
struct drm_mm_node *drm_mm_next(const struct drm_mm_node *node);

/* Iterate allocated nodes in ascending start order. */
#define drm_mm_for_each_node(entry, mm) \
    for ((entry) = drm_mm_first(mm); (entry) != NULL; (entry) = drm_mm_next(entry))

#endif /* INCLUDE_DRM_DRM_MM_H_ */
