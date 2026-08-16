/*
 *
 *      usb_sysfs.c
 *      USB bus and device sysfs integration (/sys/bus/usb/)
 *
 *      2026/8/15 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/usb/core/usb.h>
#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/usb_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>

/* USB bus type */

struct bus_type usb_bus_type = {.name = "usb", .dev_name = NULL};

/* Device attribute show functions */

/* Read the device's vendor id. */
static ssize_t id_vendor_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%04x\n", device ? device->descriptor.vendor_id : 0);
}

/* Read the device's product id. */
static ssize_t id_product_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%04x\n", device ? device->descriptor.product_id : 0);
}

/* Read the device's product string. */
static ssize_t product_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%s\n", device ? device->product : "");
}

/* Read the device's manufacturer string. */
static ssize_t manufacturer_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%s\n", device ? device->manufacturer : "");
}

/* Read the device's serial string. */
static ssize_t serial_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%s\n", device ? device->serial : "");
}

/* Read the device's speed in Mbit/s. */
static ssize_t speed_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device        = dev ? dev->driver_data : NULL;
    const char   *speed_table[] = {"0", "1.5", "12", "480", "5000", "10000"};
    const char   *speed         = "0";
    if (device && device->speed > 0 && device->speed <= USB_SPEED_SUPER_PLUS) speed = speed_table[device->speed];
    return sysfs_emit(buffer, "%s\n", speed);
}

/* Read the device's address. */
static ssize_t devnum_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%u\n", device ? device->address : 0);
}

/* Read the device's bus number. */
static ssize_t busnum_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%u\n", device ? device->bus_number : 0);
}

/* Read the device class code. */
static ssize_t b_device_class_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%02x\n", device ? device->descriptor.device_class : 0);
}

/* Read the device subclass code. */
static ssize_t b_device_subclass_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%02x\n", device ? device->descriptor.device_subclass : 0);
}

/* Read the device protocol code. */
static ssize_t b_device_protocol_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%02x\n", device ? device->descriptor.device_protocol : 0);
}

/* Read the control endpoint's maximum packet size. */
static ssize_t b_max_packet_size0_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%u\n", device ? device->descriptor.max_packet_size0 : 0);
}

/* Read the device's USB version. */
static ssize_t version_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    uint16_t      bcd    = device ? device->descriptor.usb_version : 0;
    return sysfs_emit(buffer, "%2x.%02x\n", bcd >> 8, bcd & 0xff);
}

/* Read the number of configurations. */
static ssize_t b_num_configurations_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%u\n", device ? device->descriptor.configuration_count : 0);
}

/* Read the device's release number. */
static ssize_t bcd_device_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    uint16_t      bcd    = device ? device->descriptor.device_version : 0;
    return sysfs_emit(buffer, "%2x.%02x\n", bcd >> 8, bcd & 0xff);
}

/* Read the device's port number. */
static ssize_t devpath_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%u\n", device ? device->port_number : 0);
}

/* Read the active configuration value. */
static ssize_t b_configuration_value_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%u\n", device ? device->configuration.configuration_value : 0);
}

/* Read the configuration attributes. */
static ssize_t bm_attributes_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%02x\n", device ? device->configuration.attributes : 0);
}

/* Read the configuration's maximum power draw. */
static ssize_t b_max_power_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%umA\n", device ? (unsigned)device->configuration.max_power * 2 : 0);
}

/* Read the configuration's interface count. */
static ssize_t b_num_interfaces_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%u\n", device ? device->configuration.interface_count : 0);
}

/* Device attributes */

static DEVICE_ATTR(idVendor, 0444, id_vendor_show, NULL);
static DEVICE_ATTR(idProduct, 0444, id_product_show, NULL);
static DEVICE_ATTR(product, 0444, product_show, NULL);
static DEVICE_ATTR(manufacturer, 0444, manufacturer_show, NULL);
static DEVICE_ATTR(serial, 0444, serial_show, NULL);
static DEVICE_ATTR(speed, 0444, speed_show, NULL);
static DEVICE_ATTR(devnum, 0444, devnum_show, NULL);
static DEVICE_ATTR(busnum, 0444, busnum_show, NULL);
static DEVICE_ATTR(bDeviceClass, 0444, b_device_class_show, NULL);
static DEVICE_ATTR(bDeviceSubClass, 0444, b_device_subclass_show, NULL);
static DEVICE_ATTR(bDeviceProtocol, 0444, b_device_protocol_show, NULL);
static DEVICE_ATTR(bMaxPacketSize0, 0444, b_max_packet_size0_show, NULL);
static DEVICE_ATTR(version, 0444, version_show, NULL);
static DEVICE_ATTR(bNumConfigurations, 0444, b_num_configurations_show, NULL);
static DEVICE_ATTR(bcdDevice, 0444, bcd_device_show, NULL);
static DEVICE_ATTR(devpath, 0444, devpath_show, NULL);
static DEVICE_ATTR(bConfigurationValue, 0444, b_configuration_value_show, NULL);
static DEVICE_ATTR(bmAttributes, 0444, bm_attributes_show, NULL);
static DEVICE_ATTR(bMaxPower, 0444, b_max_power_show, NULL);
static DEVICE_ATTR(bNumInterfaces, 0444, b_num_interfaces_show, NULL);

static struct attribute *usb_device_attributes[] = {
    &dev_attr_idVendor.attr,
    &dev_attr_idProduct.attr,
    &dev_attr_product.attr,
    &dev_attr_manufacturer.attr,
    &dev_attr_serial.attr,
    &dev_attr_speed.attr,
    &dev_attr_devnum.attr,
    &dev_attr_busnum.attr,
    &dev_attr_bDeviceClass.attr,
    &dev_attr_bDeviceSubClass.attr,
    &dev_attr_bDeviceProtocol.attr,
    &dev_attr_bMaxPacketSize0.attr,
    &dev_attr_version.attr,
    &dev_attr_bNumConfigurations.attr,
    &dev_attr_bcdDevice.attr,
    &dev_attr_devpath.attr,
    &dev_attr_bConfigurationValue.attr,
    &dev_attr_bmAttributes.attr,
    &dev_attr_bMaxPower.attr,
    &dev_attr_bNumInterfaces.attr,
    NULL,
};
static const struct attribute_group usb_device_group    = {.attrs = usb_device_attributes};
const struct attribute_group       *usb_device_groups[] = {&usb_device_group, NULL};

/* Initialization */

/* Register the usb bus and mark the core ready for device registration. */
void usb_sysfs_init(void)
{
#if CONFIG_SYSFS
    int ret = bus_register(&usb_bus_type);
    if (ret != EOK) {
        plogk("usb_sysfs: Bus_register(usb) failed: %d\n", ret);
        return;
    }
    plogk("usb_sysfs: registered /sys/bus/usb\n");
    usb_core_init();
#endif
}
