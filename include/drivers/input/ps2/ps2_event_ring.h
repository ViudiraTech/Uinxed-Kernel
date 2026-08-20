/*
 *
 *      ps2_event_ring.h
 *      Lock-free single-producer/single-consumer ring used by each PS/2 port.
 *      The i8042 controller lock serializes producers; each ring has one worker.
 *
 *      2026/8/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PS2_EVENT_RING_H_
#define INCLUDE_PS2_EVENT_RING_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define PS2_EVENT_QUEUE_SIZE 1024U
#define PS2_EVENT_BATCH_SIZE 64U
#define PS2_EVENT_QUEUE_MASK (PS2_EVENT_QUEUE_SIZE - 1U)
#define PS2_CACHELINE_SIZE    64U

#if (PS2_EVENT_QUEUE_SIZE & PS2_EVENT_QUEUE_MASK) != 0
#    error "PS2_EVENT_QUEUE_SIZE must be a power of two"
#endif

struct ps2_queued_byte {
        uint8_t status;
        uint8_t data;
};

/* Keep producer-owned and consumer-owned counters on separate cache lines. */
struct __attribute__((aligned(PS2_CACHELINE_SIZE))) ps2_event_ring_producer {
        uint32_t head;
        uint32_t tail_cache;
};

struct __attribute__((aligned(PS2_CACHELINE_SIZE))) ps2_event_ring_consumer {
        uint32_t tail;
        uint32_t head_cache;
};

struct ps2_event_ring {
        struct ps2_event_ring_producer producer;
        struct ps2_event_ring_consumer consumer;
        struct ps2_queued_byte         entries[PS2_EVENT_QUEUE_SIZE];
} __attribute__((aligned(PS2_CACHELINE_SIZE)));

static inline void ps2_event_ring_init(struct ps2_event_ring *ring)
{
    __atomic_store_n(&ring->producer.head, 0, __ATOMIC_RELAXED);
    ring->producer.tail_cache = 0;
    __atomic_store_n(&ring->consumer.tail, 0, __ATOMIC_RELAXED);
    ring->consumer.head_cache = 0;
}

/* Called only by the controller-serialized producer for this port. */
static inline bool ps2_event_ring_push(struct ps2_event_ring *ring, struct ps2_queued_byte event)
{
    uint32_t head = __atomic_load_n(&ring->producer.head, __ATOMIC_RELAXED);
    uint32_t tail = ring->producer.tail_cache;

    if ((uint32_t)(head - tail) >= PS2_EVENT_QUEUE_SIZE) {
        tail                      = __atomic_load_n(&ring->consumer.tail, __ATOMIC_ACQUIRE);
        ring->producer.tail_cache = tail;
        if ((uint32_t)(head - tail) >= PS2_EVENT_QUEUE_SIZE) return false;
    }

    ring->entries[head & PS2_EVENT_QUEUE_MASK] = event;
    __atomic_store_n(&ring->producer.head, head + 1U, __ATOMIC_RELEASE);
    return true;
}

/* Called only by this port's worker; publishes one tail update per batch. */
static inline size_t ps2_event_ring_pop_batch(struct ps2_event_ring *ring, struct ps2_queued_byte *events, size_t capacity)
{
    uint32_t tail;
    uint32_t head;
    size_t   count;

    if (!capacity) return 0;

    tail = __atomic_load_n(&ring->consumer.tail, __ATOMIC_RELAXED);
    head = ring->consumer.head_cache;
    if (tail == head) {
        head                      = __atomic_load_n(&ring->producer.head, __ATOMIC_ACQUIRE);
        ring->consumer.head_cache = head;
        if (tail == head) return 0;
    }

    count = (size_t)(uint32_t)(head - tail);
    if (count > capacity) count = capacity;
    for (size_t i = 0; i < count; i++) events[i] = ring->entries[(tail + (uint32_t)i) & PS2_EVENT_QUEUE_MASK];
    __atomic_store_n(&ring->consumer.tail, tail + (uint32_t)count, __ATOMIC_RELEASE);
    return count;
}

/* Consumer-side empty check used while arming the wait queue. */
static inline bool ps2_event_ring_empty(const struct ps2_event_ring *ring)
{
    uint32_t tail = __atomic_load_n(&ring->consumer.tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&ring->producer.head, __ATOMIC_ACQUIRE);
    return tail == head;
}

#endif // INCLUDE_PS2_EVENT_RING_H_
