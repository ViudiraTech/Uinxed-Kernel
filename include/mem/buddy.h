/*
 *
 *      buddy.h
 *      Generic binary buddy allocator.
 *
 *      2026/8/1 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BUDDY_H_
#define INCLUDE_BUDDY_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define BUDDY_MAX_ORDER  30U
#define BUDDY_INDEX_NONE ((int32_t) - 1)

typedef enum {
    BUDDY_PAGE_RESERVED = 0,
    BUDDY_PAGE_FREE_HEAD,
    BUDDY_PAGE_ALLOC_HEAD,
    BUDDY_PAGE_ALLOC_TAIL,
} buddy_page_state_t;

/* Kept at 16 bytes so physical-page metadata remains bounded. */
typedef struct {
        int32_t  next;
        int32_t  prev;
        uint32_t tag; // Caller-owned while the page is allocated/reserved.
        uint8_t  order;
        uint8_t  state;
        uint16_t reserved;
} buddy_page_t;

typedef struct {
        buddy_page_t *pages;
        size_t        page_count;
        size_t        free_pages;
        uint8_t       max_order;
        int32_t       free_head[BUDDY_MAX_ORDER + 1];
        size_t        free_count[BUDDY_MAX_ORDER + 1];
} buddy_allocator_t;

/* Initialise an empty allocator; ranges remain reserved until added. */
int buddy_init(buddy_allocator_t *allocator, buddy_page_t *metadata, size_t page_count, unsigned max_order);

/* Add a reserved range to the allocator, maximally coalescing it. */
int buddy_add_range(buddy_allocator_t *allocator, size_t start, size_t count);

/* Allocate/free an aligned 2^order-unit block. */
size_t buddy_alloc(buddy_allocator_t *allocator, unsigned order);
int    buddy_free(buddy_allocator_t *allocator, size_t index, unsigned order);

/*
 * Convert an allocated power-of-two block into individually-owned units and
 * return its unused suffix.  This supports legacy exact-length frame APIs
 * without leaking the rounded portion of an allocation.
 */
int buddy_trim_allocation(buddy_allocator_t *allocator, size_t index, unsigned order, size_t keep_units);

/* Smallest order able to hold count units, or BUDDY_MAX_ORDER + 1. */
unsigned buddy_order_for_units(size_t count);

/* Expensive structural validation intended for boot checks and tests. */
int buddy_validate(const buddy_allocator_t *allocator);

#endif // INCLUDE_BUDDY_H_
