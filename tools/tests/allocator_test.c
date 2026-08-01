#include <assert.h>
#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/slab.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sync/spin_lock.h>

#define TEST_ARENA_SIZE (32U * 1024U * 1024U)
#define STRESS_SLOTS    512U
#define STRESS_STEPS    50000U

static uint8_t  test_arena[TEST_ARENA_SIZE] __attribute__((aligned(4096)));
static unsigned heap_errors;
static unsigned ctor_calls;
static unsigned dtor_calls;

/* Host tests are single-threaded; kernel builds use the real IRQ-safe locks. */
uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock);
    while (__atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE)) sched_yield();
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags)
{
    (void)rflags;
    assert(lock && lock->lock);
    __atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
}

void spin_lock(spinlock_t *lock)
{
    (void)spin_lock_irqsave(lock);
}

void spin_unlock(spinlock_t *lock)
{
    spin_unlock_irqrestore(lock, 0);
}

static void allocator_error(heap_error_t error, void *pointer)
{
    assert(error == invalid_free || error == layout_error);
    assert(pointer);
    heap_errors++;
}

static void object_ctor(void *object)
{
    memset(object, 0x3c, 128);
    ctor_calls++;
}

static void object_dtor(void *object)
{
    assert(((uint8_t *)object)[0] == 0x3c);
    dtor_calls++;
}

static uint32_t random_state = 0x12345678U;

static uint32_t next_random(void)
{
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    random_state = x;
    return x;
}

static void test_buddy(void)
{
    static buddy_page_t pages[4096];
    buddy_allocator_t   allocator;
    assert(buddy_init(&allocator, pages, 4096, 12) == 0);
    assert(buddy_add_range(&allocator, 0, 4096) == 0);
    assert(allocator.free_pages == 4096);
    assert(allocator.free_count[12] == 1);
    assert(buddy_validate(&allocator) == 0);

    size_t blocks[13];
    for (unsigned order = 0; order <= 12; order++) blocks[order] = SIZE_MAX;
    for (unsigned order = 0; order < 10; order++) {
        blocks[order] = buddy_alloc(&allocator, order);
        assert(blocks[order] != SIZE_MAX);
        assert((blocks[order] & (((size_t)1 << order) - 1)) == 0);
        assert(buddy_validate(&allocator) == 0);
    }
    for (unsigned order = 10; order-- > 0;) {
        assert(buddy_free(&allocator, blocks[order], order) == 0);
        assert(buddy_validate(&allocator) == 0);
    }
    assert(allocator.free_pages == 4096);
    assert(allocator.free_count[12] == 1);

    size_t exact = buddy_alloc(&allocator, 8);
    assert(exact != SIZE_MAX);
    assert(buddy_trim_allocation(&allocator, exact, 8, 130) == 0);
    assert(allocator.free_pages == 4096 - 130);
    for (size_t i = 0; i < 130; i++) assert(buddy_free(&allocator, exact + i, 0) == 0);
    assert(allocator.free_pages == 4096);
    assert(allocator.free_count[12] == 1);
    assert(buddy_validate(&allocator) == 0);

    /* Bad order and duplicate frees must fail without damaging the lists. */
    size_t one = buddy_alloc(&allocator, 0);
    assert(one != SIZE_MAX);
    assert(buddy_free(&allocator, one, 1) != 0);
    assert(buddy_free(&allocator, one, 0) == 0);
    assert(buddy_free(&allocator, one, 0) != 0);
    assert(buddy_validate(&allocator) == 0);
}

typedef struct {
        void   *pointer;
        size_t  size;
        uint8_t pattern;
} stress_slot_t;

static void check_slot(const stress_slot_t *slot)
{
    if (!slot->pointer) return;
    const uint8_t *bytes = slot->pointer;
    for (size_t i = 0; i < slot->size; i++) assert(bytes[i] == slot->pattern);
}

static void test_heap_stress(void)
{
    stress_slot_t slots[STRESS_SLOTS] = {0};
    for (size_t step = 0; step < STRESS_STEPS; step++) {
        size_t         index = next_random() % STRESS_SLOTS;
        stress_slot_t *slot  = &slots[index];
        if (!slot->pointer) {
            size_t size   = 1 + next_random() % 32768;
            slot->pointer = malloc(size);
            assert(slot->pointer);
            assert(((uintptr_t)slot->pointer & 15U) == 0);
            assert(usable_size(slot->pointer) >= size);
            slot->size    = size;
            slot->pattern = (uint8_t)(next_random() | 1U);
            memset(slot->pointer, slot->pattern, size);
        } else if ((next_random() & 3U) == 0) {
            check_slot(slot);
            size_t new_size    = 1 + next_random() % 65536;
            size_t preserved   = slot->size < new_size ? slot->size : new_size;
            void  *replacement = realloc(slot->pointer, new_size);
            assert(replacement);
            for (size_t i = 0; i < preserved; i++) assert(((uint8_t *)replacement)[i] == slot->pattern);
            slot->pointer = replacement;
            slot->size    = new_size;
            memset(slot->pointer, slot->pattern, new_size);
        } else {
            check_slot(slot);
            free(slot->pointer);
            memset(slot, 0, sizeof(*slot));
        }
        if ((step & 1023U) == 0) assert(heap_validate() == 0);
    }
    for (size_t i = 0; i < STRESS_SLOTS; i++) {
        check_slot(&slots[i]);
        free(slots[i].pointer);
    }
    assert(heap_validate() == 0);
}

static uint32_t thread_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void *concurrent_worker(void *argument)
{
    stress_slot_t slots[64] = {0};
    uint32_t      state     = 0x6d2b79f5U ^ (uint32_t)(uintptr_t)argument;
    for (size_t step = 0; step < 12000; step++) {
        stress_slot_t *slot = &slots[thread_random(&state) % 64];
        if (!slot->pointer) {
            slot->size    = 1 + thread_random(&state) % 16384;
            slot->pointer = malloc(slot->size);
            assert(slot->pointer);
            slot->pattern = (uint8_t)(thread_random(&state) | 1U);
            memset(slot->pointer, slot->pattern, slot->size);
        } else {
            check_slot(slot);
            free(slot->pointer);
            memset(slot, 0, sizeof(*slot));
        }
    }
    for (size_t i = 0; i < 64; i++) {
        check_slot(&slots[i]);
        free(slots[i].pointer);
    }
    return NULL;
}

static void test_heap_concurrent(void)
{
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, concurrent_worker, (void *)(i + 1)) == 0);
    for (size_t i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(heap_validate() == 0);
}

static void test_heap(void)
{
    assert(heap_init(test_arena, sizeof(test_arena)) == 0);
    heap_onerror(allocator_error);
    assert(heap_validate() == 0);
    assert(malloc(0) == NULL);
    assert(aligned_alloc(3, 64) == NULL);

    for (size_t alignment = 16; alignment <= 4096; alignment <<= 1) {
        void *pointer = aligned_alloc(alignment, alignment + 37);
        assert(pointer);
        assert(((uintptr_t)pointer & (alignment - 1)) == 0);
        memset(pointer, 0xa7, alignment + 37);
        free(pointer);
    }

    void *bad = malloc(80);
    assert(bad);
    free((uint8_t *)bad + 1);
    assert(heap_errors == 1);
    free(bad);
    free(bad);
    assert(heap_errors == 2);

    slab_cache_t *cache = slab_cache_create("test-object", 128, 64, object_ctor, object_dtor);
    assert(cache);
    void *objects[300];
    for (size_t i = 0; i < 300; i++) {
        objects[i] = slab_cache_alloc(cache);
        assert(objects[i]);
        assert(((uintptr_t)objects[i] & 63U) == 0);
    }
    slab_cache_stats_t cache_stats;
    slab_cache_get_stats(cache, &cache_stats);
    assert(cache_stats.objects == 300 && cache_stats.slabs > 1);
    for (size_t i = 0; i < 300; i++) assert(slab_cache_free(cache, objects[i]) == 0);
    assert(ctor_calls == 300 && dtor_calls == 300);
    assert(slab_cache_shrink(cache) > 0);
    assert(slab_cache_destroy(cache) == 0);

    test_heap_stress();
    test_heap_concurrent();

    heap_stats_t stats;
    heap_get_stats(&stats);
    assert(stats.arena_bytes == sizeof(test_arena));
    assert(stats.metadata_bytes > 0);
    assert(stats.live_allocations == 0);
    assert(stats.allocated_bytes == 0);
    assert(stats.allocation_calls == stats.free_calls);
}

int main(void)
{
    test_buddy();
    test_heap();
    puts("PASS buddy/slab allocator invariants, API semantics and stress tests");
    return 0;
}
