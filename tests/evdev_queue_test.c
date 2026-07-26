#include <drivers/evdev_queue.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                                                                                          \
    do {                                                                                                                                   \
        if (!(condition)) {                                                                                                                \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, message);                                                                     \
            failures++;                                                                                                                    \
            return;                                                                                                                        \
        }                                                                                                                                  \
    } while (0)

static input_event_t event(uint16_t type, uint16_t code, int32_t value)
{
    return (input_event_t) {.sec = 7, .usec = 11, .type = type, .code = code, .value = value};
}

static bool push(evdev_queue_t *queue, uint16_t type, uint16_t code, int32_t value)
{
    input_event_t input = event(type, code, value);

    return evdev_queue_push(queue, &input);
}

static void test_incomplete_frame_is_not_readable(void)
{
    input_event_t storage[8];
    input_event_t output[8];
    evdev_queue_t queue;

    CHECK(evdev_queue_init(&queue, storage, 8), "queue init");
    CHECK(!push(&queue, EV_KEY, KEY_A, 1), "key must not complete a frame");
    CHECK(!evdev_queue_has_packet(&queue), "incomplete frame reported readable");
    CHECK(evdev_queue_read(&queue, output, 8) == 0, "incomplete frame was returned");

    CHECK(push(&queue, EV_SYN, SYN_REPORT, 0), "SYN_REPORT must complete frame");
    CHECK(evdev_queue_has_packet(&queue), "completed frame not readable");
    CHECK(evdev_queue_read(&queue, output, 1) == 1, "short read did not return one whole event");
    CHECK(output[0].type == EV_KEY && output[0].code == KEY_A, "wrong first event");
    CHECK(evdev_queue_has_packet(&queue), "remaining SYN_REPORT lost after short read");
    CHECK(evdev_queue_read(&queue, output, 8) == 1, "remaining report not returned");
    CHECK(output[0].type == EV_SYN && output[0].code == SYN_REPORT, "wrong report event");
}

static void test_empty_syn_report_is_dropped(void)
{
    input_event_t storage[8];
    evdev_queue_t queue;

    CHECK(evdev_queue_init(&queue, storage, 8), "queue init");
    CHECK(!push(&queue, EV_SYN, SYN_REPORT, 0), "empty report completed a frame");
    CHECK(queue.head == queue.tail, "empty report consumed queue capacity");
}

static void test_overflow_resynchronizes_with_syn_dropped(void)
{
    input_event_t storage[8];
    input_event_t output[8];
    evdev_queue_t queue;

    CHECK(evdev_queue_init(&queue, storage, 8), "queue init");
    for (int i = 0; i < 3; i++) {
        CHECK(!push(&queue, EV_REL, REL_X, i + 1), "motion completed frame early");
        CHECK(push(&queue, EV_SYN, SYN_REPORT, 0), "report did not complete frame");
    }
    CHECK(!push(&queue, EV_REL, REL_X, 8), "last free slot completed frame");
    CHECK(!push(&queue, EV_REL, REL_Y, 9), "overflowed event must await report");
    CHECK(!evdev_queue_has_packet(&queue), "overflow recovery exposed incomplete frame");
    CHECK(push(&queue, EV_SYN, SYN_REPORT, 0), "recovery report missing");

    CHECK(evdev_queue_read(&queue, output, 8) == 3, "overflow recovery frame size");
    CHECK(output[0].type == EV_SYN && output[0].code == SYN_DROPPED, "missing SYN_DROPPED");
    CHECK(output[0].sec == 7 && output[0].usec == 11, "SYN_DROPPED timestamp changed");
    CHECK(output[1].type == EV_REL && output[1].code == REL_Y && output[1].value == 9, "newest event not retained");
    CHECK(output[2].type == EV_SYN && output[2].code == SYN_REPORT, "recovery frame not terminated");
}

static void test_flush_preserves_only_complete_packet_boundaries(void)
{
    input_event_t storage[16];
    input_event_t output[16];
    evdev_queue_t queue;

    CHECK(evdev_queue_init(&queue, storage, 16), "queue init");
    push(&queue, EV_KEY, KEY_A, 1);
    push(&queue, EV_REL, REL_X, 3);
    push(&queue, EV_SYN, SYN_REPORT, 0);
    push(&queue, EV_KEY, KEY_A, 0);
    push(&queue, EV_SYN, SYN_REPORT, 0);
    push(&queue, EV_REL, REL_Y, 4);

    evdev_queue_flush_type(&queue, EV_KEY);
    CHECK(evdev_queue_has_packet(&queue), "retained complete relative frame not readable");
    CHECK(evdev_queue_read(&queue, output, 16) == 2, "flush retained wrong completed event count");
    CHECK(output[0].type == EV_REL && output[0].code == REL_X, "relative event removed");
    CHECK(output[1].type == EV_SYN && output[1].code == SYN_REPORT, "completed frame boundary removed");
    CHECK(!evdev_queue_has_packet(&queue), "incomplete relative tail exposed");

    push(&queue, EV_SYN, SYN_REPORT, 0);
    CHECK(evdev_queue_read(&queue, output, 16) == 2, "incomplete tail did not complete later");
    CHECK(output[0].code == REL_Y && output[1].code == SYN_REPORT, "wrong tail contents");
}

int main(void)
{
    test_incomplete_frame_is_not_readable();
    test_empty_syn_report_is_dropped();
    test_overflow_resynchronizes_with_syn_dropped();
    test_flush_preserves_only_complete_packet_boundaries();

    if (failures) {
        printf("%d evdev queue test(s) failed\n", failures);
        return 1;
    }
    printf("PASS evdev queue packet, overflow, and flush semantics\n");
    return 0;
}
