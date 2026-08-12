/*
 *
 *      i2c.h
 *      I2C bus subsystem header file
 *
 *      2026/7/25 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_I2C_H_
#define INCLUDE_I2C_H_

#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

#define I2C_M_RD       0x0001
#define I2C_M_TEN      0x0010
#define I2C_M_RECV_LEN 0x0400
#define I2C_M_NOSTART  0x4000

#define I2C_FUNC_I2C               0x00000001
#define I2C_FUNC_10BIT_ADDR        0x00000002
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004
#define I2C_FUNC_SMBUS_BYTE        0x00020000
#define I2C_FUNC_SMBUS_BYTE_DATA   0x00040000
#define I2C_FUNC_SMBUS_WORD_DATA   0x00080000
#define I2C_FUNC_SMBUS_BLOCK_DATA  0x01000000
#define I2C_FUNC_SMBUS_I2C_BLOCK   0x04000000
#define I2C_FUNC_SMBUS_EMUL \
    (I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_I2C_BLOCK)

#define I2C_SMBUS_QUICK          0
#define I2C_SMBUS_BYTE           1
#define I2C_SMBUS_BYTE_DATA      2
#define I2C_SMBUS_WORD_DATA      3
#define I2C_SMBUS_BLOCK_DATA     5
#define I2C_SMBUS_I2C_BLOCK_DATA 8

#define I2C_SMBUS_READ  1
#define I2C_SMBUS_WRITE 0

#define I2C_SMBUS_BLOCK_MAX 32

struct i2c_adapter;
struct i2c_client;

struct i2c_msg {
        uint16_t addr;
        uint16_t flags;
        uint16_t len;
        uint8_t *buf;
};

struct i2c_algorithm {
        int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
        uint32_t (*functionality)(struct i2c_adapter *adap);
};

struct i2c_adapter {
        const struct i2c_algorithm *algo;
        void                       *algo_data;
        int                         nr;
        char                        name[48];
        int                         timeout;
        int                         retries;
        spinlock_t                  bus_lock;
        int                         owners;
};

struct i2c_client {
        uint16_t            addr;
        struct i2c_adapter *adapter;
        char                name[20];
};

union i2c_smbus_data {
        uint8_t  byte;
        uint16_t word;
        uint8_t  block[I2C_SMBUS_BLOCK_MAX + 2];
};

/* Register an I2C adapter with the subsystem */
int i2c_add_adapter(struct i2c_adapter *adap);

/* Unregister an I2C adapter */
int i2c_del_adapter(struct i2c_adapter *adap);

/* Execute a sequence of I2C messages on an adapter */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);

/* Get an adapter by number, taking a reference */
struct i2c_adapter *i2c_get_adapter(int nr);

/* Release a reference taken by i2c_get_adapter */
void i2c_put_adapter(struct i2c_adapter *adap);

/* Lock/unlock the adapter bus */
void i2c_lock_bus(struct i2c_adapter *adap);
void i2c_unlock_bus(struct i2c_adapter *adap);

/* SMBus byte commands */
int32_t i2c_smbus_read_byte(const struct i2c_client *client);
int32_t i2c_smbus_write_byte(const struct i2c_client *client, uint8_t value);

/* SMBus byte-data commands (byte at a command register) */
int32_t i2c_smbus_read_byte_data(const struct i2c_client *client, uint8_t command);
int32_t i2c_smbus_write_byte_data(const struct i2c_client *client, uint8_t command, uint8_t value);

/* SMBus word-data commands */
int32_t i2c_smbus_read_word_data(const struct i2c_client *client, uint8_t command);
int32_t i2c_smbus_write_word_data(const struct i2c_client *client, uint8_t command, uint16_t value);

/* SMBus block-data commands (device-supplied length) */
int32_t i2c_smbus_read_block_data(const struct i2c_client *client, uint8_t command, uint8_t *values);
int32_t i2c_smbus_write_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, const uint8_t *values);

/* SMBus I2C-block commands (caller-supplied length) */
int32_t i2c_smbus_read_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, uint8_t *values);
int32_t i2c_smbus_write_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, const uint8_t *values);

#endif // INCLUDE_I2C_H_
