/*
 *
 *      swap.h
 *      Anonymous-memory swap subsystem
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SWAP_H_
#define INCLUDE_SWAP_H_

#ifdef SWAP_TEST_ONLY
#    include <stddef.h>
#    include <stdint.h>
#else
#    include <libs/std/stddef.h>
#    include <libs/std/stdint.h>
#endif

#define SWAP_PAGE_SIZE        4096ULL
#define SWAP_MAX_AREAS        32
#define SWAP_PRIORITY_DEFAULT (-2)

/* Linux swapon(2) flags. */
#define SWAP_FLAG_PREFER    0x8000U
#define SWAP_FLAG_PRIO_MASK 0x7fffU
#define SWAP_FLAG_DISCARD   0x10000U

/* Non-present x86-64 PTE encoding for a swap entry. */
#define PTE_SWAP          (1ULL << 11)
#define PTE_SWAP_BUSY     (1ULL << 52)
#define SWAP_TYPE_SHIFT   12
#define SWAP_TYPE_BITS    5
#define SWAP_OFFSET_SHIFT 17
#define SWAP_OFFSET_BITS  35
#define SWAP_TYPE_MASK    ((1ULL << SWAP_TYPE_BITS) - 1)
#define SWAP_OFFSET_MASK  ((1ULL << SWAP_OFFSET_BITS) - 1)

typedef struct swap_header_info {
        uint32_t version;
        uint64_t slots;
} swap_header_info_t;

typedef struct swap_slot_map {
        uint64_t *bitmap;
        uint32_t *refs;
        uint64_t  slots;
        uint64_t  cluster_next;
} swap_slot_map_t;

typedef struct swap_stats {
        uint64_t total_pages;
        uint64_t free_pages;
        uint64_t used_pages;
        uint64_t pages_in;
        uint64_t pages_out;
        uint64_t faults;
        uint32_t areas;
} swap_stats_t;

/* Swap-file header and swap-entry PTE encoding. */
int      swap_header_decode(const void *page, size_t bytes, uint64_t backing_pages, swap_header_info_t *info);
uint64_t swap_entry_encode(uint32_t type, uint64_t offset, uint64_t pte_flags);
int      swap_entry_is_swap(uint64_t pte);
uint32_t swap_entry_type(uint64_t pte);
uint64_t swap_entry_offset(uint64_t pte);
uint64_t swap_entry_pte_flags(uint64_t pte);

/* Slot bitmap management with cluster hinting. */
int      swap_slot_map_init(swap_slot_map_t *map, uint64_t *bitmap, uint32_t *refs, uint64_t slots);
uint64_t swap_slot_alloc(swap_slot_map_t *map);
int      swap_slot_retain(swap_slot_map_t *map, uint64_t slot);
int      swap_slot_release(swap_slot_map_t *map, uint64_t slot);
uint32_t swap_slot_refs(const swap_slot_map_t *map, uint64_t slot);

#ifndef SWAP_TEST_ONLY
#    include <mem/page.h>

/* Lifecycle, swap files, reclaim, and fault handling. */
void swap_init(void);
int  swap_activate_path(const char *path, uint32_t flags);
int  swap_deactivate_path(const char *path);
int  swap_reclaim(size_t target);
int  swap_fault(page_directory_t *directory, uintptr_t address);
int  swap_entry_retain_pte(uint64_t pte);
int  swap_entry_release_pte(uint64_t pte);
void swap_get_stats(swap_stats_t *stats);
int  swap_format_proc_swaps(char *buf, size_t cap);
#endif

#endif // INCLUDE_SWAP_H_
