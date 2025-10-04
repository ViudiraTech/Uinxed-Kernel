/*
 * page_walker.h
 * Page table walker implementation
 *
 * 2025/10/3 By W9pi3cZ1
 * Based on GPL-3.0 open source agreement
 * Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 */

#ifndef PAGE_WALKER_H
#define PAGE_WALKER_H

#include "hhdm.h"
#include "page.h"
#include "stdint.h"

typedef struct page_walk_state {
        page_directory_t *directory;
        uintptr_t         virtual_addr;
        uintptr_t         physical_addr;

        /* Precomputed indices for faster access */
        union {
                struct {
                        uint16_t l4_index;
                        uint16_t l3_index;
                        uint16_t l2_index;
                        uint16_t l1_index;
                };
                uint64_t indices;
        };

        uint16_t offset;

        /* Cached table pointers */
        page_table_t *l4_table;
        page_table_t *l3_table;
        page_table_t *l2_table;
        page_table_t *l1_table;

        /* Status flags */
        uint8_t is_valid : 1;
        uint8_t is_huge  : 1;
        uint8_t reserved : 6;
} page_walk_state_t;

/* Init page walk state */
void page_walk_init(page_walk_state_t *state, page_directory_t *directory, uintptr_t virtual_addr);

/* Execute page table walk */
uint8_t page_walk_execute(page_walk_state_t *state);

/* Check if a memory range is free using the current state */
uintptr_t walk_page_tables(page_directory_t *directory, uintptr_t virtual_addr);

/* Efficiently update state to next page */
void update_walk_state_for_next_page(page_walk_state_t *state, uintptr_t next_virtual);

/* Fast range free checker with large page */
size_t check_range_free_fast(page_directory_t *directory, uintptr_t start, size_t length);

/* Check range free with state */
size_t check_range_free_with_state(page_walk_state_t *state, uintptr_t start, size_t length);

/* Find a free virtual memory range of specified length */
uintptr_t walk_page_tables_find_free(page_directory_t *directory, uintptr_t start, size_t length);

/* Helper macros */
#define PAGE_WALK_INDEX(addr, shift) (((addr) >> (shift)) & 0x1FF)
#define IS_ALIGNED(addr, alignment)  (((addr) & ((alignment) - 1)) == 0)

#endif /* PAGE_WALKER_H */