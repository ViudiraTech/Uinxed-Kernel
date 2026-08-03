/* Host red/green regression for user write-protection COW faults. */

#include <mem/frame.h>
#include <mem/page.h>
#include <proc/process.h>

#define TEST_POOL_SIZE (16 * 1024 * 1024)
#define TEST_FRAMES    (TEST_POOL_SIZE / PAGE_4K_SIZE)

static unsigned char pool[TEST_POOL_SIZE] __attribute__((aligned(PAGE_4K_SIZE)));
static uint32_t      refs[TEST_FRAMES];
static uint64_t      next_frame = PAGE_2M_SIZE;
static int           failures;

#define CHECK(condition)                                                                                                                \
    do {                                                                                                                                \
        if (!(condition)) failures++;                                                                                                   \
    } while (0)

void *memset(void *destination, int value, size_t length)
{
    unsigned char *out = destination;
    while (length--) *out++ = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t length)
{
    unsigned char       *out = destination;
    const unsigned char *in  = source;
    while (length--) *out++ = *in++;
    return destination;
}

void *phys_to_virt(uint64_t address)
{
    return pool + address;
}

void *virt_to_phys(uint64_t address)
{
    return (void *)(address - (uint64_t)pool);
}

void flush_tlb(uint64_t address)
{
    (void)address;
}

void flush_tlb_all(void)
{
}

void spin_lock(spinlock_t *lock)
{
    (void)lock;
}

void spin_unlock(spinlock_t *lock)
{
    (void)lock;
}

uint64_t alloc_frames(size_t count)
{
    if (!count || next_frame + count * PAGE_4K_SIZE > TEST_POOL_SIZE) return 0;
    uint64_t frame = next_frame;
    next_frame += count * PAGE_4K_SIZE;
    for (size_t i = 0; i < count; i++) refs[frame / PAGE_4K_SIZE + i] = 1;
    return frame;
}

uint64_t alloc_frames_2M(size_t count)
{
    next_frame = (next_frame + PAGE_2M_SIZE - 1) & ~(PAGE_2M_SIZE - 1);
    return alloc_frames(count * PAGE_2M_SIZE / PAGE_4K_SIZE);
}

uint64_t alloc_frames_1G(size_t count)
{
    (void)count;
    return 0;
}

int frame_retain_range(uint64_t address, size_t count)
{
    size_t first = address / PAGE_4K_SIZE;
    if (!address || first + count > TEST_FRAMES) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!refs[first + i]) return -1;
        refs[first + i]++;
    }
    return 0;
}

int frame_release_range(uint64_t address, size_t count)
{
    size_t first = address / PAGE_4K_SIZE;
    if (!address || first + count > TEST_FRAMES) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!refs[first + i]) return -1;
        refs[first + i]--;
    }
    return 0;
}

uint32_t frame_refcount(uint64_t address)
{
    return refs[address / PAGE_4K_SIZE];
}

int swap_entry_is_swap(uint64_t pte)
{
    (void)pte;
    return 0;
}

int swap_entry_retain_pte(uint64_t pte)
{
    (void)pte;
    return 0;
}

int swap_entry_release_pte(uint64_t pte)
{
    (void)pte;
    return 0;
}

void free_frame(uint64_t address)
{
    (void)frame_release_range(address, 1);
}

void free_frames(uint64_t address, size_t count)
{
    (void)frame_release_range(address, count);
}

void free_frames_2M(uint64_t address)
{
    (void)frame_release_range(address, PAGE_2M_SIZE / PAGE_4K_SIZE);
}

void free_frames_1G(uint64_t address)
{
    (void)address;
}

static page_directory_t directory_create(void)
{
    page_directory_t directory = {0};
    directory.table = phys_to_virt(alloc_frames(1));
    page_table_clear(directory.table);
    return directory;
}

static page_table_entry_t *leaf_for(page_directory_t *directory, uintptr_t address)
{
    page_table_t *table = directory->table;
    uint64_t      value = table->entries[(address >> 39) & 0x1ff].value;
    if (!(value & PTE_PRESENT)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    value = table->entries[(address >> 30) & 0x1ff].value;
    if (!(value & PTE_PRESENT)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    value = table->entries[(address >> 21) & 0x1ff].value;
    if (!(value & PTE_PRESENT)) return NULL;
    table = phys_to_virt(value & PAGE_4K_MASK);
    return &table->entries[(address >> 12) & 0x1ff];
}

int main(void)
{
    const uintptr_t address = 0x400000;
    page_directory_t parent = directory_create();
    page_directory_t child  = directory_create();
    uint64_t original = alloc_frames(1);
    memset(phys_to_virt(original), 0x5a, PAGE_4K_SIZE);
    page_map_to(&parent, address, original, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE);

    CHECK(page_clone_user_cow(&child, &parent) == 0);
    CHECK((leaf_for(&parent, address)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
    CHECK((leaf_for(&child, address)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
    CHECK(frame_refcount(original) == 2);

    vm_area_t child_vma = {.start = address, .end = address + PAGE_4K_SIZE, .flags = VM_READ | VM_WRITE};
    process_t child_process = {.user_page_dir = &child, .mmap_list = &child_vma};
    CHECK(page_resolve_cow_fault(&child_process, address) == 0);
    uint64_t copied = leaf_for(&child, address)->value & PAGE_4K_MASK;
    CHECK(copied != original);
    CHECK((leaf_for(&child, address)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_WRITEABLE);
    CHECK(((unsigned char *)phys_to_virt(copied))[0] == 0x5a);
    CHECK(frame_refcount(original) == 1);

    /* This models mprotect(RW -> R): stale COW must be cleared, and a
     * protection write fault must not regain write access. */
    page_map_to(&parent, address, original, PTE_PRESENT | PTE_USER | PTE_NO_EXECUTE);
    vm_area_t parent_vma = {.start = address, .end = address + PAGE_4K_SIZE, .flags = VM_READ};
    process_t parent_process = {.user_page_dir = &parent, .mmap_list = &parent_vma};
    CHECK((leaf_for(&parent, address)->value & (PTE_COW | PTE_WRITEABLE)) == 0);
    CHECK(page_resolve_cow_fault(&parent_process, address) != 0);
    CHECK((leaf_for(&parent, address)->value & (PTE_COW | PTE_WRITEABLE)) == 0);

    page_destroy_user_space(&child);
    page_destroy_user_space(&parent);
    CHECK(frame_refcount(copied) == 0);
    CHECK(frame_refcount(original) == 0);

    if (failures) return 1;
    return 0;
}
