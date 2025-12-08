/*
 *
 *      page_walker.h
 *      Page table walker implementation header file
 *
 *      2025/10/3 By W9pi3cZ1
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef PAGE_WALKER_H_
#define PAGE_WALKER_H_

#include <hhdm.h>
#include <page.h>
#include <stdint.h>

#define PAGE_WALK_INDEX(addr, shift) (((addr) >> (shift)) & 0x1FF)
#define IS_ALIGNED(addr, alignment)  (((addr) & ((alignment) - 1)) == 0)

/* State of page walking */
typedef struct {
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
        uint8_t is_valid  : 1;
        uint8_t is_huge   : 1;
        uint8_t page_size : 2; // 0=4K, 1=2M, 2=1G
        uint8_t reserved  : 4;
} page_walk_state_t;

/* Get page size from walk state */
static inline size_t get_page_size_from_state(const page_walk_state_t *state)
{
    if (!state->is_valid) return PAGE_4K_SIZE;

    switch (state->page_size) {
        case 2 :
            return PAGE_1G_SIZE; // 1GB page
        case 1 :
            return PAGE_2M_SIZE; // 2MB page
        default :
            return PAGE_4K_SIZE; // 4KB page
    }
}

/* Check if address is aligned to specific page size */
static inline uint8_t is_page_aligned(uintptr_t addr, size_t page_size)
{
    return (addr & (page_size - 1)) == 0;
}

/* Align address down to specific page size */
static inline uintptr_t align_down_to_page(uintptr_t addr, size_t page_size)
{
    return addr & ~(page_size - 1);
}

/* Align address up to specific page size */
static inline uintptr_t align_up_to_page(uintptr_t addr, size_t page_size)
{
    return (addr + page_size - 1) & ~(page_size - 1);
}

/* Get next aligned address for specific page size */
static inline uintptr_t get_next_aligned_addr(uintptr_t addr, size_t page_size)
{
    return align_up_to_page(addr, page_size);
}

/* Init page_walk_state */
void page_walk_init(page_walk_state_t *state, page_directory_t *directory, uintptr_t virtual_addr);

/* Execute page_walk_state */
uint8_t page_walk_execute(page_walk_state_t *state);

/* Simple page table walk interface */
uintptr_t walk_page_tables(page_directory_t *directory, uintptr_t virtual_addr);

/* Efficiently update state to next page */
void update_walk_state_for_next_page(page_walk_state_t *state, uintptr_t next_virtual);

/* Check range free with state - supports multiple page sizes */
size_t check_range_free_with_state(page_walk_state_t *state, uintptr_t start, size_t length, size_t desired_size);

/* Find a free virtual memory range of specified length with preferred page size */
uintptr_t walk_page_tables_find_free(page_directory_t *directory, uintptr_t start, size_t length, size_t preferred_size);

#endif // PAGE_WALKER_H_
