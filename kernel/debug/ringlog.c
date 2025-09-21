/*
 *
 *      ringlog.c
 *      Ring log buffer
 *
 *      2025/9/21 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "ringlog.h"
#include "printk.h"
#include "string.h"

/* Initialize the ring log buffer */
void log_buffer_init(log_buffer_t *log)
{
    log->head = log->tail = 0;
    log->full             = 0;
    log->msg_head = log->msg_tail = 0;
    log->msg_full                 = 0;
}

/* Write logs to the ring log buffer */
void log_buffer_write(log_buffer_t *log, const char *fmt, ...)
{
    char temp[LOG_LINE_SIZE];

    va_list args;
    va_start(args, fmt);
    (void)vsprintf(temp, fmt, args);
    va_end(args);

    size_t len = strlen(temp);

    if (len >= LOG_LINE_SIZE) len = LOG_LINE_SIZE - 1;
    temp[len] = '\0';

    size_t msg_offset = log->head;

    for (size_t i = 0; i < len; i++) {
        log->buffer[log->head] = temp[i];
        log->head              = (log->head + 1) % LOG_BUFFER_SIZE;

        if (log->full) {
            log->tail = (log->tail + 1) % LOG_BUFFER_SIZE;

            while (log->msg_tail != log->msg_head) {
                size_t tail_offset = log->msgs[log->msg_tail].offset;
                size_t tail_len    = log->msgs[log->msg_tail].len;
                size_t tail_end    = (tail_offset + tail_len) % LOG_BUFFER_SIZE;

                if ((log->tail >= tail_offset && log->tail < tail_end)
                    || (tail_end < tail_offset && (log->tail >= tail_offset || log->tail < tail_end))) {
                    log->msg_tail = (log->msg_tail + 1) % LOG_MAX_MSGS;
                    log->msg_full = 0;
                } else {
                    break;
                }
            }
        }
        if (log->head == log->tail) log->full = 1;
    }
    log->msgs[log->msg_head].offset = msg_offset;
    log->msgs[log->msg_head].len    = len;
    log->msg_head                   = (log->msg_head + 1) % LOG_MAX_MSGS;
    if (log->msg_head == log->msg_tail) log->msg_full = 1;
}

/* Printing ring log buffer */
void log_buffer_print(log_buffer_t *log)
{
    size_t idx = log->msg_tail;

    while (idx != log->msg_head || log->msg_full) {
        size_t offset = log->msgs[idx].offset;
        size_t len    = log->msgs[idx].len;

        char   temp[LOG_LINE_SIZE];
        size_t first_part = LOG_BUFFER_SIZE - offset;

        if (first_part >= len) {
            memcpy(temp, &log->buffer[offset], len);
        } else {
            memcpy(temp, &log->buffer[offset], first_part);
            memcpy(temp + first_part, &log->buffer[0], len - first_part);
        }
        temp[len] = '\0';

        plogk(temp);

        idx           = (idx + 1) % LOG_MAX_MSGS;
        log->msg_full = 0;
    }
}
