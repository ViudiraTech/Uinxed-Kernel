/*
 *
 *      printk.h
 *      Kernel string print header file
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PRINTK_H_
#define INCLUDE_PRINTK_H_

#include <kernel/vsprintf.h>
#include <libs/std/stdarg.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>

/* NMI-safe logging: park a message from NMI context, drain via plogk later. */
#define NMI_LOG_MSG_SIZE 256

/* Window and budget of a rate-limited message: 10 messages per 5 seconds. */
#define PRINTK_RATELIMIT_TICKS (5ULL * CONFIG_TIMER_HZ)
#define PRINTK_RATELIMIT_BURST 10U

/* Declare the rate limit state of one message site; it starts with a full budget.  An interval of 0 means the site is never limited. */
#define DEFINE_RATELIMIT_STATE(name, interval_ticks, burst_messages) ratelimit_state_t name = {.interval = (interval_ticks), .burst = (burst_messages), .left = (burst_messages)}

/* Run one statement the first time its site runs, and never again after that. */
#define ONCE_LOG(statement)                                                   \
    do {                                                                      \
        static uint8_t _logged;                                               \
        if (!__atomic_exchange_n(&_logged, 1, __ATOMIC_RELAXED)) (statement); \
    } while (0)

/* Log a message the first time its site runs, for a failure the caller cannot report and that must not repeat. */
#define plogk_once(fmt, ...) ONCE_LOG(plogk(fmt, ##__VA_ARGS__))

/* Rate limit state of one message site: a site owns its own state, so a chatty site cannot mute the messages of the others. */
typedef struct {
        uint64_t interval; // ticks one budget covers
        uint64_t begin;    // tick the current budget started
        uint32_t burst;    // messages allowed per interval
        uint32_t left;     // messages left in the current budget
} ratelimit_state_t;

/* Kernel print string: unconditional console output, for /dev/kmsg writes and the panic path. */
__attribute__((format(printf, 1, 2))) void printk(const char *format, ...);

/* Kernel print log: the default for kernel messages, timestamped and gated by CONFIG_KERNEL_LOG; the owner of the failed operation reports it, not the helper that detected it. */
__attribute__((format(printf, 1, 2))) void plogk(const char *format, ...);

/* True when the state still allows a message.  The counters move without a lock, so the budget is approximate under contention. */
bool ratelimit_allow(ratelimit_state_t *state);

/* Park a message for the current CPU's next timer tick (NMI context) */
void nmi_log_message(const char *msg, size_t len);

/* Drain the current CPU's parked NMI message through plogk() */
void nmi_log_flush(void);

#endif // INCLUDE_PRINTK_H_
