#include <proc/task.h>
#include <stdint.h>
#include <string.h>

uint64_t sched_ticks(void) { return 0; }
void wait_queue_init(wait_queue_t *queue) { memset(queue, 0, sizeof(*queue)); }
task_t *wait_queue_wake_one(wait_queue_t *queue)
{
    (void)queue;
    return NULL;
}
uint64_t wait_queue_wake_all(wait_queue_t *queue)
{
    (void)queue;
    return 0;
}
