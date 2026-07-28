#include <ipc/futex.h>
#include <kernel/errno.h>
#include <proc/task.h>
#include <sync/rt_mutex.h>
#include <stdio.h>

void *memcpy(void *dst, const void *src, size_t size);
void *memset(void *dst, int value, size_t size);

static int       failures;
static uint64_t  now_ticks;
static uint64_t  realtime_ticks;
static uint64_t  observed_deadline;
static int       timed_result;
static int       wake_during_wait;
static uint32_t *wake_address;
static task_t    tasks[4];
static task_t   *mock_current = &tasks[0];

#define CHECK(condition, message)                                                                                                          \
    do {                                                                                                                                   \
        if (!(condition)) {                                                                                                                \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, message);                                                                     \
            failures++;                                                                                                                    \
            return;                                                                                                                        \
        }                                                                                                                                  \
    } while (0)

int ilist_init(ilist_node_t *list)
{
    list->next = list;
    list->prev = list;
    return 0;
}

int ilist_insert_before(ilist_node_t *node, ilist_node_t *new_node)
{
    new_node->prev   = node->prev;
    new_node->next   = node;
    node->prev->next = new_node;
    node->prev       = new_node;
    return 0;
}

int ilist_remove(ilist_node_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = node;
    return 0;
}

int ilist_is_empty(const ilist_node_t *list) { return list->next == list; }
void spin_lock(spinlock_t *lock) { (void)lock; }
void spin_unlock(spinlock_t *lock) { (void)lock; }
uint64_t sched_ticks(void) { return now_ticks; }
uint64_t test_futex_realtime_ticks(void) { return realtime_ticks; }
task_t *current_task(void) { return mock_current; }
void task_block(void) {}
int task_wakeup(task_t *task) { return task != NULL; }
task_t *pid_find_task(uint64_t pid)
{
    (void)pid;
    return NULL;
}

int user_access_ok(const void *address, size_t size, int write)
{
    (void)size;
    (void)write;
    return address == (void *)1;
}

int copy_from_user(void *dst, const void *src, size_t size)
{
    if (src == (void *)1) return -1;
    memcpy(dst, src, size);
    return 0;
}

int copy_to_user(void *dst, const void *src, size_t size)
{
    if (dst == (void *)1) return -1;
    memcpy(dst, src, size);
    return 0;
}

void wait_queue_init(wait_queue_t *queue)
{
    ilist_init(&queue->tasks);
    queue->lock.lock = 0;
}

void wait_queue_prepare(wait_queue_t *queue)
{
    mock_current->wait_queue = queue;
    ilist_init(&mock_current->sched_node);
    ilist_insert_before(&queue->tasks, &mock_current->sched_node);
}

task_t *wait_queue_wake_one(wait_queue_t *queue)
{
    if (ilist_is_empty(&queue->tasks)) return NULL;
    task_t *task = (task_t *)((char *)queue->tasks.next - offsetof(task_t, sched_node));
    ilist_remove(&task->sched_node);
    task->wait_queue = NULL;
    return task;
}

void wait_queue_sleep(void)
{
    if (wake_during_wait) futex_wake(wake_address, 1, FUTEX_BITSET_MATCH_ANY);
}

int wait_queue_wait_timed(wait_queue_t *queue, uint64_t deadline)
{
    observed_deadline = deadline;
    if (wake_during_wait) {
        futex_wake(wake_address, 1, FUTEX_BITSET_MATCH_ANY);
        return 0;
    }
    wait_queue_wake_one(queue);
    return timed_result;
}

void rt_mutex_init(rt_mutex_t *mutex, uint32_t *uaddr)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->uaddr = uaddr;
}
void pi_waiter_add(task_t *task, rt_mutex_t *mutex) { (void)task, (void)mutex; }
void pi_propagate_chain(task_t *task) { (void)task; }
void pi_waiter_augment(rb_node_t *node, void *data) { (void)node, (void)data; }
rb_node_t *rb_first(rb_root_t *root)
{
    (void)root;
    return NULL;
}
void rb_erase_augmented(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data)
{
    (void)root;
    (void)node;
    (void)augment;
    (void)data;
}

#define FUTEX_REALTIME_TICKS() test_futex_realtime_ticks()
#include "../ipc/futex.c"
#undef FUTEX_REALTIME_TICKS

static int64_t call(uint32_t *word, int op, uint32_t value, void *timeout, uint32_t bitset)
{
    return sys_futex(word, op, value, (uint64_t)timeout, NULL, bitset);
}

static void reset(void)
{
    memset(tasks, 0, sizeof(tasks));
    mock_current      = &tasks[0];
    now_ticks         = 100;
    realtime_ticks    = 1000;
    observed_deadline = 0;
    timed_result      = -ETIMEDOUT;
    wake_during_wait  = 0;
    wake_address      = NULL;
    futex_init();
}

static void test_wake_returns_zero_with_unchanged_word(void)
{
    uint32_t word = 7;
    reset();
    wake_during_wait = 1;
    wake_address     = &word;
    CHECK(call(&word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 7, NULL, 0) == 0, "normal wake must succeed");
    CHECK(word == 7, "test must leave futex word unchanged");
}

static void test_wait_relative_timeout(void)
{
    uint32_t word = 3;
    struct { int64_t sec, nsec; } timeout = {2, 1};
    reset();
    CHECK(call(&word, FUTEX_WAIT, 3, &timeout, 0) == -ETIMEDOUT, "relative wait result");
    CHECK(observed_deadline == 301, "relative timeout was not rounded up and added to now");
}

static void test_wait_bitset_absolute_deadline(void)
{
    uint32_t word = 3;
    struct { int64_t sec, nsec; } timeout = {5, 0};
    reset();
    CHECK(call(&word, FUTEX_WAIT_BITSET, 3, &timeout, 1) == -ETIMEDOUT, "absolute wait result");
    CHECK(observed_deadline == 500, "WAIT_BITSET deadline became relative");
    timeout.sec = 11;
    CHECK(call(&word, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 3, &timeout, 1) == -ETIMEDOUT,
          "realtime absolute wait result");
    CHECK(observed_deadline == 200, "realtime deadline was not translated to monotonic time");
}

static void test_timespec_and_flag_validation(void)
{
    uint32_t word = 1;
    struct { int64_t sec, nsec; } bad_nsec = {0, 1000000000};
    struct { int64_t sec, nsec; } negative = {-1, 0};
    struct { int64_t sec, nsec; } overflow = {0x7fffffffffffffffLL, 999999999};
    reset();
    CHECK(call(&word, FUTEX_WAIT, 1, (void *)1, 0) == -EFAULT, "timespec copy failure");
    CHECK(call(&word, FUTEX_WAIT, 1, &bad_nsec, 0) == -EINVAL, "nanoseconds range");
    CHECK(call(&word, FUTEX_WAIT, 1, &negative, 0) == -EINVAL, "negative seconds");
    CHECK(call(&word, FUTEX_WAIT, 1, &overflow, 0) == -EINVAL, "timespec tick overflow");
    CHECK(call(&word, FUTEX_WAIT | FUTEX_CLOCK_REALTIME, 1, NULL, 0) == -EINVAL, "realtime accepted for WAIT");
    CHECK(call(&word, FUTEX_WAKE | 0x400, 1, NULL, 0) == -EINVAL, "unknown flag accepted");
    CHECK(call(&word, FUTEX_WAIT_BITSET, 1, NULL, 0) == -EINVAL, "zero bitset accepted");
    CHECK(call(&word, FUTEX_WAKE_BITSET, 1, NULL, 0) == -EINVAL, "zero wake bitset accepted");
}

static void test_wake_timeout_race_result(void)
{
    uint32_t word = 9;
    struct { int64_t sec, nsec; } timeout = {0, 1};
    reset();
    wake_during_wait = 1;
    wake_address     = &word;
    timed_result     = -ETIMEDOUT;
    CHECK(call(&word, FUTEX_WAIT, 9, &timeout, 0) == 0, "normal wake lost at timeout boundary");
}

static void test_wake_bitset_selects_matching_waiter(void)
{
    uint32_t word = 4;
    reset();

    futex_bucket_t *bucket = &futex_hash[futex_hash_index(&word)];
    futex_entry_t  *first  = futex_create_waiter(bucket, &word, 1);
    futex_entry_t  *second = futex_create_waiter(bucket, &word, 2);
    CHECK(first && second, "waiter allocation");

    mock_current = &tasks[0];
    wait_queue_prepare(&first->wq);
    mock_current = &tasks[1];
    wait_queue_prepare(&second->wq);

    CHECK(futex_wake(&word, 1, 1) == 1, "matching waiter was not woken");
    CHECK(tasks[0].wait_queue == NULL, "matching waiter remained queued");
    CHECK(tasks[1].wait_queue == &second->wq, "nonmatching waiter was woken");

    wait_queue_wake_one(&second->wq);
    futex_remove_entry_locked(bucket, first);
    futex_remove_entry_locked(bucket, second);
}

int main(void)
{
    test_wake_returns_zero_with_unchanged_word();
    test_wait_relative_timeout();
    test_wait_bitset_absolute_deadline();
    test_timespec_and_flag_validation();
    test_wake_timeout_race_result();
    test_wake_bitset_selects_matching_waiter();
    if (failures) return 1;
    printf("futex tests passed\n");
    return 0;
}
