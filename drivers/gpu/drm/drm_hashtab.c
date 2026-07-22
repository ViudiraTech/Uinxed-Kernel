/*
 *
 *      drm_hashtab.c
 *      DRM open hash table (used by magic authentication)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_hashtab.h>
#include <alloc.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

/*
 * container_of — obtain a pointer to the containing struct from a pointer
 * to one of its members. Equivalent to the Linux kernel macro.
 */
#define container_of(ptr, type, member) ((type *)((uint8_t *)(ptr)-offsetof(type, member)))

/* Linux hash_long multiplier for 64-bit keys. */
#define HT_HASH_MULT 0x9e370001UL

/* Compute the bucket index for @key using multiplicative hashing. */
static inline unsigned int ht_hash(struct drm_open_hash *ht, unsigned long key)
{
    return (unsigned int)((key * HT_HASH_MULT) >> (64U - ht->order));
}

/* Create a hash table with 2^order buckets. Returns 0 or -ENOMEM. */
int drm_ht_create(struct drm_open_hash *ht, unsigned int order)
{
    unsigned int i;

    ht->size = 1U << order;
    ht->order = order;
    ht->table = (ilist_node_t *)malloc(ht->size * sizeof(ilist_node_t));
    if (ht->table == NULL) {
        return -ENOMEM;
    }

    for (i = 0; i < ht->size; i++) {
        ilist_init(&ht->table[i]);
    }

    return 0;
}

/* Destroy a hash table (entries are not freed). */
void drm_ht_destroy(struct drm_open_hash *ht)
{
    free(ht->table);
    memset(ht, 0, sizeof(*ht));
}

/* Insert @item keyed by item->key. Returns 0 or -EINVAL/-ENOMEM. */
int drm_ht_insert_item(struct drm_open_hash *ht, struct drm_hash_item *item)
{
    unsigned int idx;
    ilist_node_t *head, *cur;

    idx = ht_hash(ht, item->key);
    head = &ht->table[idx];

    /* Walk the bucket to check for duplicate keys. */
    for (cur = head->next; cur != head; cur = cur->next) {
        struct drm_hash_item *existing = container_of(cur, struct drm_hash_item, link);

        if (existing->key == item->key) {
            return -EINVAL;
        }
    }

    ilist_insert_after(head, &item->link);
    return 0;
}

/* Test whether (*item)->key is present; if so set *item to it. Returns 0 or -EINVAL. */
int drm_ht_peek(struct drm_open_hash *ht, struct drm_hash_item **item)
{
    unsigned int idx;
    unsigned long key;
    ilist_node_t *head, *cur;

    key = (*item)->key;
    idx = ht_hash(ht, key);
    head = &ht->table[idx];

    for (cur = head->next; cur != head; cur = cur->next) {
        struct drm_hash_item *candidate = container_of(cur, struct drm_hash_item, link);

        if (candidate->key == key) {
            *item = candidate;
            return 0;
        }
    }

    return -EINVAL;
}

/* Find an item by key. Returns 0 or -EINVAL. */
int drm_ht_find_item(struct drm_open_hash *ht, unsigned long key, struct drm_hash_item **item)
{
    unsigned int idx;
    ilist_node_t *head, *cur;

    idx = ht_hash(ht, key);
    head = &ht->table[idx];

    for (cur = head->next; cur != head; cur = cur->next) {
        struct drm_hash_item *candidate = container_of(cur, struct drm_hash_item, link);

        if (candidate->key == key) {
            *item = candidate;
            return 0;
        }
    }

    return -EINVAL;
}

/* Remove @item from the table. Returns 0 or -EINVAL. */
int drm_ht_remove_item(struct drm_open_hash *ht, struct drm_hash_item *item)
{
    (void)ht;

    if (item->link.prev == NULL) {
        return -EINVAL;
    }

    ilist_remove(&item->link);
    return 0;
}