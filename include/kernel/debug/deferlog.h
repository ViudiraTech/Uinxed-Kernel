/*
 *
 *      deferlog.h
 *      Deferred log buffer header file
 *
 *      2025/9/21 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DEFERLOG_H_
#define INCLUDE_DEFERLOG_H_

typedef struct {
        char logs[CONFIG_DEFERLOG_ENTRY_COUNT][CONFIG_DEFERLOG_MAX_LENGTH];
        int  head;
        int  tail;
        int  count;
} deferlog_t;

/* Write logs to the deferred log buffer */
__attribute__((format(printf, 2, 3))) void deferlog_write(deferlog_t *log, const char *fmt, ...);

/* Printing deferred log buffer */
void deferlog_print(deferlog_t *log);

#endif // INCLUDE_DEFERLOG_H_
