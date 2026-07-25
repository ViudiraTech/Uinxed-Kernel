/*
 * Host regression for the real page-table COW implementation.
 *
 * The fake frame backend deliberately models ownership independently of
 * page.c so wrong retain/release sizes and rollback leaks remain observable.
 */

#include <mem/frame.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

#define TEST_POOL_SIZE   (32 * 1024 * 1024)
#define TEST_FRAME_COUNT (3 * PAGE_1G_SIZE / PAGE_4K_SIZE + 4096)
#define SPARSE_VIRT_BASE 0x100000000000ULL

static unsigned char test_pool[TEST_POOL_SIZE] __attribute__((aligned(PAGE_4K_SIZE)));
static uint32_t      test_refs[TEST_FRAME_COUNT];
static uint64_t      next_frame = PAGE_2M_SIZE;
static size_t        allocation_attempts;
static size_t        allocation_limit = SIZE_MAX;
static size_t        live_allocated_frames;
static int           sparse_1g_used;
static uint64_t      sparse_copy_source;
static uint64_t      sparse_copy_destination;
static size_t        sparse_copy_length;
static unsigned char sparse_old_marker;
static unsigned char sparse_new_marker;

static int test_failures;
static int test_failure_line;

#define CHECK(expr)                \
    do {                           \
        if (!(expr)) {                \
            test_failures++;          \
            test_failure_line = __LINE__; \
        }                             \
    } while (0)

static void bytes_set(void *dst, unsigned char value, size_t length)
{
    unsigned char *out = dst;
    while (length--) *out++ = value;
}

static int bytes_equal(const void *left, const void *right, size_t length)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    while (length--) {
        if (*a++ != *b++) return 0;
    }
    return 1;
}

void *memset(void *dst, int value, size_t length)
{
    bytes_set(dst, (unsigned char)value, length);
    return dst;
}

static int is_sparse_pointer(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return address >= SPARSE_VIRT_BASE &&
           address < SPARSE_VIRT_BASE + TEST_FRAME_COUNT * PAGE_4K_SIZE;
}

void *memcpy(void *dst, const void *src, size_t length)
{
    if (is_sparse_pointer(dst) || is_sparse_pointer(src)) {
        sparse_copy_source      = (uintptr_t)src - SPARSE_VIRT_BASE;
        sparse_copy_destination = (uintptr_t)dst - SPARSE_VIRT_BASE;
        sparse_copy_length      = length;
        sparse_new_marker       = sparse_old_marker;
        return dst;
    }

    unsigned char       *out = dst;
    const unsigned char *in  = src;
    while (length--) *out++ = *in++;
    return dst;
}

void *phys_to_virt(uint64_t address)
{
    if (address >= TEST_POOL_SIZE) return (void *)(SPARSE_VIRT_BASE + address);
    return test_pool + address;
}

void *virt_to_phys(uint64_t address)
{
    return (void *)(address - (uint64_t)test_pool);
}

void flush_tlb(uint64_t address)
{
    (void)address;
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
    allocation_attempts++;
    if (allocation_attempts > allocation_limit || !count || next_frame + count * PAGE_4K_SIZE > TEST_POOL_SIZE) return 0;

    uint64_t result = next_frame;
    next_frame += count * PAGE_4K_SIZE;
    for (size_t i = 0; i < count; i++) test_refs[result / PAGE_4K_SIZE + i] = 1;
    live_allocated_frames += count;
    return result;
}

uint64_t alloc_frames_2M(size_t count)
{
    next_frame = (next_frame + PAGE_2M_SIZE - 1) & ~(PAGE_2M_SIZE - 1);
    return alloc_frames(count * (PAGE_2M_SIZE / PAGE_4K_SIZE));
}

uint64_t alloc_frames_1G(size_t count)
{
    allocation_attempts++;
    if (allocation_attempts > allocation_limit || count != 1 || sparse_1g_used) return 0;

    uint64_t result = 2 * PAGE_1G_SIZE;
    size_t   first  = result / PAGE_4K_SIZE;
    for (size_t i = 0; i < PAGE_1G_SIZE / PAGE_4K_SIZE; i++) test_refs[first + i] = 1;
    live_allocated_frames += PAGE_1G_SIZE / PAGE_4K_SIZE;
    sparse_1g_used = 1;
    return result;
}

int frame_retain_range(uint64_t address, size_t count)
{
    size_t first = address / PAGE_4K_SIZE;
    if (!address || !count || first + count > TEST_FRAME_COUNT) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!test_refs[first + i]) return -1;
    }
    for (size_t i = 0; i < count; i++) test_refs[first + i]++;
    return 0;
}

int frame_release_range(uint64_t address, size_t count)
{
    size_t first = address / PAGE_4K_SIZE;
    if (!address || !count || first + count > TEST_FRAME_COUNT) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!test_refs[first + i]) return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (--test_refs[first + i] == 0) live_allocated_frames--;
    }
    return 0;
}

uint32_t frame_refcount(uint64_t address)
{
    size_t frame = address / PAGE_4K_SIZE;
    return frame < TEST_FRAME_COUNT ? test_refs[frame] : 0;
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
    (void)frame_release_range(address, PAGE_1G_SIZE / PAGE_4K_SIZE);
}

static page_directory_t make_directory(void)
{
    page_directory_t directory = {0};
    uint64_t         root      = alloc_frames(1);
    directory.table            = phys_to_virt(root);
    page_table_clear(directory.table);
    return directory;
}

static page_table_entry_t *leaf_for(page_directory_t *directory, uintptr_t address)
{
    page_table_t *table = directory->table;
    uint64_t      entry = table->entries[(address >> 39) & 0x1ff].value;
    if (!(entry & PTE_PRESENT)) return 0;
    table = phys_to_virt(entry & PAGE_4K_MASK);

    page_table_entry_t *leaf = &table->entries[(address >> 30) & 0x1ff];
    if (leaf->value & PTE_HUGE) return leaf;
    if (!(leaf->value & PTE_PRESENT)) return 0;
    table = phys_to_virt(leaf->value & PAGE_4K_MASK);

    leaf = &table->entries[(address >> 21) & 0x1ff];
    if (leaf->value & PTE_HUGE) return leaf;
    if (!(leaf->value & PTE_PRESENT)) return 0;
    table = phys_to_virt(leaf->value & PAGE_4K_MASK);
    return &table->entries[(address >> 12) & 0x1ff];
}

static void mark_owned_range(uint64_t address, size_t count)
{
    size_t first = address / PAGE_4K_SIZE;
    for (size_t i = 0; i < count; i++) {
        test_refs[first + i] = 1;
        live_allocated_frames++;
    }
}

static void reset_backend(void)
{
    bytes_set(test_pool, 0, sizeof(test_pool));
    bytes_set(test_refs, 0, sizeof(test_refs));
    next_frame            = PAGE_2M_SIZE;
    allocation_attempts   = 0;
    allocation_limit      = SIZE_MAX;
    live_allocated_frames = 0;
    sparse_1g_used        = 0;
    sparse_copy_source    = 0;
    sparse_copy_destination = 0;
    sparse_copy_length      = 0;
    sparse_old_marker       = 0x77;
    sparse_new_marker       = 0;
}

static void test_map_status_and_collision(void)
{
    reset_backend();
    page_directory_t directory = make_directory();
    uint64_t         first     = alloc_frames(1);
    uint64_t         second    = alloc_frames(1);

    CHECK(page_map_new_to(&directory, 0x400000, first, PTE_PRESENT | PTE_USER | PTE_WRITEABLE) == 0);
    CHECK(page_map_new_to(&directory, 0x400000, second, PTE_PRESENT | PTE_USER | PTE_WRITEABLE) != 0);
    CHECK((leaf_for(&directory, 0x400000)->value & PAGE_4K_MASK) == first);
    size_t baseline = live_allocated_frames;

    allocation_limit = allocation_attempts;
    CHECK(page_map_new_to(&directory, 0x8000000000ULL, second, PTE_PRESENT | PTE_USER) != 0);
    CHECK(directory.table->entries[1].value == 0);
    CHECK(live_allocated_frames == baseline);
    allocation_limit = SIZE_MAX;

    free_frame(second);
    page_destroy_user_space(&directory);
    CHECK(live_allocated_frames == 0);
}

static void test_clone_fault_and_lifetime(void)
{
    reset_backend();
    page_directory_t parent = make_directory();
    page_directory_t child  = make_directory();

    const uintptr_t private_va = 0x400000;
    const uintptr_t shared_va  = 0x800000;
    const uintptr_t readonly_va = 0xc00000;
    uint64_t        private_frame = alloc_frames(1);
    uint64_t        shared_frame  = alloc_frames(1);
    uint64_t        readonly_frame = alloc_frames(1);

    bytes_set(phys_to_virt(private_frame), 0x5a, PAGE_4K_SIZE);
    page_map_to(&parent, private_va, private_frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE);
    page_map_to(&parent, shared_va, shared_frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_SHARED);
    page_map_to(&parent, readonly_va, readonly_frame, PTE_PRESENT | PTE_USER | PTE_NO_EXECUTE);

    CHECK(page_clone_user_cow(&child, &parent) == 0);

    page_table_entry_t *parent_private = leaf_for(&parent, private_va);
    page_table_entry_t *child_private  = leaf_for(&child, private_va);
    page_table_entry_t *child_shared   = leaf_for(&child, shared_va);
    page_table_entry_t *child_readonly = leaf_for(&child, readonly_va);
    CHECK((parent_private->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
    CHECK((child_private->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
    CHECK((child_shared->value & (PTE_SHARED | PTE_WRITEABLE | PTE_COW)) == (PTE_SHARED | PTE_WRITEABLE));
    CHECK((child_readonly->value & (PTE_WRITEABLE | PTE_COW)) == 0);
    CHECK(frame_refcount(private_frame) == 2);
    CHECK(frame_refcount(shared_frame) == 2);
    CHECK(frame_refcount(readonly_frame) == 2);

    page_map_to(&parent, readonly_va, readonly_frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE);
    CHECK((leaf_for(&parent, readonly_va)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);

    CHECK(page_resolve_cow_fault(&child, private_va) == 0);
    child_private             = leaf_for(&child, private_va);
    uint64_t child_frame      = child_private->value & PAGE_4K_MASK;
    CHECK(child_frame != private_frame);
    CHECK((child_private->value & PTE_WRITEABLE) != 0);
    CHECK((child_private->value & PTE_COW) == 0);
    CHECK(bytes_equal(phys_to_virt(child_frame), phys_to_virt(private_frame), PAGE_4K_SIZE));
    ((unsigned char *)phys_to_virt(child_frame))[0] = 0xa5;
    CHECK(((unsigned char *)phys_to_virt(private_frame))[0] == 0x5a);
    CHECK(frame_refcount(private_frame) == 1);

    page_destroy_user_space(&child);
    CHECK(frame_refcount(child_frame) == 0);
    CHECK(frame_refcount(shared_frame) == 1);
    CHECK(frame_refcount(readonly_frame) == 1);

    size_t before_fault_allocations = allocation_attempts;
    CHECK(page_resolve_cow_fault(&parent, private_va) == 0);
    CHECK(allocation_attempts == before_fault_allocations);
    CHECK((leaf_for(&parent, private_va)->value & (PTE_WRITEABLE | PTE_COW)) == PTE_WRITEABLE);

    page_destroy_user_space(&parent);
    CHECK(frame_refcount(private_frame) == 0);
    CHECK(frame_refcount(shared_frame) == 0);
    CHECK(frame_refcount(readonly_frame) == 0);
    CHECK(live_allocated_frames == 0);
}

static void test_huge_leaf_retain_release(void)
{
    reset_backend();
    page_directory_t parent = make_directory();
    page_directory_t child  = make_directory();

    const uintptr_t va_2m = 0x20000000;
    const uintptr_t va_1g = 0x40000000;
    const uint64_t   frame_2m = 0x1000000;
    const uint64_t   frame_1g = 0x40000000;
    mark_owned_range(frame_2m, PAGE_2M_SIZE / PAGE_4K_SIZE);
    mark_owned_range(frame_1g, PAGE_1G_SIZE / PAGE_4K_SIZE);
    bytes_set(phys_to_virt(frame_2m), 0x33, PAGE_2M_SIZE);

    page_map_to_2M(&parent, va_2m, frame_2m, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE);
    page_map_to_1G(&parent, va_1g, frame_1g, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_PCD);
    CHECK(page_clone_user_cow(&child, &parent) == 0);
    CHECK(frame_refcount(frame_2m) == 2);
    CHECK(frame_refcount(frame_2m + PAGE_2M_SIZE - PAGE_4K_SIZE) == 2);
    CHECK(frame_refcount(frame_1g) == 2);
    CHECK(frame_refcount(frame_1g + PAGE_1G_SIZE - PAGE_4K_SIZE) == 2);
    CHECK((leaf_for(&child, va_2m)->value & (PTE_COW | PTE_WRITEABLE | PTE_NO_EXECUTE)) == (PTE_COW | PTE_NO_EXECUTE));
    CHECK((leaf_for(&child, va_1g)->value & (PTE_COW | PTE_WRITEABLE | PTE_PCD)) == (PTE_COW | PTE_PCD));

    CHECK(page_resolve_cow_fault(&child, va_2m) == 0);
    uint64_t child_2m = leaf_for(&child, va_2m)->value & PAGE_2M_MASK;
    CHECK(child_2m != frame_2m);
    CHECK(((unsigned char *)phys_to_virt(child_2m))[0] == 0x33);
    CHECK(((unsigned char *)phys_to_virt(child_2m))[PAGE_2M_SIZE - 1] == 0x33);
    CHECK(frame_refcount(frame_2m) == 1);

    allocation_limit = allocation_attempts;
    CHECK(page_resolve_cow_fault(&child, va_1g) != 0);
    CHECK((leaf_for(&child, va_1g)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
    allocation_limit = SIZE_MAX;

    CHECK(page_resolve_cow_fault(&child, va_1g) == 0);
    uint64_t child_1g = leaf_for(&child, va_1g)->value & PAGE_1G_MASK;
    CHECK(child_1g == 2 * PAGE_1G_SIZE);
    CHECK(sparse_copy_source == frame_1g);
    CHECK(sparse_copy_destination == child_1g);
    CHECK(sparse_copy_length == PAGE_1G_SIZE);
    CHECK(sparse_new_marker == sparse_old_marker);
    CHECK(frame_refcount(frame_1g) == 1);

    page_destroy_user_space(&child);
    size_t attempts_before_1g = allocation_attempts;
    CHECK(page_resolve_cow_fault(&parent, va_1g) == 0);
    CHECK(allocation_attempts == attempts_before_1g);
    CHECK((leaf_for(&parent, va_1g)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_WRITEABLE);
    CHECK(page_unmap_release(&parent, va_2m + PAGE_4K_SIZE) == 0);
    CHECK(frame_refcount(frame_2m) == 1);
    CHECK(frame_refcount(frame_2m + PAGE_4K_SIZE) == 0);
    CHECK(frame_refcount(frame_2m + PAGE_2M_SIZE - PAGE_4K_SIZE) == 1);
    page_destroy_user_space(&parent);
    CHECK(frame_refcount(frame_2m) == 0);
    CHECK(frame_refcount(frame_1g + PAGE_1G_SIZE - PAGE_4K_SIZE) == 0);
    CHECK(live_allocated_frames == 0);
}

static void test_clone_allocation_rollback(void)
{
    reset_backend();
    page_directory_t parent = make_directory();
    page_directory_t child  = make_directory();
    uint64_t         frame  = alloc_frames(1);
    page_map_to(&parent, 0x400000, frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE);

    uint32_t original_refcount = frame_refcount(frame);
    size_t   baseline_live     = live_allocated_frames;
    allocation_limit           = allocation_attempts + 2;

    CHECK(page_clone_user_cow(&child, &parent) != 0);
    CHECK(frame_refcount(frame) == original_refcount);
    CHECK(leaf_for(&parent, 0x400000)->value & PTE_WRITEABLE);
    CHECK(!(leaf_for(&parent, 0x400000)->value & PTE_COW));
    CHECK(child.table->entries[0].value == 0);
    CHECK(live_allocated_frames == baseline_live);

    allocation_limit = SIZE_MAX;
    page_destroy_user_space(&child);
    page_destroy_user_space(&parent);
    CHECK(live_allocated_frames == 0);
}

int main(void)
{
    test_map_status_and_collision();
    test_clone_fault_and_lifetime();
    test_huge_leaf_retain_release();
    test_clone_allocation_rollback();
    return test_failures ? (test_failure_line & 0xff) : 0;
}
