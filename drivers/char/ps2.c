/*
 *
 *      ps2.c
 *      PS/2 controller driver
 *
 *      2025/9/7 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "ps2.h"
#include "common.h"

/* Waiting for PS/2 read idle */
void wait_ps2_read(void)
{
    for (size_t i = 0; i < 1000000; ++i) {
        if (!(inb(PS2_CMD_PORT) & 0x01)) return;
    }
}

/* Waiting for PS/2 write idle */
void wait_ps2_write(void)
{
    for (size_t i = 0; i < 1000000; ++i) {
        if (!(inb(PS2_CMD_PORT) & 0x02)) return;
    }
}

/* Read PS/2 data */
uint8_t ps2_read_data(void)
{
    wait_ps2_read();
    return inb(PS2_DATA_PORT);
}

/* Read PS/2 command */
uint8_t ps2_read_cmd(void)
{
    wait_ps2_read();
    return inb(PS2_CMD_PORT);
}

/* Sending data to PS/2 */
void ps2_write_data(uint8_t data)
{
    wait_ps2_write();
    outb(PS2_DATA_PORT, data);
}

/* Sending commands to PS/2 */
void ps2_write_cmd(uint8_t cmd)
{
    wait_ps2_write();
    outb(PS2_CMD_PORT, cmd);
}
