/*
 *
 *      page_walker.c
 *      Page table walker implementation
 *
 *      2025/10/3 By W9pi3cZ1
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <hhdm.h>
#include <page.h>
#include <page_walker.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Init page_walk_state */
void page_walk_init(page_walk_state_t *state, page_directory_t *directory, uintptr_t virtual_addr)
{
    if (!state || !directory) return;

    state->directory    = directory;
    state->virtual_addr = virtual_addr;

    /* Precompute all indices for fast access */
    state->l4_index = PAGE_WALK_INDEX(virtual_addr, 39);
    state->l3_index = PAGE_WALK_INDEX(virtual_addr, 30);
    state->l2_index = PAGE_WALK_INDEX(virtual_addr, 21);
    state->l1_index = PAGE_WALK_INDEX(virtual_addr, 12);
    state->offset   = virtual_addr & 0xfff;

    /* Initialize table pointers */
    state->l4_table = directory->table;
    state->l3_table = 0;
    state->l2_table = 0;
    state->l1_table = 0;

    /* Reset status */
    state->is_valid      = 0;
    state->is_huge       = 0;
    state->page_size     = 0;
    state->physical_addr = 0;
}

/* Fast page table lookup helper */
static inline uint8_t page_table_lookup(page_table_t *table, uint16_t index, page_table_t **next_table, uint64_t *entry_value)
{
    if (!table || index >= 512) return 0;
    uint64_t entry = table->entries[index].value;

    if (!(entry & PTE_PRESENT)) return 0;
    if (entry_value) *entry_value = entry;
    if (next_table && !is_huge_page(&table->entries[index])) *next_table = (page_table_t *)phys_to_virt(entry & PAGE_4K_MASK);
    return 1;
}

/* Execute page_walk_state */
uint8_t page_walk_execute(page_walk_state_t *state)
{
    if (!state || !state->directory || !state->l4_table) {
        if (state) state->is_valid = 0;
        return 0;
    }

    /* L4 lookup */
    uint64_t l4_entry;
    if (!page_table_lookup(state->l4_table, state->l4_index, &state->l3_table, &l4_entry)) {
        state->is_valid = 0;
        return 0;
    }

    /* L3 lookup - check for 1GB huge page */
    uint64_t l3_entry;
    if (!page_table_lookup(state->l3_table, state->l3_index, &state->l2_table, &l3_entry)) {
        state->is_valid = 0;
        return 0;
    }
    if (is_huge_page(&state->l3_table->entries[state->l3_index])) {
        state->physical_addr = (l3_entry & PAGE_1G_MASK) | (state->virtual_addr & (PAGE_1G_SIZE - 1));
        state->is_valid      = 1;
        state->is_huge       = 1;
        state->page_size     = 2; // 1GB page
        return 1;
    }

    /* L2 lookup - check for 2MB huge page */
    uint64_t l2_entry;
    if (!page_table_lookup(state->l2_table, state->l2_index, &state->l1_table, &l2_entry)) {
        state->is_valid = 0;
        return 0;
    }
    if (is_huge_page(&state->l2_table->entries[state->l2_index])) {
        state->physical_addr = (l2_entry & PAGE_2M_MASK) | (state->virtual_addr & (PAGE_2M_SIZE - 1));
        state->is_valid      = 1;
        state->is_huge       = 1;
        state->page_size     = 1; // 2MB page
        return 1;
    }

    /* L1 lookup - regular 4KB page */
    uint64_t l1_entry;
    if (!page_table_lookup(state->l1_table, state->l1_index, 0, &l1_entry)) {
        state->is_valid = 0;
        return 0;
    }

    state->physical_addr = (l1_entry & PAGE_4K_MASK) | state->offset;
    state->is_valid      = 1;
    state->is_huge       = 0;
    state->page_size     = 0; // 4KB page
    return 1;
}

/* Simple page table walk interface */
uintptr_t walk_page_tables(page_directory_t *directory, uintptr_t virtual_addr)
{
    if (!directory || !virtual_addr) return 0;

    page_walk_state_t state;
    page_walk_init(&state, directory, virtual_addr);

    return page_walk_execute(&state) ? state.physical_addr : 0;
}

/* Efficiently update state to next page */
void update_walk_state_for_next_page(page_walk_state_t *state, uintptr_t next_virtual)
{
    if (!state) return;

    uintptr_t old_virtual = state->virtual_addr;
    state->virtual_addr   = next_virtual;

    /* Update L1 index (most frequent change) */
    state->l1_index = PAGE_WALK_INDEX(next_virtual, 12);
    state->offset   = next_virtual & 0xfff;

    const uintptr_t difference = old_virtual ^ next_virtual;

    /* Only update higher levels when crossing boundaries */
    if (difference >> 21) { // L2 boundary
        state->l2_index = PAGE_WALK_INDEX(next_virtual, 21);
        state->l2_table = 0;
        if (difference >> 30) { // L3 boundary
            state->l3_index = PAGE_WALK_INDEX(next_virtual, 30);
            state->l3_table = 0;
            if (difference >> 39) { // L4 boundary
                state->l4_index = PAGE_WALK_INDEX(next_virtual, 39);
                /* l4_table remains the same (directory doesn't change) */
            }
        }
    }
}

/* Check range free with state - supports multiple page sizes */
size_t check_range_free_with_state(page_walk_state_t *state, uintptr_t start, size_t length, size_t desired_size)
{
    if (!state || !length) return 0;

    size_t    free_bytes = 0;
    uintptr_t current    = start;

    /* Initialize state for start address */
    page_walk_init(state, state->directory, current);

    while (free_bytes < length) {
        if (page_walk_execute(state)) {
            /* Page is mapped - get its size and skip accordingly */
            size_t page_size = get_page_size_from_state(state);
            current          = align_up_to_page(current + 1, page_size);
            free_bytes       = 0; // Reset free bytes count - not contiguous

            /* Reinitialize state for new address */
            page_walk_init(state, state->directory, current);
            continue;
        }

        /* Page is free - determine how much we can advance */
        size_t current_free = PAGE_4K_SIZE; // At least 4K is free

        /* Check if we can use larger pages */
        if (desired_size >= PAGE_2M_SIZE && state->is_huge && state->page_size == 1) {
            /* Check if we have enough space for 2MB page */
            if (length - free_bytes >= PAGE_2M_SIZE) current_free = PAGE_2M_SIZE;
        } else if (desired_size >= PAGE_1G_SIZE && state->is_huge && state->page_size == 2) {
            /* Check if we have enough space for 1GB page */
            if (length - free_bytes >= PAGE_1G_SIZE) current_free = PAGE_1G_SIZE;
        }
        free_bytes += current_free;
        current += current_free;

        /* Update state for next check */
        update_walk_state_for_next_page(state, current);
    }
    return free_bytes;
}

/* Find a free virtual memory range of specified length with preferred page size */
uintptr_t walk_page_tables_find_free(page_directory_t *directory, uintptr_t start, size_t length, size_t preferred_size)
{
    if (!directory || !length) return 0;

    /* Validate preferred page size */
    if (preferred_size != PAGE_4K_SIZE && preferred_size != PAGE_2M_SIZE && preferred_size != PAGE_1G_SIZE) { preferred_size = PAGE_4K_SIZE; }

    uintptr_t         candidate = align_up_to_page(start, preferred_size);
    page_walk_state_t state;

    /* Ensure length is aligned to preferred page size */
    size_t aligned_length = align_up_to_page(length, preferred_size);

    while (1) {
        /* Check if candidate is properly aligned */
        if (!is_page_aligned(candidate, preferred_size)) {
            candidate = align_up_to_page(candidate, preferred_size);
            continue;
        }

        /* Initialize walk state */
        page_walk_init(&state, directory, candidate);

        /* Check candidate region */
        size_t free_length = check_range_free_with_state(&state, candidate, aligned_length, preferred_size);
        if (free_length >= aligned_length) return candidate;

        /* Move to next potential candidate */
        if (free_length > 0) {
            candidate += free_length;
        } else {
            /* No free space at all, move to next aligned address */
            candidate += preferred_size;
        }

        /* Overflow detection */
        if (candidate < start) return 0;
    }
    return 0; // No suitable range found
}
