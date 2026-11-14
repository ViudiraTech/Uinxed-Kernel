/*
 *
 *      deferlog.c
 *      Deferred log buffer
 *
 *      2025/9/21 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/debug/deferlog.h>
#include <kernel/debug/printk.h>
#include <libs/std/string.h>

/* Initialize the deferred log buffer */
static void deferlog_init(deferlog_t *log)
{
    memset(log, 0, sizeof(deferlog_t));
    log->head  = 0;
    log->tail  = 0;
    log->count = 0;
}

/* Write logs to the deferred log buffer */
__attribute__((format(printf, 2, 3))) void deferlog_write(deferlog_t *log, const char *fmt, ...)
{
    va_list args;

    if (!log->count && !log->head && !log->tail) deferlog_init(log);
    memset(log->logs[log->head], 0, CONFIG_DEFERLOG_MAX_LENGTH);

    va_start(args, fmt);
    (void)vsnprintf(log->logs[log->head], CONFIG_DEFERLOG_MAX_LENGTH, fmt, args);
    va_end(args);

    log->head = (log->head + 1) % CONFIG_DEFERLOG_ENTRY_COUNT;

    if (log->count == CONFIG_DEFERLOG_ENTRY_COUNT) {
        log->tail = (log->tail + 1) % CONFIG_DEFERLOG_ENTRY_COUNT;
    } else {
        log->count++;
    }
}

/* Printing deferred log buffer */
void deferlog_print(deferlog_t *log)
{
    int current = log->tail;

    for (int i = 0; i < log->count; i++) {
        plogk("%s", log->logs[current]);
        current = (current + 1) % CONFIG_DEFERLOG_ENTRY_COUNT;
    }
}
