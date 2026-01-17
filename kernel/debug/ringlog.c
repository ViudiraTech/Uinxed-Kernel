/*
 *
 *      ringlog.c
 *      Ring log buffer
 *
 *      2025/9/21 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <printk.h>
#include <ringlog.h>
#include <string.h>

/* Initialize the ring log buffer */
static void log_buffer_init(log_buffer_t *log)
{
    memset(log, 0, sizeof(log_buffer_t));
    log->head  = 0;
    log->tail  = 0;
    log->count = 0;
}

/* Write logs to the ring log buffer */
void log_buffer_write(log_buffer_t *log, const char *fmt, ...)
{
    va_list args;

    if (!log->count && !log->head && !log->tail) log_buffer_init(log);
    memset(log->logs[log->head], 0, LOG_MAX_LENGTH);

    va_start(args, fmt);
    (void)vsprintf(log->logs[log->head], fmt, args);
    va_end(args);

    log->head = (log->head + 1) % LOG_BUFFER_SIZE;

    if (log->count == LOG_BUFFER_SIZE) {
        log->tail = (log->tail + 1) % LOG_BUFFER_SIZE;
    } else {
        log->count++;
    }
}

/* Printing ring log buffer */
void log_buffer_print(log_buffer_t *log)
{
    int current = log->tail;

    for (int i = 0; i < log->count; i++) {
        plogk("%s", log->logs[current]);
        current = (current + 1) % LOG_BUFFER_SIZE;
    }
}
