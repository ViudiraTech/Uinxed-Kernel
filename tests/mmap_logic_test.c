#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <syscall/mmap.h>

extern int printf(const char *format, ...);

#define TEST_BASE  0x400000ULL
#define MAX_FRAMES 16
#define MAX_MAPS   16

typedef struct {
    uint64_t      phys;
    unsigned char data[PAGE_4K_SIZE];
    int           released;
} mock_frame_t;

typedef struct {
    uint64_t va;
    uint64_t phys;
    uint64_t flags;
} mock_map_t;

static process_t    test_process;
static vm_area_t    test_vma;
static mock_frame_t frames[MAX_FRAMES];
static mock_map_t   maps[MAX_MAPS];
static size_t       frame_count;
static size_t       map_count;
static int          allocation_limit;
static int          map_failure_after;
static int          tests_failed;

#define EXPECT(condition)                                                                 \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            printf("assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 0;                                                                     \
        }                                                                                 \
    } while (0)

process_t *process_current(void)
{
    return &test_process;
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
    if (count != 1 || frame_count == MAX_FRAMES || allocation_limit == 0) return 0;
    if (allocation_limit > 0) allocation_limit--;
    mock_frame_t *frame = &frames[frame_count++];
    frame->phys         = frame_count * PAGE_4K_SIZE;
    frame->released     = 0;
    return frame->phys;
}

void *phys_to_virt(uint64_t phys)
{
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].phys == phys) return frames[i].data;
    }
    return NULL;
}

int frame_release_range(uint64_t phys, size_t count)
{
    if (count != 1) return -1;
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].phys == phys) {
            frames[i].released = 1;
            return 0;
        }
    }
    return -1;
}

int page_map_new_to(page_directory_t *directory, uint64_t va, uint64_t phys, uint64_t flags)
{
    (void)directory;
    if (map_failure_after == 0 || map_count == MAX_MAPS) return -1;
    if (map_failure_after > 0) map_failure_after--;
    for (size_t i = 0; i < map_count; i++) {
        if (maps[i].va == va) return -1;
    }
    maps[map_count++] = (mock_map_t){va, phys, flags};
    return 0;
}

uint64_t page_unmap(page_directory_t *directory, uint64_t va)
{
    (void)directory;
    for (size_t i = 0; i < map_count; i++) {
        if (maps[i].va == va) {
            uint64_t phys = maps[i].phys;
            maps[i]       = maps[--map_count];
            return phys;
        }
    }
    return 0;
}

int page_unmap_release(page_directory_t *directory, uint64_t va)
{
    uint64_t phys = page_unmap(directory, va);
    return phys ? frame_release_range(phys, 1) : 0;
}

static int reset_mapping(size_t pages, vm_flags_t flags)
{
    memset(&test_process, 0, sizeof(test_process));
    memset(&test_vma, 0, sizeof(test_vma));
    memset(frames, 0, sizeof(frames));
    memset(maps, 0, sizeof(maps));
    frame_count                = 0;
    map_count                  = 0;
    allocation_limit           = -1;
    map_failure_after          = -1;
    test_vma.start             = TEST_BASE;
    test_vma.end               = TEST_BASE + pages * PAGE_4K_SIZE;
    test_vma.flags             = flags;
    test_vma.type              = VM_REGION_MMAP;
    test_vma.vm_pgoff          = 37;
    test_process.mmap_list     = &test_vma;
    test_process.user_page_dir = (page_directory_t *)(uintptr_t)1;
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = alloc_frames(1);
        memset(phys_to_virt(phys), (int)(0x40 + i), PAGE_4K_SIZE);
        EXPECT(page_map_new_to(test_process.user_page_dir, TEST_BASE + i * PAGE_4K_SIZE, phys, PTE_USER | PTE_PRESENT) == 0);
    }
    return 1;
}

static int old_data_is_intact(size_t pages)
{
    for (size_t i = 0; i < pages; i++) {
        unsigned char *data = phys_to_virt(maps[i].phys);
        if (!data || data[0] != (unsigned char)(0x40 + i) || data[PAGE_4K_SIZE - 1] != (unsigned char)(0x40 + i)) return 0;
    }
    return 1;
}

static int test_growth_preserves_pages_and_metadata(void)
{
    EXPECT(reset_mapping(2, VM_READ | VM_EXEC | VM_SHARED));
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 4 * PAGE_4K_SIZE, MREMAP_MAYMOVE, 0) == TEST_BASE);
    EXPECT(map_count == 4 && old_data_is_intact(2));
    EXPECT(test_vma.start == TEST_BASE && test_vma.end == TEST_BASE + 4 * PAGE_4K_SIZE);
    EXPECT(test_vma.flags == (VM_READ | VM_EXEC | VM_SHARED) && test_vma.vm_pgoff == 37);
    EXPECT(((unsigned char *)phys_to_virt(maps[2].phys))[0] == 0);
    EXPECT(((unsigned char *)phys_to_virt(maps[3].phys))[PAGE_4K_SIZE - 1] == 0);
    return 1;
}

static int test_allocation_failure_rolls_back(void)
{
    EXPECT(reset_mapping(2, VM_READ | VM_WRITE));
    allocation_limit = 1;
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 4 * PAGE_4K_SIZE, MREMAP_MAYMOVE, 0) == -ENOMEM);
    EXPECT(map_count == 2 && test_vma.end == TEST_BASE + 2 * PAGE_4K_SIZE);
    EXPECT(old_data_is_intact(2) && frames[2].released);
    return 1;
}

static int test_page_table_failure_rolls_back(void)
{
    EXPECT(reset_mapping(2, VM_READ | VM_WRITE));
    map_failure_after = 1;
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 4 * PAGE_4K_SIZE, 0, 0) == -ENOMEM);
    EXPECT(map_count == 2 && old_data_is_intact(2));
    EXPECT(frames[2].released && frames[3].released);
    return 1;
}

static int test_occupied_extension_fails_without_replacement(void)
{
    EXPECT(reset_mapping(2, VM_READ));
    vm_area_t blocker = {.start = TEST_BASE + 2 * PAGE_4K_SIZE, .end = TEST_BASE + 3 * PAGE_4K_SIZE, .flags = VM_WRITE};
    test_vma.next     = &blocker;
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 3 * PAGE_4K_SIZE, MREMAP_MAYMOVE, 0) == -ENOMEM);
    EXPECT(map_count == 2 && test_vma.end == TEST_BASE + 2 * PAGE_4K_SIZE && old_data_is_intact(2));
    return 1;
}

static int test_fixed_move_is_safe_failure(void)
{
    EXPECT(reset_mapping(2, VM_READ | VM_WRITE));
    uint64_t destination = TEST_BASE + 8 * PAGE_4K_SIZE;
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 3 * PAGE_4K_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, destination) == -ENOMEM);
    EXPECT(map_count == 2 && test_vma.start == TEST_BASE && test_vma.end == TEST_BASE + 2 * PAGE_4K_SIZE);
    EXPECT(old_data_is_intact(2));
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, PAGE_4K_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, destination) == -ENOMEM);
    EXPECT(map_count == 2 && test_vma.end == TEST_BASE + 2 * PAGE_4K_SIZE && old_data_is_intact(2));
    return 1;
}

static int test_file_growth_is_safe_failure(void)
{
    EXPECT(reset_mapping(2, VM_READ | VM_SHARED));
    test_vma.vm_file = (vfs_node_t)(uintptr_t)1;
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 3 * PAGE_4K_SIZE, MREMAP_MAYMOVE, 0) == -ENOMEM);
    EXPECT(map_count == 2 && test_vma.end == TEST_BASE + 2 * PAGE_4K_SIZE && test_vma.vm_pgoff == 37);
    EXPECT(old_data_is_intact(2));
    return 1;
}

static int test_fixed_validation(void)
{
    EXPECT(reset_mapping(2, VM_READ));
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 3 * PAGE_4K_SIZE, MREMAP_FIXED, TEST_BASE) == -EINVAL);
    EXPECT(sys_mremap(TEST_BASE, 2 * PAGE_4K_SIZE, 3 * PAGE_4K_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED,
                      TEST_BASE + PAGE_4K_SIZE) == -EINVAL);
    EXPECT(map_count == 2 && old_data_is_intact(2));
    return 1;
}

static void run_test(const char *name, int (*test)(void))
{
    if (test()) {
        printf("PASS %s\n", name);
    } else {
        tests_failed++;
        printf("FAIL %s\n", name);
    }
}

int main(void)
{
    run_test("growth preserves pages and metadata", test_growth_preserves_pages_and_metadata);
    run_test("allocation failure rolls back", test_allocation_failure_rolls_back);
    run_test("page-table failure rolls back", test_page_table_failure_rolls_back);
    run_test("occupied extension is safe failure", test_occupied_extension_fails_without_replacement);
    run_test("fixed move is safe failure", test_fixed_move_is_safe_failure);
    run_test("file growth is safe failure", test_file_growth_is_safe_failure);
    run_test("fixed validation", test_fixed_validation);
    return tests_failed ? 1 : 0;
}
