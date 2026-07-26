/*
 * Linux-compatible evdev per-client packet queue.
 */

#include <drivers/evdev_queue.h>

static bool is_power_of_two(unsigned int value)
{
    return value && !(value & (value - 1));
}

bool evdev_queue_init(evdev_queue_t *queue, input_event_t *buffer, unsigned int size)
{
    if (!queue || !buffer || size < 4 || !is_power_of_two(size)) return false;

    queue->head        = 0;
    queue->tail        = 0;
    queue->packet_head = 0;
    queue->size        = size;
    queue->buffer      = buffer;
    return true;
}

bool evdev_queue_has_packet(const evdev_queue_t *queue)
{
    return queue && queue->packet_head != queue->tail;
}

bool evdev_queue_push(evdev_queue_t *queue, const input_event_t *event)
{
    unsigned int mask;

    if (!queue || !queue->buffer || !event) return false;

    if (event->type == EV_SYN && event->code == SYN_REPORT && queue->packet_head == queue->head) return false;

    mask                         = queue->size - 1;
    queue->buffer[queue->head++] = *event;
    queue->head &= mask;

    if (queue->head == queue->tail) {
        queue->tail                = (queue->head - 2) & mask;
        queue->buffer[queue->tail] = (input_event_t) {
            .sec   = event->sec,
            .usec  = event->usec,
            .type  = EV_SYN,
            .code  = SYN_DROPPED,
            .value = 0,
        };
        queue->packet_head = queue->tail;
    }

    if (event->type == EV_SYN && event->code == SYN_REPORT) {
        queue->packet_head = queue->head;
        return true;
    }
    return false;
}

size_t evdev_queue_read(evdev_queue_t *queue, input_event_t *events, size_t max_events)
{
    size_t       count = 0;
    unsigned int mask;

    if (!queue || !queue->buffer || !events || !max_events) return 0;

    mask = queue->size - 1;
    while (count < max_events && evdev_queue_has_packet(queue)) {
        events[count++] = queue->buffer[queue->tail];
        queue->tail     = (queue->tail + 1) & mask;
    }
    return count;
}

void evdev_queue_flush_type(evdev_queue_t *queue, unsigned int type)
{
    unsigned int index;
    unsigned int head;
    unsigned int count;
    unsigned int mask;

    if (!queue || !queue->buffer || type == EV_SYN) return;

    mask               = queue->size - 1;
    head               = queue->tail;
    queue->packet_head = queue->tail;
    count              = 1;

    for (index = queue->tail; index != queue->head; index = (index + 1) & mask) {
        input_event_t *event     = &queue->buffer[index];
        bool           is_report = event->type == EV_SYN && event->code == SYN_REPORT;

        if (event->type == type)
            continue;
        else if (is_report && !count)
            continue;
        else if (head != index)
            queue->buffer[head] = *event;

        count++;
        head = (head + 1) & mask;
        if (is_report) {
            count              = 0;
            queue->packet_head = head;
        }
    }
    queue->head = head;
}

void evdev_queue_discard_pending(evdev_queue_t *queue, const input_event_t *syn_dropped)
{
    if (!queue || !syn_dropped || queue->head == queue->tail) return;

    queue->head = queue->packet_head = queue->tail;
    (void)evdev_queue_push(queue, syn_dropped);
}
