/*
 *
 *      ringlog.h
 *      Ring log buffer header file
 *
 *      2025/9/21 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "stddef.h"

#define LOG_BUFFER_SIZE 4096 // Each log buffer size
#define LOG_LINE_SIZE   512  // Maximum length of a single log
#define LOG_MAX_MSGS    128  // Store up to 128 messages

typedef struct {
        char   buffer[LOG_BUFFER_SIZE];
        size_t head;
        size_t tail;
        int    full;

        struct {
                size_t offset;
                size_t len;
        } msgs[LOG_MAX_MSGS];

        size_t msg_head;
        size_t msg_tail;
        int    msg_full;
} log_buffer_t;

/* Initialize the ring log buffer */
void log_buffer_init(log_buffer_t *log);

/* Write logs to the ring log buffer */
void log_buffer_write(log_buffer_t *log, const char *fmt, ...);

/* Printing ring log buffer */
void log_buffer_print(log_buffer_t *log);
