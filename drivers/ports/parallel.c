/*
 *
 *      parallel.c
 *      Parallel port
 *
 *      2024/9/8 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <parallel.h>
#include <printk.h>
#include <timer.h>

/* Check if the specified parallel port exists */
static int parallel_detect(uint16_t port)
{
    uint8_t orig = inb(port + CONTROL_REG);
    outb(port + CONTROL_REG, orig ^ 0xc);
    msleep(10);
    return inb(port + CONTROL_REG) == (orig ^ 0xc);
}

/* Initialize the specified parallel port */
static void init_parallel_port(uint16_t port)
{
    uint8_t control = 0;
    control |= 0x04; // Initialize peripherals
    control |= 0x08; // Select peripherals
    outb(port + CONTROL_REG, control);

    if (!(inb(port + STATUS_REG) & 0x40)) {
        plogk("parallel: Parallel port %s test failed.\n", PORT_TO_LPT(port));
        return;
    }
    plogk("parallel: Local port: %s, Status: 0x%02x\n", PORT_TO_LPT(port), inb(port + STATUS_REG));
}

/* Initialize parallel port */
void init_parallel(void)
{
    uint16_t lpt_port[3] = {PARALLEL_PORT_1, PARALLEL_PORT_2, PARALLEL_PORT_3};
    int      valid_ports = 0;

    for (int i = 0; i < 3; i++) {
        if (parallel_detect(lpt_port[i])) {
            init_parallel_port(lpt_port[i]);
            valid_ports++;
        }
    }
    if (valid_ports == 0) plogk("parallel: No parallel port available.\n");
}

/* Check if the specified parallel port is busy */
int parallel_port_busy(uint16_t port)
{
    return !(inb(port + STATUS_REG) & 0x80);
}

/* Write parallel port */
void write_parallel(uint16_t port, uint8_t data)
{
    while (parallel_port_busy(port));

    outb(port + DATA_REG, data);
    outb(port + CONTROL_REG, inb(port + CONTROL_REG) & ~0x01);
    msleep(10);
    outb(port + CONTROL_REG, inb(port + CONTROL_REG) | 0x01);
}

/* Get the status value of the specified parallel port */
uint8_t get_parallel_status(uint16_t port)
{
    return inb(port + STATUS_REG);
}
