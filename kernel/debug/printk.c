/*
 *
 *      printk.c
 *      Kernel string printing
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/smp.h>
#include <drivers/firmware/acpi.h>
#include <drivers/tty/tty.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <process/sched.h>
#include <sync/spin_lock.h>

/* Deepest nesting the log path allows before it drops a message. */
#define PRINTK_MAX_RECURSION 3

/* Per-CPU NMI message slot, drained by nmi_log_flush() on the next timer tick. */
struct nmi_log_slot {
        char msg[NMI_LOG_MSG_SIZE];
        bool busy;
};

/* Lock for printk */
static spinlock_t printk_lock = {
    .lock   = 0,
    .rflags = 0,
};

static struct nmi_log_slot nmi_log_slots[CONFIG_NMI_LOG_MAX_CPUS];
static uint64_t            nmi_log_lost[CONFIG_NMI_LOG_MAX_CPUS]; // dropped per CPU
static uint8_t             printk_depth[CONFIG_NMI_LOG_MAX_CPUS]; // log nesting, per CPU

/* Per-CPU log recursion depth: the first entry takes printk_lock, a nested one writes unlocked, and an entry past PRINTK_MAX_RECURSION is dropped.  False means drop. */
static bool printk_enter(uint32_t *cpu_out, uint64_t *rflags)
{
    uint32_t cpu = get_current_cpu_id();
    if (cpu >= CONFIG_NMI_LOG_MAX_CPUS) return false;
    *cpu_out = cpu;
    *rflags  = 0;

    if (printk_depth[cpu] >= PRINTK_MAX_RECURSION) return false;
    if (printk_depth[cpu] == 0) *rflags = spin_lock_irqsave(&printk_lock);
    printk_depth[cpu]++;
    return true;
}

/* Release the log lock when the outermost entry on this CPU leaves. */
static void printk_exit(uint32_t cpu, uint64_t rflags)
{
    printk_depth[cpu]--;
    if (printk_depth[cpu] == 0) spin_unlock_irqrestore(&printk_lock, rflags);
}

/* Kernel print string */
__attribute__((format(printf, 1, 2))) void printk(const char *format, ...)
{
    uint32_t cpu;
    uint64_t rflags;
    if (!printk_enter(&cpu, &rflags)) return;

    va_list args;
    va_start(args, format);
    vwprintf(&tty_writer, format, args);
    va_end(args);

    printk_exit(cpu, rflags);
}

/* Kernel print log */
__attribute__((format(printf, 1, 2))) void plogk(const char *format, ...)
{
#if CONFIG_KERNEL_LOG
    uint32_t cpu;
    uint64_t rflags;
    if (!printk_enter(&cpu, &rflags)) return;

    /* Prefix and body are one record and must not interleave across CPUs. */
    uint64_t now = nano_time();
    char     prefix[48];
    (void)snprintf(prefix, sizeof(prefix), "[%5llu.%06llu] ", (now / 1000000000), ((now / 1000) % 1000000));
    tty_print_str(prefix);
    va_list args;
    va_start(args, format);
    vwprintf(&tty_writer, format, args);
    va_end(args);

    printk_exit(cpu, rflags);
#else
    (void)format;
#endif
}

/* True when the state still allows a message.  One caller advances the window and refills the budget; the rest only spend what is left, so no lock is needed on the interrupt path. */
bool ratelimit_allow(ratelimit_state_t *state)
{
    uint64_t now   = sched_ticks();
    uint64_t begin = __atomic_load_n(&state->begin, __ATOMIC_RELAXED);

    if (now - begin >= state->interval && __atomic_compare_exchange_n(&state->begin, &begin, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_store_n(&state->left, state->burst, __ATOMIC_RELAXED);
    }

    uint32_t left = __atomic_load_n(&state->left, __ATOMIC_RELAXED);
    while (left > 0) {
        if (__atomic_compare_exchange_n(&state->left, &left, left - 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return true;
    }
    return false;
}

/* Park a message for the current CPU's next timer tick (NMI context). */
void nmi_log_message(const char *msg, size_t len)
{
    uint32_t cpu = get_current_cpu_id();
    if (cpu >= CONFIG_NMI_LOG_MAX_CPUS) return;
    struct nmi_log_slot *s = &nmi_log_slots[cpu];
    if (s->busy) {
        __atomic_add_fetch(&nmi_log_lost[cpu], 1, __ATOMIC_RELAXED);
        return; // keep the first of two pending messages
    }
    if (len >= NMI_LOG_MSG_SIZE) len = NMI_LOG_MSG_SIZE - 1;
    memcpy(s->msg, msg, len);
    s->msg[len] = '\0';
    s->busy     = true;
}

/* Drain the current CPU's pending NMI message through plogk(). */
void nmi_log_flush(void)
{
    uint32_t cpu = get_current_cpu_id();
    if (cpu >= CONFIG_NMI_LOG_MAX_CPUS) return;
    struct nmi_log_slot *s = &nmi_log_slots[cpu];

    /* Report dropped messages (first-wins policy) before the pending one. */
    uint64_t lost = __atomic_exchange_n(&nmi_log_lost[cpu], 0, __ATOMIC_RELAXED);
    if (lost) plogk("nmi_log: %llu message%s lost due to overflow.\n", lost, lost == 1 ? "" : "s");

    if (!s->busy) return;
    char local[NMI_LOG_MSG_SIZE];
    memcpy(local, s->msg, NMI_LOG_MSG_SIZE);
    s->busy = false; // a new NMI may now overwrite the slot
    plogk("%s", local);
}
