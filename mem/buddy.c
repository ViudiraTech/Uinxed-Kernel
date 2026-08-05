/*
 *
 *      buddy.c
 *      Generic binary buddy allocator.
 *
 *      2026/8/1 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <mem/buddy.h>

static size_t order_units(unsigned order)
{
    return (size_t)1 << order;
}

/* Smallest order able to hold count units. */
unsigned buddy_order_for_units(size_t count)
{
    if (!count) return 0;

    unsigned order = 0;
    size_t   units = 1;
    while (units < count && order < BUDDY_MAX_ORDER) {
        units <<= 1;
        order++;
    }
    if (units < count) return BUDDY_MAX_ORDER + 1;
    return order;
}

/* Reset the allocator over the given page metadata. */
int buddy_init(buddy_allocator_t *allocator, buddy_page_t *metadata, size_t page_count, unsigned max_order)
{
    if (!allocator || !metadata || !page_count || page_count > 0x7fffffffU || max_order > BUDDY_MAX_ORDER) return -1;

    allocator->pages      = metadata;
    allocator->page_count = page_count;
    allocator->free_pages = 0;
    allocator->max_order  = (uint8_t)max_order;

    for (unsigned order = 0; order <= BUDDY_MAX_ORDER; order++) {
        allocator->free_head[order]  = BUDDY_INDEX_NONE;
        allocator->free_count[order] = 0;
    }
    for (size_t i = 0; i < page_count; i++) {
        metadata[i].next     = BUDDY_INDEX_NONE;
        metadata[i].prev     = BUDDY_INDEX_NONE;
        metadata[i].tag      = 0;
        metadata[i].order    = 0;
        metadata[i].state    = BUDDY_PAGE_RESERVED;
        metadata[i].reserved = 0;
    }
    return 0;
}

static void list_add(buddy_allocator_t *allocator, size_t index, unsigned order)
{
    buddy_page_t *page = &allocator->pages[index];
    int32_t       head = allocator->free_head[order];

    page->prev  = BUDDY_INDEX_NONE;
    page->next  = head;
    page->order = (uint8_t)order;
    page->state = BUDDY_PAGE_FREE_HEAD;
    if (head != BUDDY_INDEX_NONE) allocator->pages[(size_t)head].prev = (int32_t)index;
    allocator->free_head[order] = (int32_t)index;
    allocator->free_count[order]++;
    allocator->free_pages += order_units(order);
}

static void list_remove(buddy_allocator_t *allocator, size_t index, unsigned order)
{
    buddy_page_t *page = &allocator->pages[index];

    if (page->prev == BUDDY_INDEX_NONE)
        allocator->free_head[order] = page->next;
    else
        allocator->pages[(size_t)page->prev].next = page->next;
    if (page->next != BUDDY_INDEX_NONE) allocator->pages[(size_t)page->next].prev = page->prev;

    page->next  = BUDDY_INDEX_NONE;
    page->prev  = BUDDY_INDEX_NONE;
    page->state = BUDDY_PAGE_RESERVED;
    allocator->free_count[order]--;
    allocator->free_pages -= order_units(order);
}

/* Insert a block into its free list, merging buddies up while possible. */
static void add_block(buddy_allocator_t *allocator, size_t index, unsigned order)
{
    while (order < allocator->max_order) {
        size_t buddy = index ^ order_units(order);
        if (buddy >= allocator->page_count) break;

        buddy_page_t *buddy_page = &allocator->pages[buddy];
        if (buddy_page->state != BUDDY_PAGE_FREE_HEAD || buddy_page->order != order) break;

        list_remove(allocator, buddy, order);
        if (buddy < index) index = buddy;
        order++;
    }
    list_add(allocator, index, order);
}

/* Add a page range to the allocator, split into aligned blocks. */
int buddy_add_range(buddy_allocator_t *allocator, size_t start, size_t count)
{
    if (!allocator || !count || start >= allocator->page_count || count > allocator->page_count - start) return -1;

    for (size_t i = start; i < start + count; i++) {
        if (allocator->pages[i].state != BUDDY_PAGE_RESERVED) return -1;
    }

    while (count) {
        unsigned order = buddy_order_for_units(count);
        if (order > allocator->max_order) order = allocator->max_order;
        if (order_units(order) > count) order--;
        while (order && (start & (order_units(order) - 1))) order--;

        add_block(allocator, start, order);
        size_t units = order_units(order);
        start += units;
        count -= units;
    }
    return 0;
}

/* Allocate a block of the given order, splitting a larger one if needed. */
size_t buddy_alloc(buddy_allocator_t *allocator, unsigned order)
{
    if (!allocator || order > allocator->max_order) return SIZE_MAX;

    unsigned found = order;
    while (found <= allocator->max_order && allocator->free_head[found] == BUDDY_INDEX_NONE) found++;
    if (found > allocator->max_order) return SIZE_MAX;

    size_t index = (size_t)allocator->free_head[found];
    list_remove(allocator, index, found);

    while (found > order) {
        found--;
        list_add(allocator, index + order_units(found), found);
    }

    size_t units                  = order_units(order);
    allocator->pages[index].state = BUDDY_PAGE_ALLOC_HEAD;
    allocator->pages[index].order = (uint8_t)order;
    for (size_t i = 1; i < units; i++) {
        allocator->pages[index + i].state = BUDDY_PAGE_ALLOC_TAIL;
        allocator->pages[index + i].order = 0;
    }
    return index;
}

/* Return a block of the given order to the allocator. */
int buddy_free(buddy_allocator_t *allocator, size_t index, unsigned order)
{
    if (!allocator || order > allocator->max_order || index >= allocator->page_count) return -1;
    size_t units = order_units(order);
    if ((index & (units - 1)) || units > allocator->page_count - index) return -1;
    if (allocator->pages[index].state != BUDDY_PAGE_ALLOC_HEAD || allocator->pages[index].order != order) return -1;
    for (size_t i = 1; i < units; i++) {
        if (allocator->pages[index + i].state != BUDDY_PAGE_ALLOC_TAIL) return -1;
    }

    for (size_t i = 0; i < units; i++) {
        allocator->pages[index + i].state = BUDDY_PAGE_RESERVED;
        allocator->pages[index + i].order = 0;
        allocator->pages[index + i].tag   = 0;
    }
    add_block(allocator, index, order);
    return 0;
}

/* Shrink a block down to keep_units pages, freeing the tail. */
int buddy_trim_allocation(buddy_allocator_t *allocator, size_t index, unsigned order, size_t keep_units)
{
    if (!allocator || order > allocator->max_order || index >= allocator->page_count) return -1;
    size_t units = order_units(order);
    if (!keep_units || keep_units > units || units > allocator->page_count - index) return -1;
    if (allocator->pages[index].state != BUDDY_PAGE_ALLOC_HEAD || allocator->pages[index].order != order) return -1;
    for (size_t i = 1; i < units; i++) {
        if (allocator->pages[index + i].state != BUDDY_PAGE_ALLOC_TAIL) return -1;
    }

    for (size_t i = 0; i < units; i++) {
        allocator->pages[index + i].state = BUDDY_PAGE_RESERVED;
        allocator->pages[index + i].order = 0;
        allocator->pages[index + i].tag   = 0;
    }
    for (size_t i = 0; i < keep_units; i++) {
        allocator->pages[index + i].state = BUDDY_PAGE_ALLOC_HEAD;
        allocator->pages[index + i].order = 0;
    }
    if (keep_units < units) return buddy_add_range(allocator, index + keep_units, units - keep_units);
    return 0;
}

/* Check whether index sits on the free list of the given order. */
static int node_in_list(const buddy_allocator_t *allocator, size_t wanted, unsigned order)
{
    int32_t node  = allocator->free_head[order];
    size_t  guard = 0;
    while (node != BUDDY_INDEX_NONE && guard++ <= allocator->page_count) {
        if ((size_t)node == wanted) return 1;
        if (node < 0 || (size_t)node >= allocator->page_count) return 0;
        node = allocator->pages[(size_t)node].next;
    }
    return 0;
}

/* Verify free lists and buddy coalescing invariants. */
int buddy_validate(const buddy_allocator_t *allocator)
{
    if (!allocator || !allocator->pages || !allocator->page_count || allocator->max_order > BUDDY_MAX_ORDER) return -1;

    size_t total_free  = 0;
    size_t total_heads = 0;
    for (unsigned order = 0; order <= allocator->max_order; order++) {
        int32_t node     = allocator->free_head[order];
        int32_t previous = BUDDY_INDEX_NONE;
        size_t  count    = 0;
        while (node != BUDDY_INDEX_NONE) {
            if (node < 0 || (size_t)node >= allocator->page_count || count++ > allocator->page_count) return -1;
            size_t              index = (size_t)node;
            size_t              units = order_units(order);
            const buddy_page_t *page  = &allocator->pages[index];
            if (page->state != BUDDY_PAGE_FREE_HEAD || page->order != order || page->prev != previous) return -1;
            if ((index & (units - 1)) || units > allocator->page_count - index) return -1;
            for (size_t i = 1; i < units; i++) {
                if (allocator->pages[index + i].state == BUDDY_PAGE_FREE_HEAD) return -1;
            }
            if (order < allocator->max_order) {
                size_t buddy = index ^ units;
                if (buddy < allocator->page_count && allocator->pages[buddy].state == BUDDY_PAGE_FREE_HEAD
                    && allocator->pages[buddy].order == order)
                    return -1; /* Coalescing invariant violated. */
            }
            previous = node;
            node     = page->next;
        }
        if (count != allocator->free_count[order]) return -1;
        total_heads += count;
        total_free += count * order_units(order);
    }
    if (total_free != allocator->free_pages) return -1;

    size_t observed_heads = 0;
    for (size_t i = 0; i < allocator->page_count; i++) {
        if (allocator->pages[i].state == BUDDY_PAGE_FREE_HEAD) {
            unsigned order = allocator->pages[i].order;
            if (order > allocator->max_order || !node_in_list(allocator, i, order)) return -1;
            observed_heads++;
        }
    }
    return observed_heads == total_heads ? 0 : -1;
}
