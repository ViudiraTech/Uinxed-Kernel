/*
 *
 *      dmi_sysfs.c
 *      DMI / SMBIOS firmware data in sysfs
 *
 *      2026/8/6 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/smbios.h>
#include <drivers/base/device.h>
#include <fs/sysfs/dmi_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/hhdm.h>

static bool dmi_sysfs_ready;

/* /sys/class/dmi/id attribute files */

static ssize_t str_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t product_uuid_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t modalias_show(struct device *dev, struct device_attribute *attr, char *buf);

static DEVICE_ATTR(bios_vendor, 0444, str_show, NULL);
static DEVICE_ATTR(bios_version, 0444, str_show, NULL);
static DEVICE_ATTR(bios_date, 0444, str_show, NULL);
static DEVICE_ATTR(sys_vendor, 0444, str_show, NULL);
static DEVICE_ATTR(product_name, 0444, str_show, NULL);
static DEVICE_ATTR(product_version, 0444, str_show, NULL);
static DEVICE_ATTR(product_serial, 0444, str_show, NULL);
static DEVICE_ATTR(board_serial, 0444, str_show, NULL);
static DEVICE_ATTR(product_uuid, 0444, product_uuid_show, NULL);
static DEVICE_ATTR(modalias, 0444, modalias_show, NULL);

static ssize_t str_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    const char *(*getter)(void) = NULL;
    (void)dev;
    if (attr == &dev_attr_bios_vendor)
        getter = smbios_bios_vendor;
    else if (attr == &dev_attr_bios_version)
        getter = smbios_bios_version;
    else if (attr == &dev_attr_bios_date)
        getter = smbios_bios_release_date;
    else if (attr == &dev_attr_sys_vendor)
        getter = smbios_sys_manufacturer;
    else if (attr == &dev_attr_product_name)
        getter = smbios_sys_product_name;
    else if (attr == &dev_attr_product_version)
        getter = smbios_sys_version;
    else if (attr == &dev_attr_product_serial || attr == &dev_attr_board_serial)
        getter = smbios_sys_serial_number;
    if (!getter) return -EIO;
    return sysfs_emit(buf, "%s\n", getter() ? getter() : "");
}

static ssize_t product_uuid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    uint8_t uuid[16];
    (void)dev;
    (void)attr;
    smbios_sys_uuid(uuid);
    return sysfs_emit(buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10],
                      uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "dmi:bvn%s:bvr%s:bd%s:svn%s:pn%s:pvr%s\n", smbios_bios_vendor(), smbios_bios_version(), smbios_bios_release_date(), smbios_sys_manufacturer(), smbios_sys_product_name(),
                      smbios_sys_version());
}

static struct attribute *dmi_attributes[] = {
    &dev_attr_bios_vendor.attr,
    &dev_attr_bios_version.attr,
    &dev_attr_bios_date.attr,
    &dev_attr_sys_vendor.attr,
    &dev_attr_product_name.attr,
    &dev_attr_product_version.attr,
    &dev_attr_product_serial.attr,
    &dev_attr_board_serial.attr,
    &dev_attr_product_uuid.attr,
    &dev_attr_modalias.attr,
    NULL,
};

static struct attribute_group dmi_group = {
    .attrs = dmi_attributes,
};

static const struct attribute_group *dmi_groups[] = {
    &dmi_group,
    NULL,
};

static struct class dmi_class = {.name = "dmi", .dev_groups = dmi_groups};

/* /sys/firmware/dmi/tables/DMI binary dump */

static const uint8_t *dmi_table_base(size_t *capacity)
{
    void *entry = smbios_entry();
    if (!entry || !capacity) return NULL;
    if (*(uint8_t *)entry == 0x5F && ((uint8_t *)entry)[1] == 0x53 && ((uint8_t *)entry)[2] == 0x4D && ((uint8_t *)entry)[3] == 0x33) {
        entry_point_64_t *ep = entry;
        *capacity            = 0x100000; // entry points do not carry the length
        return (const uint8_t *)phys_to_virt(ep->structure_table_address);
    }
    entry_point_32_t *ep = entry;
    *capacity            = ep->structure_table_length;
    return (const uint8_t *)phys_to_virt(ep->structure_table_address);
}

/* Walk the self-terminating structure table (end marker type 127). */
static size_t dmi_table_walk(const uint8_t *table, size_t capacity)
{
    size_t offset = 0;
    while (offset + 4 <= capacity) {
        uint8_t type = table[offset];
        uint8_t len  = table[offset + 1];
        if (len < 4) break;
        size_t next = offset + len;
        while (next + 1 < capacity && !(table[next] == 0 && table[next + 1] == 0)) next++;
        next += 2;
        if (next > capacity) break;
        if (type == 127) return next;
        offset = next;
    }
    return offset;
}

static ssize_t dmi_tables_read(struct kobject *kobj, struct bin_attribute *attr, char *buffer, int64_t pos, size_t count)
{
    const uint8_t *table;
    size_t         capacity;
    size_t         length;
    (void)kobj;
    (void)attr;
    table = dmi_table_base(&capacity);
    if (!table || !capacity) return -ENOENT;
    length = dmi_table_walk(table, capacity);
    if (pos < 0 || (uint64_t)pos >= length) return 0;
    if (count > length - (size_t)pos) count = length - (size_t)pos;
    memcpy(buffer, table + pos, count);
    return (ssize_t)count;
}

static struct bin_attribute dmi_tables_attr = {
    .attr = __ATTR(DMI, 0400),
    .read = dmi_tables_read,
};

/* Registration */

/* Register the DMI class and firmware tables in sysfs. */
void dmi_sysfs_init(void)
{
#if CONFIG_SYSFS
    struct kobject *firmware_kobj;
    struct kobject *tables_kobj;

    if (dmi_sysfs_ready) return;
    if (!smbios_entry()) return;

    if (class_register(&dmi_class) == EOK) (void)device_create(&dmi_class, NULL, 0, NULL, "id");

    firmware_kobj = NULL;
    if (sysfs_root_kobj) {
        for (clist_t node = sysfs_root_kobj->children; node; node = node->next) {
            struct kobject *child = node->data;
            if (child && child->name && streq(child->name, "firmware")) {
                firmware_kobj = child;
                break;
            }
        }
    }
    if (firmware_kobj) {
        tables_kobj = kobject_create_and_add("tables", firmware_kobj);
        if (tables_kobj) {
            if (sysfs_create_bin_file(tables_kobj, &dmi_tables_attr) != EOK) {
                kobject_del(tables_kobj);
                kobject_put(tables_kobj);
            }
        }
    }

    dmi_sysfs_ready = true;
    plogk("dmi_sysfs: /sys/class/dmi/id and /sys/firmware/dmi/tables registered.\n");
#endif
}
