/*
 *
 *      drm_hashtab.h
 *      DRM open hash table (used by magic authentication)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux drm_hashtab API (drivers/gpu/drm/drm_hashtab.c).
 *  Chained hash table keyed by unsigned long; the table grows as a power
 *  of two fixed at creation time, matching the legacy DRM auth use case.
 *
 */

#ifndef INCLUDE_DRM_DRM_HASHTAB_H_
#define INCLUDE_DRM_DRM_HASHTAB_H_

#include <libs/glist/intrusive_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

struct drm_hash_item {
        ilist_node_t  link;
        unsigned long key;
};

struct drm_open_hash {
        unsigned int  size;  // number of buckets
        unsigned int  order; // log2(size)
        ilist_node_t *table; // bucket array (each a list head)
};

/* Create a hash table with 2^order buckets. Returns 0 or -ENOMEM. */
int drm_ht_create(struct drm_open_hash *ht, unsigned int order);

/* Destroy a hash table (entries are not freed). */
void drm_ht_destroy(struct drm_open_hash *ht);

/* Insert @item keyed by item->key. Returns 0 or -EINVAL/-ENOMEM. */
int drm_ht_insert_item(struct drm_open_hash *ht, struct drm_hash_item *item);

/* Test whether item->key is present; if so set *item to it. Returns 0 or -EINVAL. */
int drm_ht_peek(struct drm_open_hash *ht, struct drm_hash_item **item);

/* Find an item by key. Returns 0 or -EINVAL. */
int drm_ht_find_item(struct drm_open_hash *ht, unsigned long key, struct drm_hash_item **item);

/* Remove @item from the table. Returns 0 or -EINVAL. */
int drm_ht_remove_item(struct drm_open_hash *ht, struct drm_hash_item *item);

#endif /* INCLUDE_DRM_DRM_HASHTAB_H_ */
