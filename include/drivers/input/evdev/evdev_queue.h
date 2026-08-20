/*
 *
 *      evdev_queue.h
 *      Linux-compatible evdev per-client packet queue definitions
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EVDEV_QUEUE_H_
#define INCLUDE_EVDEV_QUEUE_H_

#include <drivers/input/input_event.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>

typedef struct {
        unsigned int   head;
        unsigned int   tail;
        unsigned int   packet_head;
        unsigned int   size;
        input_event_t *buffer;
} evdev_queue_t;

/* Initialize a queue over @buffer with @size event slots. */
bool evdev_queue_init(evdev_queue_t *queue, input_event_t *buffer, unsigned int size);

/* True if a complete packet (ending in SYN_REPORT) is available. */
bool evdev_queue_has_packet(const evdev_queue_t *queue);

/* Append one event; returns false if the buffer overflows. */
bool evdev_queue_push(evdev_queue_t *queue, const input_event_t *event);

/* Pop up to @max_events events, stopping at a packet boundary. */
size_t evdev_queue_read(evdev_queue_t *queue, input_event_t *events, size_t max_events);

/* Drop events of the given type from the queue. */
void evdev_queue_flush_type(evdev_queue_t *queue, unsigned int type);

/* Discard the partial packet and inject the SYN_DROPPED event. */
void evdev_queue_discard_pending(evdev_queue_t *queue, const input_event_t *syn_dropped);

#endif // INCLUDE_EVDEV_QUEUE_H_
