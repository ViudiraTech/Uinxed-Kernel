/*
 *
 *      drm_idr.c
 *      Integer ID allocator (hash-backed IDR)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm_idr.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* Multiplicative hash constant for power-of-two table sizes (Knuth). */
#define IDR_HASH_MULT 2654435761U

/* Initial table capacity (must be power of two). */
#define IDR_INIT_CAPACITY 64U

/* Sentinel: id == 0 && ptr == NULL marks an empty slot. */
#define IDR_SLOT_EMPTY(e) ((e).id == 0U && (e).ptr == NULL)

/* Load factor threshold numerator / denominator; grow when count > capacity * 3 / 4. */
#define IDR_LOAD_NUM 3U
#define IDR_LOAD_DEN 4U

/* Compute the hash bucket index for a given id. */
static inline uint32_t idr_hash(uint32_t id, uint32_t capacity)
{
    return (id * IDR_HASH_MULT) & (capacity - 1U);
}

/*
 * Internal: linear-probe search for @id in the table.
 * Returns a pointer to the entry if found, or a pointer to the first empty
 * slot encountered during the probe if not found. If the table is full and
 * the id is not present, returns NULL.
 */
static struct drm_idr_entry *idr_probe(struct drm_idr *idr, uint32_t id)
{
    uint32_t idx, start;

    start = idr_hash(id, idr->capacity);
    for (idx = start;; idx = (idx + 1U) & (idr->capacity - 1U)) {
        struct drm_idr_entry *e = &idr->table[idx];

        if (e->id == id) { return e; }
        if (IDR_SLOT_EMPTY(*e)) { return e; }
        /* Wrapped around — table is full and id not present. */
        if (idx == ((start + idr->capacity - 1U) & (idr->capacity - 1U))) { break; }
    }
    return NULL;
}

/*
 * Internal: grow the table to double its current capacity, rehashing all
 * live entries. Returns 0 on success, -ENOMEM on allocation failure.
 *
 * Must be called with idr->lock held.
 */
static int idr_grow(struct drm_idr *idr)
{
    uint32_t              new_cap, i;
    struct drm_idr_entry *new_table;

    new_cap   = idr->capacity * 2U;
    new_table = (struct drm_idr_entry *)malloc(new_cap * sizeof(struct drm_idr_entry));
    if (new_table == NULL) { return -ENOMEM; }
    memset(new_table, 0, new_cap * sizeof(struct drm_idr_entry));

    /* Rehash all live entries into the new table. */
    for (i = 0; i < idr->capacity; i++) {
        struct drm_idr_entry *src = &idr->table[i];

        if (IDR_SLOT_EMPTY(*src)) { continue; }

        /* Linear-probe into the new table. */
        uint32_t idx = idr_hash(src->id, new_cap);

        while (!IDR_SLOT_EMPTY(new_table[idx])) { idx = (idx + 1U) & (new_cap - 1U); }
        new_table[idx] = *src;
    }

    free(idr->table);
    idr->table    = new_table;
    idr->capacity = new_cap;
    return 0;
}

/* Initialize an empty IDR. */
void drm_idr_init(struct drm_idr *idr)
{
    memset(idr, 0, sizeof(*idr));
    idr->table = (struct drm_idr_entry *)malloc(IDR_INIT_CAPACITY * sizeof(struct drm_idr_entry));
    if (idr->table == NULL) { return; }
    memset(idr->table, 0, IDR_INIT_CAPACITY * sizeof(struct drm_idr_entry));
    idr->capacity = IDR_INIT_CAPACITY;
    idr->next_id  = 1U;
}

/* Release all IDR storage. Entries are not freed (callers own them). */
void drm_idr_destroy(struct drm_idr *idr)
{
    spin_lock(&idr->lock);
    free(idr->table);
    memset(idr, 0, sizeof(*idr));
    spin_unlock(&idr->lock);
}

/* Look up the pointer bound to @id, or NULL if none. */
void *drm_idr_find(struct drm_idr *idr, uint32_t id)
{
    struct drm_idr_entry *e;
    void                 *ptr = NULL;

    if (id == DRM_IDR_INVALID) { return NULL; }

    spin_lock(&idr->lock);
    e = idr_probe(idr, id);
    if (e != NULL && e->id == id) { ptr = e->ptr; }
    spin_unlock(&idr->lock);
    return ptr;
}

/*
 * Allocate a new id in [start, end) bound to @ptr.
 * If @end is 0, the range is unbounded (up to UINT32_MAX).
 * Returns 0 and stores the id in *@id_out, or a negative errno on failure.
 */
int drm_idr_alloc(struct drm_idr *idr, void *ptr, uint32_t start, uint32_t end, uint32_t *id_out)
{
    uint32_t id, effective_end;
    int      ret;

    if (end == 0U) {
        effective_end = UINT32_MAX;
    } else {
        effective_end = end;
    }

    spin_lock(&idr->lock);

    /* Grow if load factor would exceed 3/4. */
    if (idr->count * IDR_LOAD_DEN >= idr->capacity * IDR_LOAD_NUM) {
        ret = idr_grow(idr);
        if (ret != 0) {
            spin_unlock(&idr->lock);
            return ret;
        }
    }

    id = start;
    if (id < idr->next_id) { id = idr->next_id; }

    for (; id < effective_end; id++) {
        uint32_t idx = idr_hash(id, idr->capacity);

        /* Linear-probe for an empty slot. */
        for (;; idx = (idx + 1U) & (idr->capacity - 1U)) {
            struct drm_idr_entry *e = &idr->table[idx];

            if (e->id == id) {
                /* This id is already occupied; try the next candidate id. */
                break;
            }
            if (IDR_SLOT_EMPTY(*e)) {
                /* Found an empty slot — claim it. */
                e->id  = id;
                e->ptr = ptr;
                idr->count++;
                idr->next_id = id + 1U;
                *id_out      = id;
                spin_unlock(&idr->lock);
                return 0;
            }
        }
    }

    spin_unlock(&idr->lock);
    return -ENOSPC;
}

/* Allocate the specific @id; returns 0 or -EEXIST/-ENOMEM. */
int drm_idr_alloc_exact(struct drm_idr *idr, void *ptr, uint32_t id)
{
    struct drm_idr_entry *e;
    int                   ret = 0;

    if (id == DRM_IDR_INVALID) { return -EINVAL; }

    spin_lock(&idr->lock);

    /* Grow if needed. */
    if (idr->count * IDR_LOAD_DEN >= idr->capacity * IDR_LOAD_NUM) {
        ret = idr_grow(idr);
        if (ret != 0) {
            spin_unlock(&idr->lock);
            return ret;
        }
    }

    e = idr_probe(idr, id);
    if (e == NULL) {
        /* Table is full and id not present. */
        spin_unlock(&idr->lock);
        return -ENOSPC;
    }

    if (e->id == id) {
        /* Already occupied. */
        spin_unlock(&idr->lock);
        return -EEXIST;
    }

    /* The probe returned an empty slot. */
    e->id  = id;
    e->ptr = ptr;
    idr->count++;

    spin_unlock(&idr->lock);
    return 0;
}

/* Remove @id; returns the pointer that was bound or NULL. */
void *drm_idr_remove(struct drm_idr *idr, uint32_t id)
{
    struct drm_idr_entry *e;
    void                 *ptr = NULL;

    if (id == DRM_IDR_INVALID) { return NULL; }

    spin_lock(&idr->lock);
    e = idr_probe(idr, id);
    if (e != NULL && e->id == id) {
        ptr    = e->ptr;
        e->id  = 0U;
        e->ptr = NULL;
        idr->count--;
    }
    spin_unlock(&idr->lock);
    return ptr;
}

/* Replace the pointer bound to @id; returns the old pointer or NULL. */
void *drm_idr_replace(struct drm_idr *idr, void *ptr, uint32_t id)
{
    struct drm_idr_entry *e;
    void                 *old = NULL;

    if (id == DRM_IDR_INVALID) { return NULL; }

    spin_lock(&idr->lock);
    e = idr_probe(idr, id);
    if (e != NULL && e->id == id) {
        old    = e->ptr;
        e->ptr = ptr;
    }
    spin_unlock(&idr->lock);
    return old;
}

/* Iterate every entry: fn returns 0 to continue, non-zero to stop. */
int drm_idr_for_each(struct drm_idr *idr, int (*fn)(uint32_t id, void *ptr, void *data), void *data)
{
    uint32_t i;
    int      ret = 0;

    spin_lock(&idr->lock);
    for (i = 0; i < idr->capacity; i++) {
        struct drm_idr_entry *e = &idr->table[i];

        if (IDR_SLOT_EMPTY(*e)) { continue; }
        ret = fn(e->id, e->ptr, data);
        if (ret != 0) { break; }
    }
    spin_unlock(&idr->lock);
    return ret;
}