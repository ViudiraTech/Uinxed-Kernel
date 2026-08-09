/*
 *
 *      i2c_sysfs.h
 *      I2C bus and i2c-dev sysfs integration header
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_I2C_SYSFS_H_
#define INCLUDE_I2C_SYSFS_H_

struct i2c_adapter;

/* Register the "i2c" bus type and the "i2c-dev" class. */
void i2c_sysfs_init(void);

/*
 * Publish an adapter as /sys/bus/i2c/devices/i2c-N, /sys/class/i2c-dev/i2c-N
 * and /dev/i2c-N.  Called by i2c_add_adapter().
 */
int i2c_sysfs_adapter_add(struct i2c_adapter *adap);

/* Undo i2c_sysfs_adapter_add().  Called by i2c_del_adapter(). */
void i2c_sysfs_adapter_del(struct i2c_adapter *adap);

#endif // INCLUDE_I2C_SYSFS_H_
