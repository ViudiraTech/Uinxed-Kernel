/*
 *
 *      drm_hashtab.c
 *      DRM open hash table (used by magic authentication)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_hashtab.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

/* Allocate a bucket array of 2^order entries. */
int drm_ht_create(struct drm_open_hash *ht, unsigned int order)
{
    uint32_t size;

    if (!ht || order >= 32) return -EINVAL;

    size      = 1u << order;
    ht->table = malloc((size_t)size * sizeof(ilist_node_t));
    if (!ht->table) {
        plogk("drm_hashtab: Create failed: out of memory (%u buckets)\n", size);
        return -ENOMEM;
    }
    ht->size  = size;
    ht->order = order;
    for (uint32_t i = 0; i < size; i++) ilist_init(&ht->table[i]);
    return 0;
}

/* Release the bucket array; chained items are owned by the caller. */
void drm_ht_destroy(struct drm_open_hash *ht)
{
    if (ht && ht->table) {
        free(ht->table);
        ht->table = NULL;
        ht->size  = 0;
        ht->order = 0;
    }
}

/* Insert @item at the head of its bucket; duplicate keys are rejected. */
int drm_ht_insert_item(struct drm_open_hash *ht, struct drm_hash_item *item)
{
    ilist_node_t *bucket;
    ilist_node_t *node;

    if (!ht || !ht->table || !item) return -EINVAL;

    bucket = &ht->table[item->key & (ht->size - 1)];
    for (node = bucket->next; node && node != bucket; node = node->next) {
        struct drm_hash_item *hit = container_of(node, struct drm_hash_item, link);
        if (hit->key == item->key) {
            plogk("drm_hashtab: Insert: duplicate key 0x%lx\n", item->key);
            return -EINVAL;
        }
    }
    ilist_insert_after(bucket, &item->link);
    return 0;
}

/* Test whether (*item)->key is present; if so store the table entry in *item. */
int drm_ht_peek(struct drm_open_hash *ht, struct drm_hash_item **item)
{
    if (!ht || !ht->table || !item || !*item) return -EINVAL;
    return drm_ht_find_item(ht, (*item)->key, item);
}

/* Locate an item by key. */
int drm_ht_find_item(struct drm_open_hash *ht, unsigned long key, struct drm_hash_item **item)
{
    ilist_node_t *bucket;
    ilist_node_t *node;

    if (!ht || !ht->table || !item) return -EINVAL;

    bucket = &ht->table[key & (ht->size - 1)];
    for (node = bucket->next; node && node != bucket; node = node->next) {
        struct drm_hash_item *hit = container_of(node, struct drm_hash_item, link);
        if (hit->key == key) {
            *item = hit;
            return 0;
        }
    }
    plogk("drm_hashtab: Find: key 0x%lx not found.\n", key);
    return -EINVAL;
}

/* Unlink @item from its bucket. */
int drm_ht_remove_item(struct drm_open_hash *ht, struct drm_hash_item *item)
{
    ilist_node_t *bucket;
    ilist_node_t *node;

    if (!ht || !ht->table || !item) return -EINVAL;

    bucket = &ht->table[item->key & (ht->size - 1)];
    for (node = bucket->next; node && node != bucket; node = node->next) {
        if (node == &item->link) {
            ilist_remove(&item->link);
            return 0;
        }
    }
    plogk("drm_hashtab: Remove: item is not linked in the table.\n");
    return -EINVAL;
}
