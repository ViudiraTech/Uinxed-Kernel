/*
 * page_walker.c
 * Page table walker implementation
 *
 * 2025/10/3 By W9pi3cZ1  
 * Based on GPL-3.0 open source agreement
 * Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 */

#include "page_walker.h"
#include "hhdm.h"
#include "page.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"

/* Constants for faster masking */
static const uint64_t HUGE_PAGE_1G_MASK = ~((1ULL << 30) - 1);
static const uint64_t HUGE_PAGE_2M_MASK = ~((1ULL << 21) - 1);

/* Init page_walk_state */
void page_walk_init(page_walk_state_t *state, page_directory_t *directory, uintptr_t virtual_addr)
{
    if (!state || !directory) { return; }

    state->directory    = directory;
    state->virtual_addr = virtual_addr;

    /* Precompute all indices for fast access */
    state->l4_index = PAGE_WALK_INDEX(virtual_addr, 39);
    state->l3_index = PAGE_WALK_INDEX(virtual_addr, 30);
    state->l2_index = PAGE_WALK_INDEX(virtual_addr, 21);
    state->l1_index = PAGE_WALK_INDEX(virtual_addr, 12);
    state->offset   = virtual_addr & 0xFFF;

    /* Initialize table pointers */
    state->l4_table = directory->table;
    state->l3_table = NULL;
    state->l2_table = NULL;
    state->l1_table = NULL;

    /* Reset status */
    state->is_valid      = 0;
    state->is_huge       = 0;
    state->physical_addr = 0;
}

/* Fast page table lookup helper */
static inline uint8_t page_table_lookup(page_table_t *table, uint16_t index, page_table_t **next_table, uint64_t *entry_value)
{
    if (next_table && *next_table && entry_value) {
        *entry_value = (uintptr_t)virt_to_phys((uintptr_t)*next_table);
        return 1;
    }

    if (!table || index >= 512) { return 0; }

    uint64_t entry = table->entries[index].value;
    if (!(entry & PTE_PRESENT)) { return 0; }

    if (entry_value) { *entry_value = entry; }

    if (next_table && !is_huge_page(&table->entries[index])) { *next_table = (page_table_t *)phys_to_virt(entry & PAGE_FLAGS_MASK); }

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
        state->physical_addr = (l3_entry & HUGE_PAGE_1G_MASK) | (state->virtual_addr & ((1ULL << 30) - 1));
        state->is_valid      = 1;
        state->is_huge       = 1;
        return 1;
    }

    /* L2 lookup - check for 2MB huge page */
    uint64_t l2_entry;
    if (!page_table_lookup(state->l2_table, state->l2_index, &state->l1_table, &l2_entry)) {
        state->is_valid = 0;
        return 0;
    }

    if (is_huge_page(&state->l2_table->entries[state->l2_index])) {
        state->physical_addr = (l2_entry & HUGE_PAGE_2M_MASK) | (state->virtual_addr & ((1ULL << 21) - 1));
        state->is_valid      = 1;
        state->is_huge       = 1;
        return 1;
    }

    /* L1 lookup - regular 4KB page */
    uint64_t l1_entry;
    if (!page_table_lookup(state->l1_table, state->l1_index, NULL, &l1_entry)) {
        state->is_valid = 0;
        return 0;
    }

    state->physical_addr = (l1_entry & PAGE_FLAGS_MASK) | state->offset;
    state->is_valid      = 1;
    state->is_huge       = 0;
    return 1;
}

/* Simple page table walk interface */
uintptr_t walk_page_tables(page_directory_t *directory, uintptr_t virtual_addr)
{
    if (!directory || !virtual_addr) { return 0; }

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
    state->offset   = next_virtual & 0xFFF;

    const uintptr_t difference = old_virtual ^ next_virtual;

    /* Only update higher levels when crossing boundaries */
    if (difference >> 21) { /* L2 boundary */
        state->l2_index = PAGE_WALK_INDEX(next_virtual, 21);
        state->l2_table = NULL;

        if (difference >> 30) { /* L3 boundary */
            state->l3_index = PAGE_WALK_INDEX(next_virtual, 30);
            state->l3_table = NULL;

            if (difference >> 39) { /* L4 boundary */
                state->l4_index = PAGE_WALK_INDEX(next_virtual, 39);
                /* l4_table remains the same (directory doesn't change) */
            }
        }
    }
}

/* Check range free with state */
size_t check_range_free_with_state(page_walk_state_t *state, uintptr_t start, size_t length) // NOLINT
{
    if (!state || length == 0) { return 0; }

    size_t    free_bytes = 0;
    uintptr_t current    = ALIGN_UP(start, PAGE_SIZE);

    /* Reinitialize state for start address */
    page_walk_init(state, state->directory, current);

    while (free_bytes < length) {
        if (page_walk_execute(state)) {
            break; // Page is mapped
        }

        free_bytes += PAGE_SIZE;
        current += PAGE_SIZE;

        /* Efficiently update state for next page */
        update_walk_state_for_next_page(state, current);
    }

    return free_bytes;
}

/* Fast range free checker with large page */
size_t check_range_free_fast(page_directory_t *directory, uintptr_t start, size_t length) // NOLINT
{
    if (!directory || length == 0) return 0;

    page_walk_state_t state;
    uintptr_t         current = ALIGN_UP(start, 1 << 21); /* 2MB alignment */
    size_t            checked = 0;

    page_walk_init(&state, directory, current);
    while (checked < length) {
        update_walk_state_for_next_page(&state, current);
        if (page_walk_execute(&state)) {
            break; // Page is mapped
        }
        checked += (1 << 21); /* Check 2MB at a time */
        current += (1 << 21);
    }

    return checked > length ? length : checked;
}

/* Find a free virtual memory range of specified length */
uintptr_t walk_page_tables_find_free(page_directory_t *directory, uintptr_t start, size_t length) // NOLINT
{
    if (!directory || length == 0) { return 0; }

    uintptr_t         candidate = ALIGN_UP(start, PAGE_SIZE);
    page_walk_state_t state;

    /* Align length to page boundary */
    size_t aligned_length = ALIGN_UP(length, PAGE_SIZE);

    while (1) {
        /* Initialize walk state */
        page_walk_init(&state, directory, candidate);

        /* Check candidate region */
        size_t free_length = check_range_free_with_state(&state, candidate, aligned_length);

        if (free_length >= aligned_length) { return candidate; }

        /* Jump to next candidate (aligned to next potential boundary) */
        candidate += ALIGN_UP(free_length + PAGE_SIZE, PAGE_SIZE);

        /* Prevent infinite loop with sanity check */
        if (candidate < start) { /* Overflow detection */
            return 0;
        }
    }
}