/*
 *
 *      input_event.h
 *      Generic input event definitions
 *
 *      2026/6/12 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_INPUT_EVENT_H_
#define INCLUDE_INPUT_EVENT_H_

#include <stdint.h>

/* Event type values returned in `input_event_t.type`. */
enum {
    input_event_type_raw = 1,
};

/*
 * Generic raw input event exposed through /dev/input/eventX.
 *
 * `type` identifies the event payload format.
 * `code` and `raw` carry the original device-specific input byte for raw events.
 * `value` is 1 for key press bytes and 0 for key release bytes in the PS/2 path.
 * Events are returned as a packed array when reading /dev/input/eventX.
 */
typedef struct {
        uint64_t timestamp_ns;
        uint16_t type;
        uint16_t code;
        int32_t  value;
        uint8_t  raw;
        uint8_t  reserved[3];
} input_event_t;

#endif // INCLUDE_INPUT_EVENT_H_
