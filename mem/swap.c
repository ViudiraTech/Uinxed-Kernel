/*
 *
 *      swap.c
 *      Linux-compatible swap area management and anonymous-page paging
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <mem/swap.h>

#ifdef SWAP_TEST_ONLY
#    include <string.h>
#    define PTE_PRESENT    (1ULL << 0)
#    define PTE_WRITEABLE  (1ULL << 1)
#    define PTE_USER       (1ULL << 2)
#    define PTE_COW        (1ULL << 9)
#    define PTE_SHARED     (1ULL << 10)
#    define PTE_NO_EXECUTE (1ULL << 63)
#else
#    include <chipset/common.h>
#    include <drivers/blockdev.h>
#    include <fs/vfs.h>
#    include <kernel/errno.h>
#    include <libs/std/stdbool.h>
#    include <libs/std/stdlib.h>
#    include <libs/std/string.h>
#    include <mem/frame.h>
#    include <mem/hhdm.h>
#    include <mem/heap.h>
#    include <mem/page.h>
#    include <proc/process.h>
#    include <sync/spin_lock.h>
#endif

#define SWAP_SIGNATURE_OFFSET (SWAP_PAGE_SIZE - 10)
#define SWAP_HEADER_VERSION   1024
#define SWAP_HEADER_LAST_PAGE 1028

static uint32_t swap_le32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

int swap_header_decode(const void *page, size_t bytes, uint64_t backing_pages, swap_header_info_t *info)
{
    const uint8_t *data = page;
    if (!data || !info || bytes < SWAP_PAGE_SIZE || backing_pages < 2) return -1;
    if (memcmp(data + SWAP_SIGNATURE_OFFSET, "SWAPSPACE2", 10)) return -1;

    uint32_t version   = swap_le32(data + SWAP_HEADER_VERSION);
    uint32_t last_page = swap_le32(data + SWAP_HEADER_LAST_PAGE);
    if (version != 1 || last_page < 1 || (uint64_t)last_page >= backing_pages) return -1;

    info->version = version;
    info->slots   = last_page;
    return 0;
}

uint64_t swap_entry_encode(uint32_t type, uint64_t offset, uint64_t pte_flags)
{
    uint64_t preserve = pte_flags & (PTE_WRITEABLE | PTE_USER | PTE_COW | PTE_SHARED | PTE_NO_EXECUTE);
    return PTE_SWAP | preserve | (((uint64_t)type & SWAP_TYPE_MASK) << SWAP_TYPE_SHIFT) |
           ((offset & SWAP_OFFSET_MASK) << SWAP_OFFSET_SHIFT);
}

int swap_entry_is_swap(uint64_t pte)
{
    return !(pte & PTE_PRESENT) && (pte & PTE_SWAP) && swap_entry_offset(pte) != 0;
}

uint32_t swap_entry_type(uint64_t pte)
{
    return (uint32_t)((pte >> SWAP_TYPE_SHIFT) & SWAP_TYPE_MASK);
}

uint64_t swap_entry_offset(uint64_t pte)
{
    return (pte >> SWAP_OFFSET_SHIFT) & SWAP_OFFSET_MASK;
}

uint64_t swap_entry_pte_flags(uint64_t pte)
{
    return pte & (PTE_WRITEABLE | PTE_USER | PTE_COW | PTE_SHARED | PTE_NO_EXECUTE);
}

static int swap_slot_valid(const swap_slot_map_t *map, uint64_t slot)
{
    return map && map->bitmap && map->refs && slot > 0 && slot <= map->slots;
}

static int swap_slot_used(const swap_slot_map_t *map, uint64_t slot)
{
    return (map->bitmap[slot / 64] >> (slot % 64)) & 1U;
}

static void swap_slot_set(swap_slot_map_t *map, uint64_t slot, int used)
{
    uint64_t mask = 1ULL << (slot % 64);
    if (used)
        map->bitmap[slot / 64] |= mask;
    else
        map->bitmap[slot / 64] &= ~mask;
}

int swap_slot_map_init(swap_slot_map_t *map, uint64_t *bitmap, uint32_t *refs, uint64_t slots)
{
    if (!map || !bitmap || !refs || slots < 1) return -1;
    map->bitmap       = bitmap;
    map->refs         = refs;
    map->slots        = slots;
    map->cluster_next = 1;
    memset(bitmap, 0, (size_t)((slots + 64) / 64) * sizeof(*bitmap));
    memset(refs, 0, (size_t)(slots + 1) * sizeof(*refs));
    return 0;
}

uint64_t swap_slot_alloc(swap_slot_map_t *map)
{
    if (!map || !map->slots) return 0;
    uint64_t start = map->cluster_next;
    if (start < 1 || start > map->slots) start = 1;

    for (uint64_t scanned = 0; scanned < map->slots; scanned++) {
        uint64_t slot = start + scanned;
        if (slot > map->slots) slot -= map->slots;
        if (!swap_slot_used(map, slot)) {
            swap_slot_set(map, slot, 1);
            map->refs[slot] = 1;
            map->cluster_next = slot == map->slots ? 1 : slot + 1;
            return slot;
        }
    }
    return 0;
}

int swap_slot_retain(swap_slot_map_t *map, uint64_t slot)
{
    if (!swap_slot_valid(map, slot) || !swap_slot_used(map, slot) || !map->refs[slot] || map->refs[slot] == UINT32_MAX) return -1;
    map->refs[slot]++;
    return 0;
}

int swap_slot_release(swap_slot_map_t *map, uint64_t slot)
{
    if (!swap_slot_valid(map, slot) || !swap_slot_used(map, slot) || !map->refs[slot]) return -1;
    if (!--map->refs[slot]) swap_slot_set(map, slot, 0);
    return 0;
}

uint32_t swap_slot_refs(const swap_slot_map_t *map, uint64_t slot)
{
    return swap_slot_valid(map, slot) ? map->refs[slot] : 0;
}

#ifndef SWAP_TEST_ONLY

typedef enum {
    SWAP_BACKEND_BLOCK,
    SWAP_BACKEND_FILE,
} swap_backend_t;

typedef struct swap_area {
    bool              active;
    bool              draining;
    uint8_t           type;
    int               priority;
    swap_backend_t    backend;
    blockdev_device_t device;
    vfs_node_t        file;
    swap_slot_map_t   slots;
    spinlock_t        lock;
    uint64_t          pages_in;
    uint64_t          pages_out;
    char              path[VFS_PATH_MAX];
} swap_area_t;

static swap_area_t swap_areas[SWAP_MAX_AREAS];
static spinlock_t  swap_lock;

static int swap_area_io(const swap_area_t *area, uint64_t slot, void *buffer, int write)
{
    if (!area || !area->active || !slot || slot > area->slots.slots) return -EINVAL;
    uint64_t offset = slot * SWAP_PAGE_SIZE;
    if (area->backend == SWAP_BACKEND_BLOCK) {
        return write ? blockdev_write_bytes(&area->device, offset, buffer, SWAP_PAGE_SIZE)
                     : blockdev_read_bytes(&area->device, offset, buffer, SWAP_PAGE_SIZE);
    }

    size_t actual = write ? callbackof(area->file, write)(area->file->handle, buffer, (size_t)offset, SWAP_PAGE_SIZE)
                          : callbackof(area->file, read)(area->file->handle, buffer, (size_t)offset, SWAP_PAGE_SIZE);
    return actual == SWAP_PAGE_SIZE ? EOK : -EIO;
}

static swap_area_t *swap_area_for_type(uint32_t type)
{
    return type < SWAP_MAX_AREAS && swap_areas[type].active ? &swap_areas[type] : NULL;
}

static int swap_area_retain_entry(uint64_t pte, int retain)
{
    if (!swap_entry_is_swap(pte)) return -EINVAL;
    swap_area_t *area = swap_area_for_type(swap_entry_type(pte));
    if (!area) return -EINVAL;
    spin_lock(&area->lock);
    int result = retain ? swap_slot_retain(&area->slots, swap_entry_offset(pte)) : swap_slot_release(&area->slots, swap_entry_offset(pte));
    spin_unlock(&area->lock);
    return result ? -EINVAL : EOK;
}

int swap_entry_retain_pte(uint64_t pte)
{
    return swap_area_retain_entry(pte, 1);
}

int swap_entry_release_pte(uint64_t pte)
{
    return swap_area_retain_entry(pte, 0);
}

void swap_init(void)
{
    memset(swap_areas, 0, sizeof(swap_areas));
    swap_lock.lock   = 0;
    swap_lock.rflags = 0;
}

static int swap_area_alloc(uint8_t *type, swap_area_t **area)
{
    spin_lock(&swap_lock);
    for (uint32_t i = 0; i < SWAP_MAX_AREAS; i++) {
        if (!swap_areas[i].active && !swap_areas[i].draining) {
            swap_areas[i].draining = true;
            *type = (uint8_t)i;
            *area = &swap_areas[i];
            spin_unlock(&swap_lock);
            return EOK;
        }
    }
    spin_unlock(&swap_lock);
    return -EPERM;
}

static int swap_area_setup_slots(swap_area_t *area, uint64_t pages, const uint8_t header[SWAP_PAGE_SIZE])
{
    swap_header_info_t info;
    if (swap_header_decode(header, SWAP_PAGE_SIZE, pages, &info)) return -EINVAL;
    size_t words = (size_t)((info.slots + 64) / 64);
    area->slots.bitmap = calloc(words, sizeof(uint64_t));
    area->slots.refs   = calloc((size_t)info.slots + 1, sizeof(uint32_t));
    if (!area->slots.bitmap || !area->slots.refs) {
        free(area->slots.bitmap);
        free(area->slots.refs);
        area->slots.bitmap = NULL;
        area->slots.refs   = NULL;
        return -ENOMEM;
    }
    if (swap_slot_map_init(&area->slots, area->slots.bitmap, area->slots.refs, info.slots)) return -ENOMEM;
    return EOK;
}

int swap_activate_path(const char *path, uint32_t flags)
{
    if (!path || !*path || (flags & ~(SWAP_FLAG_PREFER | SWAP_FLAG_PRIO_MASK | SWAP_FLAG_DISCARD))) return -EINVAL;

    spin_lock(&swap_lock);
    for (uint32_t i = 0; i < SWAP_MAX_AREAS; i++) {
        if (swap_areas[i].active && !strcmp(swap_areas[i].path, path)) {
            spin_unlock(&swap_lock);
            return -EBUSY;
        }
    }
    spin_unlock(&swap_lock);

    uint8_t      type;
    swap_area_t *area;
    int          result = swap_area_alloc(&type, &area);
    if (result) return result;

    memset(area, 0, sizeof(*area));
    area->type     = type;
    area->draining = true;
    area->priority = (flags & SWAP_FLAG_PREFER) ? (int)(flags & SWAP_FLAG_PRIO_MASK) : SWAP_PRIORITY_DEFAULT;
    area->lock.lock = 0;
    area->lock.rflags = 0;
    strncpy(area->path, path, sizeof(area->path) - 1);

    uint8_t header[SWAP_PAGE_SIZE];
    if (!strncmp(path, "/dev/", 5) && blockdev_open_name(path, &area->device) == EOK) {
        if (area->device.read_only || !area->device.sector_size ||
            area->device.sector_count > UINT64_MAX / area->device.sector_size) {
            memset(area, 0, sizeof(*area));
            return -EROFS;
        }
        uint64_t pages = area->device.sector_count * area->device.sector_size / SWAP_PAGE_SIZE;
        if (blockdev_read_bytes(&area->device, 0, header, sizeof(header)) != EOK || (result = swap_area_setup_slots(area, pages, header))) {
            blockdev_release(&area->device);
            memset(area, 0, sizeof(*area));
            return result ? result : -EIO;
        }
        area->backend = SWAP_BACKEND_BLOCK;
    } else {
        vfs_node_t file = vfs_open(path);
        if (!file) {
            memset(area, 0, sizeof(*area));
            return -ENOENT;
        }
        if ((file->type & ~file_delete) != file_none || file->size < 2 * SWAP_PAGE_SIZE || !file->handle ||
            callbackof(file, read) == NULL || callbackof(file, write) == NULL) {
            vfs_close(file);
            memset(area, 0, sizeof(*area));
            return -EINVAL;
        }
        result = vfs_writeback_range(file, 0, file->size - 1, 1);
        if (!result) result = vfs_invalidate_pages(file, 0, file->size - 1, 0);
        if (result) {
            vfs_close(file);
            memset(area, 0, sizeof(*area));
            return result;
        }
        size_t actual = callbackof(file, read)(file->handle, header, 0, sizeof(header));
        if (actual != sizeof(header) || (result = swap_area_setup_slots(area, file->size / SWAP_PAGE_SIZE, header))) {
            vfs_close(file);
            memset(area, 0, sizeof(*area));
            return result ? result : -EIO;
        }
        area->backend = SWAP_BACKEND_FILE;
        area->file    = file;
        file->flags |= VFS_NODE_SWAPFILE;
    }

    spin_lock(&swap_lock);
    area->draining = false;
    area->active = true;
    spin_unlock(&swap_lock);
    return EOK;
}

static page_table_entry_t *swap_pte_lookup(page_directory_t *directory, uintptr_t address)
{
    if (!directory || !directory->table || ((address >> 39) & 0x1ff) >= 256) return NULL;
    page_table_t *table = directory->table;
    uint64_t value = table->entries[(address >> 39) & 0x1ff].value;
    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    value = table->entries[(address >> 30) & 0x1ff].value;
    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    value = table->entries[(address >> 21) & 0x1ff].value;
    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    return &table->entries[(address >> 12) & 0x1ff];
}

int swap_fault(page_directory_t *directory, uintptr_t address)
{
    if (!directory) return -EINVAL;
    address = ALIGN_DOWN(address, SWAP_PAGE_SIZE);

    uint64_t entry;
    page_table_entry_t *pte;
    for (;;) {
        spin_lock(&directory->lock);
        pte = swap_pte_lookup(directory, address);
        entry = pte ? __atomic_load_n(&pte->value, __ATOMIC_ACQUIRE) : 0;
        if (!swap_entry_is_swap(entry)) {
            spin_unlock(&directory->lock);
            return -EINVAL;
        }
        if (!(entry & PTE_SWAP_BUSY)) break;
        spin_unlock(&directory->lock);
        __asm__ volatile("pause");
    }
    __atomic_store_n(&pte->value, entry | PTE_SWAP_BUSY, __ATOMIC_RELEASE);
    flush_tlb(address);
    spin_unlock(&directory->lock);

    swap_area_t *area = swap_area_for_type(swap_entry_type(entry));
    uint64_t frame = area ? alloc_frames(1) : 0;
    int result = (!frame || !area) ? -ENOMEM : swap_area_io(area, swap_entry_offset(entry), phys_to_virt(frame), 0);

    spin_lock(&directory->lock);
    pte = swap_pte_lookup(directory, address);
    if (!pte || __atomic_load_n(&pte->value, __ATOMIC_ACQUIRE) != (entry | PTE_SWAP_BUSY)) {
        spin_unlock(&directory->lock);
        if (frame) (void)frame_release_range(frame, 1);
        return -EAGAIN;
    }
    if (result == EOK) {
        uint64_t flags = swap_entry_pte_flags(entry) | PTE_PRESENT;
        __atomic_store_n(&pte->value, frame | flags, __ATOMIC_RELEASE);
        flush_tlb(address);
        (void)swap_entry_release_pte(entry);
        area->pages_in++;
    } else {
        __atomic_store_n(&pte->value, entry, __ATOMIC_RELEASE);
        flush_tlb(address);
        if (frame) (void)frame_release_range(frame, 1);
    }
    spin_unlock(&directory->lock);
    return result;
}

static int swap_out_page(page_directory_t *directory, uintptr_t address)
{
    swap_area_t *best = NULL;
    spin_lock(&swap_lock);
    for (uint32_t i = 0; i < SWAP_MAX_AREAS; i++) {
        swap_area_t *area = &swap_areas[i];
        if (!area->active || area->draining) continue;
        if (!best || area->priority > best->priority) best = area;
    }
    spin_unlock(&swap_lock);
    if (!best) return -ENOSPC;

    spin_lock(&best->lock);
    uint64_t slot = swap_slot_alloc(&best->slots);
    spin_unlock(&best->lock);
    if (!slot) return -ENOSPC;

    spin_lock(&directory->lock);
    page_table_entry_t *pte = swap_pte_lookup(directory, address);
    uint64_t value = pte ? __atomic_load_n(&pte->value, __ATOMIC_ACQUIRE) : 0;
    if (!pte || !(value & PTE_PRESENT) || !(value & PTE_USER) || (value & (PTE_SHARED | PTE_HUGE)) ||
        frame_refcount(value & PAGE_4K_MASK) != 1) {
        spin_unlock(&directory->lock);
        (void)swap_slot_release(&best->slots, slot);
        return -EAGAIN;
    }

    uint64_t entry = swap_entry_encode(best->type, slot, value & ~PAGE_4K_MASK) | PTE_SWAP_BUSY;
    __atomic_store_n(&pte->value, entry, __ATOMIC_RELEASE);
    flush_tlb(address);
    spin_unlock(&directory->lock);

    int result = swap_area_io(best, slot, phys_to_virt(value & PAGE_4K_MASK), 1);

    spin_lock(&directory->lock);
    pte = swap_pte_lookup(directory, address);
    if (pte && __atomic_load_n(&pte->value, __ATOMIC_ACQUIRE) == entry) {
        if (result == EOK) {
            __atomic_store_n(&pte->value, entry & ~PTE_SWAP_BUSY, __ATOMIC_RELEASE);
            flush_tlb(address);
            (void)frame_release_range(value & PAGE_4K_MASK, 1);
            best->pages_out++;
        } else {
            __atomic_store_n(&pte->value, value, __ATOMIC_RELEASE);
            flush_tlb(address);
            (void)swap_slot_release(&best->slots, slot);
        }
    } else if (result == EOK) {
        (void)swap_slot_release(&best->slots, slot);
        result = -EAGAIN;
    }
    spin_unlock(&directory->lock);
    return result;
}

int swap_reclaim(size_t target)
{
    size_t reclaimed = 0;
    size_t cursor = 0;
    process_t *proc;
    while (reclaimed < target && (proc = process_iterate_get(&cursor)) != NULL) {
        if (proc->user_page_dir) {
            spin_lock(&proc->mmap_lock);
            for (vm_area_t *vma = proc->mmap_list; vma && reclaimed < target; vma = vma->next) {
                if (vma->vm_file || (vma->flags & VM_SHARED)) continue;
                for (uintptr_t va = vma->start; va < vma->end && reclaimed < target; va += SWAP_PAGE_SIZE) {
                    if (swap_out_page(proc->user_page_dir, va) == EOK) reclaimed++;
                }
            }
            spin_unlock(&proc->mmap_lock);
        }
        process_put(proc);
    }
    return (int)reclaimed;
}

static int swapoff_area_in(page_directory_t *directory, page_table_t *table, int level, uintptr_t base, uint32_t type)
{
    uint64_t shift = level == 4 ? 39 : (level == 3 ? 30 : (level == 2 ? 21 : 12));
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t value = __atomic_load_n(&table->entries[i].value, __ATOMIC_ACQUIRE);
        uintptr_t address = base | ((uintptr_t)i << shift);
        if (level == 1) {
            if (swap_entry_is_swap(value) && swap_entry_type(value) == type) {
                int result = swap_fault(directory, address);
                if (result) return result;
            }
        } else if (value & PTE_PRESENT) {
            if (value & PTE_HUGE) continue;
            int result = swapoff_area_in(directory, phys_to_virt(value & PAGE_4K_MASK), level - 1, address, type);
            if (result) return result;
        }
    }
    return EOK;
}

int swap_deactivate_path(const char *path)
{
    if (!path) return -EINVAL;
    swap_area_t *area = NULL;
    spin_lock(&swap_lock);
    for (uint32_t i = 0; i < SWAP_MAX_AREAS; i++) {
        if (swap_areas[i].active && !strcmp(swap_areas[i].path, path)) {
            area = &swap_areas[i];
            area->draining = true;
            break;
        }
    }
    spin_unlock(&swap_lock);
    if (!area) return -EINVAL;

    size_t cursor = 0;
    process_t *proc;
    int result = EOK;
    while ((proc = process_iterate_get(&cursor)) != NULL) {
        if (proc->user_page_dir) result = swapoff_area_in(proc->user_page_dir, proc->user_page_dir->table, 4, 0, area->type);
        process_put(proc);
        if (result) break;
    }
    if (result) {
        area->draining = false;
        return result;
    }

    spin_lock(&area->lock);
    for (uint64_t slot = 1; slot <= area->slots.slots; slot++) {
        if (swap_slot_refs(&area->slots, slot)) {
            spin_unlock(&area->lock);
            area->draining = false;
            return -EBUSY;
        }
    }
    spin_unlock(&area->lock);
    if (area->backend == SWAP_BACKEND_FILE) {
        area->file->flags &= ~VFS_NODE_SWAPFILE;
        vfs_close(area->file);
    } else {
        blockdev_release(&area->device);
    }
    free(area->slots.bitmap);
    free(area->slots.refs);
    memset(area, 0, sizeof(*area));
    return EOK;
}

void swap_get_stats(swap_stats_t *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    spin_lock(&swap_lock);
    for (uint32_t i = 0; i < SWAP_MAX_AREAS; i++) {
        swap_area_t *area = &swap_areas[i];
        if (!area->active) continue;
        stats->areas++;
        stats->total_pages += area->slots.slots;
        stats->pages_in += area->pages_in;
        stats->pages_out += area->pages_out;
        for (uint64_t slot = 1; slot <= area->slots.slots; slot++) {
            if (swap_slot_refs(&area->slots, slot)) stats->used_pages++;
        }
    }
    spin_unlock(&swap_lock);
    stats->free_pages = stats->total_pages - stats->used_pages;
}

#endif /* !SWAP_TEST_ONLY */
