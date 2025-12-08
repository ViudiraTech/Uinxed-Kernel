/*
 *
 *      ps2.c
 *      PS/2 controller driver
 *
 *      2025/9/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <printk.h>
#include <ps2.h>

/* Waiting for PS/2 read ready */
int wait_ps2_read(void)
{
    for (size_t i = 0; i < 10000; ++i) {
        if (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) return 0;
    }
    return 1;
}

/* Waiting for PS/2 write ready */
int wait_ps2_write(void)
{
    for (size_t i = 0; i < 10000; ++i) {
        if (!(ps2_read_status() & PS2_STATUS_INPUT_FULL)) return 0;
    }
    return 1;
}

/* Read PS/2 data */
uint8_t ps2_read_data(void)
{
    wait_ps2_read();
    return inb(PS2_DATA_PORT);
}

/* Read PS/2 status */
uint8_t ps2_read_status(void)
{
    return inb(PS2_STATUS_PORT);
}

/* Read PS/2 configuration */
uint8_t ps2_read_config(void)
{
    ps2_write_cmd(PS2_CMD_READ_CONFIG);
    return ps2_read_data();
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
    outb(PS2_STATUS_PORT, cmd);
}

/* Sending configuration to PS/2 */
void ps2_write_config(uint8_t config)
{
    ps2_write_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);
}

/* Initialize the PS/2 controller */
void init_ps2(void)
{
    /* Disable all ports */
    ps2_write_cmd(PS2_CMD_DISABLE_PORT1);
    ps2_write_cmd(PS2_CMD_DISABLE_PORT2);

    /* Flush the output buffer */
    for (size_t i = 0; i < 10000; ++i) {
        if (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) ps2_read_data();
    }

    /* Disable all IRQs and clocks */
    uint8_t config = ps2_read_config();
    config &= ~PS2_CONFIG_PORT1_IRQ;
    config &= ~PS2_CONFIG_PORT2_IRQ;
    config &= ~PS2_CONFIG_PORT1_CLOCK;
    config &= ~PS2_CONFIG_PORT2_CLOCK;
    ps2_write_config(config);

    /* Controller self-test */
    ps2_write_cmd(PS2_CMD_SELF_TEST);
    if (ps2_read_data() != PS2_RESPONSE_SELFTEST) {
        plogk("ps/2: Controller self-test failed.\n");
    } else {
        plogk("ps/2: Controller self-test successful.\n");
    }

    /* Detect whether it is a dual-channel controller */
    ps2_write_cmd(PS2_CMD_ENABLE_PORT2);
    int is_dual_channel = (ps2_read_config() & PS2_CONFIG_PORT2_CLOCK) == 0;

    if (!is_dual_channel) {
        ps2_write_cmd(PS2_CMD_DISABLE_PORT2);
        plogk("ps/2: Single channel controller detected.\n");
    } else {
        plogk("ps/2: Dual channel controller detected.\n");
    }

    /* Test port */
    ps2_write_cmd(PS2_CMD_TEST_PORT1);
    if (ps2_read_data() != PS2_RESPONSE_TEST) plogk("ps/2: Port 1 test failed.\n");
    if (is_dual_channel) {
        ps2_write_cmd(PS2_CMD_TEST_PORT2);
        if (ps2_read_data() != PS2_RESPONSE_TEST) plogk("ps/2: Port 2 test failed.\n");
    }

    /* Enable all ports, IRQs, and clocks */
    ps2_write_cmd(PS2_CMD_ENABLE_PORT1);
    if (is_dual_channel) ps2_write_cmd(PS2_CMD_ENABLE_PORT2);

    uint8_t final_config = ps2_read_config();
    final_config |= PS2_CONFIG_PORT1_IRQ;
    final_config |= PS2_CONFIG_PORT2_IRQ;
    final_config |= PS2_CONFIG_PORT1_CLOCK;
    final_config |= PS2_CONFIG_PORT2_CLOCK;
    ps2_write_config(final_config);
}
