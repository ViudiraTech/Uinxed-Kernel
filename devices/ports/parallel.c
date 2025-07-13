/*
 *
 *      parallel.c
 *      Parallel Port
 *
 *      2024/9/8 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "parallel.h"
#include "common.h"
#include "timer.h"

/* Waiting for the parallel port to become ready */
void wait_parallel_ready(void)
{
    while ((!inb(LPT1_PORT_STATUS)) & 0x80) msleep(10);
    return;
}

/* Write to parallel port */
void parallel_write(const char c)
{
    wait_parallel_ready();
    outb(LPT1_PORT_BASE, c);

    char lpt1_control = (char)inb(LPT1_PORT_CONTROL);
    outb(LPT1_PORT_CONTROL, lpt1_control | 1);
    msleep(10);
    outb(LPT1_PORT_CONTROL, lpt1_control);

    wait_parallel_ready();
    return;
}
