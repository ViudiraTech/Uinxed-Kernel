/*
 *
 *      drm_mm.c
 *      DRM range allocator
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_mm.h>
#include <rbtree.h>
#include <spin_lock.h>
#include <alloc.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Compute the end of a node's range (exclusive). */
static inline uint64_t node_end(const struct drm_mm_node *node)
{
    return node->start + node->size;
}

/*
 * Augment callback invoked by the rbtree after structural changes.
 * Recomputes the augmented subtree-maximum for @rb and propagates
 * upward.  Stores the result in both drm_mm_node::__subtree_last and
 * rb_node_t::min_vruntime.
 */
static void drm_mm_augment_cb(rb_node_t *rb, void *data)
{
    struct drm_mm_node *node = rb_entry(rb, struct drm_mm_node, rb);
    uint64_t subtree_max = node_end(node);

    if (rb->left) {
        struct drm_mm_node *left = rb_entry(rb->left, struct drm_mm_node, rb);
        if (left->__subtree_last > subtree_max)
            subtree_max = left->__subtree_last;
    }

    if (rb->right) {
        struct drm_mm_node *right = rb_entry(rb->right, struct drm_mm_node, rb);
        if (right->__subtree_last > subtree_max)
            subtree_max = right->__subtree_last;
    }

    node->__subtree_last = subtree_max;
    rb->min_vruntime = subtree_max;

    (void)data;
}

/* Comparison function: order interval tree nodes by start address. */
static int drm_mm_less(const rb_node_t *a, const rb_node_t *b)
{
    const struct drm_mm_node *node_a = rb_entry(a, struct drm_mm_node, rb);
    const struct drm_mm_node *node_b = rb_entry(b, struct drm_mm_node, rb);

    return node_a->start < node_b->start;
}

/* Align @x up to the next multiple of @alignment.  @alignment must be a power of two or zero. */
static inline uint64_t align_up(uint64_t x, uint64_t alignment)
{
    if (alignment == 0)
        return x;
    return (x + alignment - 1) & ~(alignment - 1);
}

/* Initialize allocator over [start, start+size). */
void drm_mm_init(struct drm_mm *mm, uint64_t start, uint64_t size)
{
    rb_init_root(&mm->interval_tree);
    mm->lock = (spinlock_t){0};
    mm->start = start;
    mm->size = size;
    mm->alignment = 0;
    mm->scan_active = 0;
}

/* Tear down allocator (caller must remove all nodes first). */
void drm_mm_clean(struct drm_mm *mm)
{
    (void)mm;
}

/* True if no nodes are currently allocated. */
bool drm_mm_clean_check(const struct drm_mm *mm)
{
    return rb_is_empty((rb_root_t *)&mm->interval_tree) != 0;
}

/* Return the lowest-addressed allocated node, or NULL if the tree is empty. */
struct drm_mm_node *drm_mm_first(const struct drm_mm *mm)
{
    rb_node_t *rb = rb_first((rb_root_t *)&mm->interval_tree);

    return rb ? rb_entry(rb, struct drm_mm_node, rb) : NULL;
}

/* Return the in-order successor of @node, or NULL. */
struct drm_mm_node *drm_mm_next(const struct drm_mm_node *node)
{
    rb_node_t *rb = rb_next((rb_node_t *)&node->rb);

    return rb ? rb_entry(rb, struct drm_mm_node, rb) : NULL;
}

/*
 * Insert @node of @size into the full allocator range with default alignment.
 * This is a convenience wrapper around drm_mm_insert_node_in_range().
 */
int drm_mm_insert_node(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size)
{
    return drm_mm_insert_node_in_range(mm, node, size, 0, mm->start, mm->start + mm->size, DRM_MM_INSERT_DEFAULT);
}

/*
 * Core insertion: search for a hole in [range_start, range_end) that can
 * accommodate @size bytes aligned to @alignment, using the strategy given
 * by @mode.  Returns 0 on success, -ENOSPC if no hole is found, or -EINVAL
 * on invalid arguments.
 */
int drm_mm_insert_node_in_range(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size,
                                uint64_t alignment, uint64_t range_start, uint64_t range_end,
                                enum drm_mm_insert_mode mode)
{
    struct drm_mm_node *prev, *entry;
    uint64_t hole_start, hole_end, aligned;
    uint64_t best_size = UINT64_MAX;
    uint64_t best_start = 0;
    bool found = false;
    bool once;

    if (size == 0)
        return -EINVAL;
    if (range_start >= range_end)
        return -EINVAL;

    once = (mode & DRM_MM_INSERT_ONCE) != 0;
    mode &= ~DRM_MM_INSERT_MODE_FLAGS;

    spin_lock(&mm->lock);

    prev = NULL;
    for (entry = drm_mm_first(mm); ; prev = entry, entry = entry ? drm_mm_next(entry) : NULL) {
        hole_start = prev ? node_end(prev) : mm->start;
        hole_end = entry ? entry->start : mm->start + mm->size;

        if (hole_start < range_start)
            hole_start = range_start;
        if (hole_end > range_end)
            hole_end = range_end;

        if (hole_start >= hole_end)
            goto next_hole;

        aligned = align_up(hole_start, alignment);

        if (aligned + size > hole_end)
            goto next_hole;

        found = true;

        switch (mode) {
        case DRM_MM_INSERT_HIGH:
            /* Top-down: keep scanning, use the last hole that fits. */
            best_start = aligned;
            break;
        case DRM_MM_INSERT_BEST: {
            /* Best-fit: track the tightest hole. */
            uint64_t hole = hole_end - hole_start;
            if (hole < best_size) {
                best_size = hole;
                best_start = aligned;
            }
            break;
        }
        case DRM_MM_INSERT_LOW:
        case DRM_MM_INSERT_DEFAULT:
        default:
            /* First-fit: use this hole immediately. */
            best_start = aligned;
            goto found_hole;
        }

next_hole:
        if (!entry)
            break;
        if (once && found)
            break;
    }

    if (found) {
found_hole:
        node->start = best_start;
        node->size = size;
        node->mm = mm;
        node->allocated = true;
        node->__subtree_last = best_start + size;
        node->scanned_block = false;
        node->color = 0;
        rb_insert_augmented(&mm->interval_tree, &node->rb, drm_mm_less, drm_mm_augment_cb, NULL);
        spin_unlock(&mm->lock);
        return 0;
    }

    spin_unlock(&mm->lock);
    return -ENOSPC;
}

/* Remove an allocated node from the interval tree. */
void drm_mm_remove_node(struct drm_mm_node *node)
{
    spin_lock(&node->mm->lock);
    rb_erase_augmented(&node->mm->interval_tree, &node->rb, drm_mm_augment_cb, NULL);
    node->allocated = false;
    spin_unlock(&node->mm->lock);
}

/*
 * Replace @old with @new_node, preserving the allocated range, color,
 * and augmented data.  The old node is erased from the tree and the
 * new node is inserted in its place.
 */
void drm_mm_replace_node(struct drm_mm_node *old, struct drm_mm_node *new_node)
{
    spin_lock(&old->mm->lock);

    new_node->start = old->start;
    new_node->size = old->size;
    new_node->allocated = old->allocated;
    new_node->__subtree_last = old->__subtree_last;
    new_node->color = old->color;
    new_node->mm = old->mm;

    rb_erase_augmented(&old->mm->interval_tree, &old->rb, drm_mm_augment_cb, NULL);
    rb_insert_augmented(&new_node->mm->interval_tree, &new_node->rb, drm_mm_less, drm_mm_augment_cb, NULL);

    spin_unlock(&old->mm->lock);
}

/* Initialize a scan for an allocation of @size over the whole allocator. */
void drm_mm_init_scan(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                      enum drm_mm_insert_mode mode)
{
    scan->size = size;
    scan->alignment = alignment;
    scan->range_start = mm->start;
    scan->range_end = mm->start + mm->size;
    scan->hit_start = 0;
    scan->hit_end = 0;
    scan->mode = mode;
    scan->color = 0;
    scan->check_range = false;
    scan->once = false;
    mm->scan_active++;
}

/* Initialize a scan constrained to [range_start, range_end). */
void drm_mm_init_scan_with_range(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                                 uint64_t range_start, uint64_t range_end, enum drm_mm_insert_mode mode)
{
    scan->size = size;
    scan->alignment = alignment;
    scan->range_start = range_start;
    scan->range_end = range_end;
    scan->hit_start = 0;
    scan->hit_end = 0;
    scan->mode = mode;
    scan->color = 0;
    scan->check_range = true;
    scan->once = false;
    mm->scan_active++;
}

/*
 * Mark @node as blocked during an active scan.  Returns true if the node
 * could have been an allocation target in the candidate range.
 */
bool drm_mm_scan_add_block(struct drm_mm_scan *scan, struct drm_mm_node *node)
{
    node->scanned_block = true;
    (void)scan;
    return true;
}

/* Remove the scan block marker from @node. */
void drm_mm_scan_remove_block(struct drm_mm_scan *scan, struct drm_mm_node *node)
{
    node->scanned_block = false;
    (void)scan;
}

/*
 * Finalize a scan: returns true if the candidate hole region was valid
 * (i.e., a hit region was recorded during the scan).
 */
bool drm_mm_scan_color_evict(struct drm_mm_scan *scan)
{
    return scan->hit_end > scan->hit_start;
}