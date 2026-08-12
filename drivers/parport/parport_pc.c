/*
 *
 *      parport_pc.c
 *      PC-style parallel port hardware driver (SPP)
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/parport/parport.h>
#include <kernel/debug/ringlog.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>

log_buffer_t parallel_log;

static uint8_t parport_pc_read_data(parport_t *p)
{
    return inb(p->base + PARPORT_DATA_REG);
}

static void parport_pc_write_data(parport_t *p, uint8_t v)
{
    outb(p->base + PARPORT_DATA_REG, v);
}

static uint8_t parport_pc_read_status(parport_t *p)
{
    /* Busy is active-low in hardware; Linux reports it active-high. */
    return inb(p->base + PARPORT_STATUS_REG) ^ PARPORT_BUSY;
}

static uint8_t parport_pc_read_control(parport_t *p)
{
    return inb(p->base + PARPORT_CONTROL_REG);
}

static void parport_pc_write_control(parport_t *p, uint8_t v)
{
    outb(p->base + PARPORT_CONTROL_REG, v);
}

static void parport_pc_frob_control(parport_t *p, uint8_t mask, uint8_t v)
{
    outb(p->base + PARPORT_CONTROL_REG, (inb(p->base + PARPORT_CONTROL_REG) & ~mask) | (v & mask));
}

/* Probe a legacy base port for a readable/writable control register. */
static int parport_pc_detect(uint16_t base)
{
    uint8_t orig = inb(base + PARPORT_CONTROL_REG);
    outb(base + PARPORT_CONTROL_REG, orig ^ 0x0c);
    msleep(10);
    return inb(base + PARPORT_CONTROL_REG) == (orig ^ 0x0c);
}

/* Probe the legacy parallel-port bases and register any that respond. */
int parport_pc_init(void)
{
    static const uint16_t legacy_bases[] = {0x378, 0x278, 0x3bc};
    int                   registered     = 0;

    for (size_t i = 0; i < sizeof(legacy_bases) / sizeof(legacy_bases[0]); i++) {
        if (!parport_pc_detect(legacy_bases[i])) continue;
        if (parport_register_port("parport", legacy_bases[i], -1, NULL) != 0) continue;
        log_buffer_write(&parallel_log, "parallel: Port %s detected.\n", legacy_bases[i] == 0x378 ? "LPT1" : legacy_bases[i] == 0x278 ? "LPT2" : "LPT3");
        registered++;
    }
    if (registered) log_buffer_write(&parallel_log, "parallel: %d port(s) available.\n", registered);
    return registered;
}
