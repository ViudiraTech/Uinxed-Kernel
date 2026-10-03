/*
 *
 *      ringlog.h
 *      Ring log buffer header file
 *
 *      2025/9/21 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RINGLOG_H_
#define INCLUDE_RINGLOG_H_

typedef struct {
        char logs[CONFIG_LOG_BUFFER_SIZE][CONFIG_LOG_MAX_LENGTH];
        int  head;
        int  tail;
        int  count;
} log_buffer_t;

/* Write logs to the ring log buffer */
__attribute__((format(printf, 2, 3))) void log_buffer_write(log_buffer_t *log, const char *fmt, ...);

/* Printing ring log buffer */
void log_buffer_print(log_buffer_t *log);

#endif // INCLUDE_RINGLOG_H_
