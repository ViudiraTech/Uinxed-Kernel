/*
 *
 *      i2c-core.c
 *      I2C bus subsystem core implementation
 *
 *      2026/7/25 By Uinxed
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/i2c/i2c.h>
#include <fs/sysfs/i2c_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/glist/circular_list.h>
#include <libs/std/string.h>
#include <proc/sched.h>
#include <sync/spin_lock.h>

#if CONFIG_I2C

static spinlock_t i2c_adapter_lock;
static clist_t    i2c_adapter_list = 0;
static int        i2c_adapter_next_nr;

int i2c_add_adapter(struct i2c_adapter *adap)
{
    if (!adap || !adap->algo || !adap->algo->master_xfer) return -EINVAL;

    spin_lock(&i2c_adapter_lock);
    if (clist_search(i2c_adapter_list, adap)) {
        spin_unlock(&i2c_adapter_lock);
        return -EEXIST;
    }

    adap->bus_lock.lock   = 0;
    adap->bus_lock.rflags = 0;
    adap->owners          = 1;

    if (adap->timeout <= 0) adap->timeout = 1000;
    if (adap->retries <= 0) adap->retries = 3;

    adap->nr = i2c_adapter_next_nr++;
    if (!adap->name[0]) snprintf(adap->name, sizeof(adap->name), "i2c-%d", adap->nr);
    i2c_adapter_list = clist_append(i2c_adapter_list, adap);
    if (!clist_search(i2c_adapter_list, adap)) {
        spin_unlock(&i2c_adapter_lock);
        return -ENOMEM;
    }
    spin_unlock(&i2c_adapter_lock);

    plogk("i2c: adapter '%s' registered as bus %d\n", adap->name, adap->nr);
#    if CONFIG_SYSFS
    (void)i2c_sysfs_adapter_add(adap);
#    endif
    return 0;
}

int i2c_del_adapter(struct i2c_adapter *adap)
{
    if (!adap) return -EINVAL;

    spin_lock(&adap->bus_lock);
    spin_lock(&i2c_adapter_lock);
    if (!clist_search(i2c_adapter_list, adap)) {
        spin_unlock(&i2c_adapter_lock);
        spin_unlock(&adap->bus_lock);
        return -ENODEV;
    }
    if (adap->owners > 1) {
        spin_unlock(&i2c_adapter_lock);
        spin_unlock(&adap->bus_lock);
        return -EBUSY;
    }
    i2c_adapter_list = clist_delete(i2c_adapter_list, adap);
    adap->owners     = 0;
    spin_unlock(&i2c_adapter_lock);
    spin_unlock(&adap->bus_lock);

#    if CONFIG_SYSFS
    i2c_sysfs_adapter_del(adap);
#    endif
    plogk("i2c: adapter '%s' (bus %d) unregistered.\n", adap->name, adap->nr);
    return 0;
}

struct i2c_adapter *i2c_get_adapter(int nr)
{
    struct i2c_adapter *result = 0;

    spin_lock(&i2c_adapter_lock);
    for (clist_t node = i2c_adapter_list; node; node = node->next) {
        struct i2c_adapter *adap = node->data;
        if (adap->nr == nr) {
            adap->owners++;
            result = adap;
            break;
        }
    }
    spin_unlock(&i2c_adapter_lock);
    return result;
}

void i2c_put_adapter(struct i2c_adapter *adap)
{
    if (!adap) return;

    spin_lock(&i2c_adapter_lock);
    if (adap->owners > 0) adap->owners--;
    spin_unlock(&i2c_adapter_lock);
}

void i2c_lock_bus(struct i2c_adapter *adap)
{
    if (adap) spin_lock(&adap->bus_lock);
}

void i2c_unlock_bus(struct i2c_adapter *adap)
{
    if (adap) spin_unlock(&adap->bus_lock);
}

static int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
    int ret, try;

    ret = 0;
    for (try = 0; try <= adap->retries; try++) {
        ret = adap->algo->master_xfer(adap, msgs, num);
        if (ret != -EAGAIN) break;
    }
    return ret;
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
    int ret;

    if (!adap || !adap->algo || !adap->algo->master_xfer) return -EINVAL;
    if (!msgs || num <= 0) return -EINVAL;

    spin_lock(&adap->bus_lock);
    if (adap->owners <= 0) {
        spin_unlock(&adap->bus_lock);
        return -ENODEV;
    }
    ret = __i2c_transfer(adap, msgs, num);
    if (ret < 0 && ret != -EAGAIN && ret != -ENXIO && ret != -EINVAL && ret != -ENODEV) {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("i2c: transfer failed on adapter %s (msg count %d): %d\n", adap->name, num, ret);
            last_log = sched_ticks();
        }
    }
    spin_unlock(&adap->bus_lock);
    return ret < 0 ? ret : num;
}

static int32_t i2c_smbus_xfer_emulated(struct i2c_adapter *adap, uint16_t addr, int read_write, uint8_t command, int protocol,
                                       union i2c_smbus_data *data)
{
    struct i2c_msg msg[2];
    int            nmsgs = 0;
    int            ret   = 0;
    uint8_t        dummy;
    uint8_t        tmp[I2C_SMBUS_BLOCK_MAX + 2];

    if (adap->owners <= 0) return -ENODEV;

    switch (protocol) {
        case I2C_SMBUS_QUICK :
            msg[0].addr  = addr;
            msg[0].flags = (read_write == I2C_SMBUS_READ) ? I2C_M_RD : 0;
            msg[0].len   = 0;
            msg[0].buf   = &dummy;
            nmsgs        = 1;
            break;

        case I2C_SMBUS_BYTE :
            msg[0].addr  = addr;
            msg[0].flags = (read_write == I2C_SMBUS_READ) ? I2C_M_RD : 0;
            msg[0].len   = 1;
            msg[0].buf   = &data->byte;
            nmsgs        = 1;
            break;

        case I2C_SMBUS_BYTE_DATA :
            if (read_write == I2C_SMBUS_READ) {
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 1;
                msg[0].buf   = &command;
                msg[1].addr  = addr;
                msg[1].flags = I2C_M_RD;
                msg[1].len   = 1;
                msg[1].buf   = &data->byte;
                nmsgs        = 2;
            } else {
                data->word   = command | ((uint16_t)data->byte << 8);
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 2;
                msg[0].buf   = (uint8_t *)&data->word;
                nmsgs        = 1;
            }
            break;

        case I2C_SMBUS_WORD_DATA :
            if (read_write == I2C_SMBUS_READ) {
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 1;
                msg[0].buf   = &command;
                msg[1].addr  = addr;
                msg[1].flags = I2C_M_RD;
                msg[1].len   = 2;
                msg[1].buf   = (uint8_t *)&data->word;
                nmsgs        = 2;
            } else {
                tmp[0]       = command;
                tmp[1]       = data->word & 0xFF;
                tmp[2]       = (data->word >> 8) & 0xFF;
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 3;
                msg[0].buf   = tmp;
                nmsgs        = 1;
            }
            break;

        case I2C_SMBUS_BLOCK_DATA :
            if (read_write == I2C_SMBUS_READ) {
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 1;
                msg[0].buf   = &command;
                msg[1].addr  = addr;
                msg[1].flags = I2C_M_RD;
                msg[1].len   = I2C_SMBUS_BLOCK_MAX + 1;
                msg[1].buf   = data->block;
                nmsgs        = 2;
                ret          = __i2c_transfer(adap, msg, 2);
                if (ret >= 0 && data->block[0] > I2C_SMBUS_BLOCK_MAX) data->block[0] = I2C_SMBUS_BLOCK_MAX;
                return ret < 0 ? ret : 0;
            } else {
                uint8_t len = data->block[0];
                if (len > I2C_SMBUS_BLOCK_MAX) return -EINVAL;
                tmp[0] = command;
                tmp[1] = len;
                memcpy(&tmp[2], &data->block[1], len);
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 2 + len;
                msg[0].buf   = tmp;
                nmsgs        = 1;
            }
            break;

        case I2C_SMBUS_I2C_BLOCK_DATA :
            if (read_write == I2C_SMBUS_READ) {
                uint8_t len = data->block[0];
                if (len > I2C_SMBUS_BLOCK_MAX) return -EINVAL;
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 1;
                msg[0].buf   = &command;
                msg[1].addr  = addr;
                msg[1].flags = I2C_M_RD;
                msg[1].len   = len;
                msg[1].buf   = &data->block[1];
                nmsgs        = 2;
            } else {
                uint8_t len = data->block[0];
                if (len > I2C_SMBUS_BLOCK_MAX) return -EINVAL;
                tmp[0] = command;
                memcpy(&tmp[1], &data->block[1], len);
                msg[0].addr  = addr;
                msg[0].flags = 0;
                msg[0].len   = 1 + len;
                msg[0].buf   = tmp;
                nmsgs        = 1;
            }
            break;

        default :
            return -EOPNOTSUPP;
    }

    ret = __i2c_transfer(adap, msg, nmsgs);
    return ret < 0 ? ret : 0;
}

int32_t i2c_smbus_read_byte(const struct i2c_client *client)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter) return -EINVAL;

    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data);
    i2c_unlock_bus(client->adapter);
    return ret < 0 ? (int32_t)ret : (int32_t)data.byte;
}

int32_t i2c_smbus_write_byte(const struct i2c_client *client, uint8_t value)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter) return -EINVAL;

    data.byte = value;
    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_WRITE, 0, I2C_SMBUS_BYTE, &data);
    i2c_unlock_bus(client->adapter);
    return ret;
}

int32_t i2c_smbus_read_byte_data(const struct i2c_client *client, uint8_t command)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter) return -EINVAL;

    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_READ, command, I2C_SMBUS_BYTE_DATA, &data);
    i2c_unlock_bus(client->adapter);
    return ret < 0 ? (int32_t)ret : (int32_t)data.byte;
}

int32_t i2c_smbus_write_byte_data(const struct i2c_client *client, uint8_t command, uint8_t value)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter) return -EINVAL;

    data.byte = value;
    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA, &data);
    i2c_unlock_bus(client->adapter);
    return ret;
}

int32_t i2c_smbus_read_word_data(const struct i2c_client *client, uint8_t command)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter) return -EINVAL;

    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_READ, command, I2C_SMBUS_WORD_DATA, &data);
    i2c_unlock_bus(client->adapter);
    return ret < 0 ? (int32_t)ret : (int32_t)data.word;
}

int32_t i2c_smbus_write_word_data(const struct i2c_client *client, uint8_t command, uint16_t value)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter) return -EINVAL;

    data.word = value;
    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_WRITE, command, I2C_SMBUS_WORD_DATA, &data);
    i2c_unlock_bus(client->adapter);
    return ret;
}

int32_t i2c_smbus_read_block_data(const struct i2c_client *client, uint8_t command, uint8_t *values)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter || !values) return -EINVAL;

    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_READ, command, I2C_SMBUS_BLOCK_DATA, &data);
    i2c_unlock_bus(client->adapter);
    if (ret < 0) return ret;

    memcpy(values, &data.block[1], data.block[0]);
    return data.block[0];
}

int32_t i2c_smbus_write_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, const uint8_t *values)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter || !values) return -EINVAL;
    if (length > I2C_SMBUS_BLOCK_MAX) return -EINVAL;

    data.block[0] = length;
    memcpy(&data.block[1], values, length);
    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_WRITE, command, I2C_SMBUS_BLOCK_DATA, &data);
    i2c_unlock_bus(client->adapter);
    return ret;
}

int32_t i2c_smbus_read_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, uint8_t *values)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter || !values) return -EINVAL;
    if (length > I2C_SMBUS_BLOCK_MAX) return -EINVAL;

    data.block[0] = length;
    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_READ, command, I2C_SMBUS_I2C_BLOCK_DATA, &data);
    i2c_unlock_bus(client->adapter);
    if (ret < 0) return ret;

    memcpy(values, &data.block[1], length);
    return length;
}

int32_t i2c_smbus_write_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, const uint8_t *values)
{
    union i2c_smbus_data data;
    int                  ret;

    if (!client || !client->adapter || !values) return -EINVAL;
    if (length > I2C_SMBUS_BLOCK_MAX) return -EINVAL;

    data.block[0] = length;
    memcpy(&data.block[1], values, length);
    i2c_lock_bus(client->adapter);
    ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, I2C_SMBUS_WRITE, command, I2C_SMBUS_I2C_BLOCK_DATA, &data);
    i2c_unlock_bus(client->adapter);
    return ret;
}

#endif
