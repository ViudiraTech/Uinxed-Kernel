/*
 *
 *      ringlog.h
 *      Ring log buffer header file
 *
 *      2025/9/21 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RINGLOG_H_
#define INCLUDE_RINGLOG_H_

#include <stddef.h>

#define LOG_MAX_LENGTH  1024
#define LOG_BUFFER_SIZE 32

typedef struct {
        char logs[LOG_BUFFER_SIZE][LOG_MAX_LENGTH];
        int  head;
        int  tail;
        int  count;
} log_buffer_t;

/* Write logs to the ring log buffer */
void log_buffer_write(log_buffer_t *log, const char *fmt, ...);

/* Printing ring log buffer */
void log_buffer_print(log_buffer_t *log);

#endif // INCLUDE_RINGLOG_H_
