/*
 *
 *      tpm_sysfs.c
 *      TPM sysfs class integration (/sys/class/tpm, /sys/class/tpmrm)
 *
 *      2026/8/6 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/char/tpm/tpm.h>
#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/tpm_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>

/* Return the TPM device bound to a device-model device. */
static tpm_device_t *tpm_dev_from(struct device *dev)
{
    return dev ? dev->driver_data : NULL;
}

/* Show the TPM specification major version. */
static ssize_t tpm_version_major_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    tpm_device_t *tpm = tpm_dev_from(dev);
    (void)attr;
    return sysfs_emit(buf, "%d\n", tpm ? tpm->version : 0);
}

/* Show the TPM specification minor version. */
static ssize_t tpm_version_minor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "0\n");
}

/* Show the TPM firmware version. */
static ssize_t tpm_fwver_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    tpm_device_t *tpm = tpm_dev_from(dev);
    uint32_t      fw  = 0;
    (void)attr;
    if (!tpm) return -ENODEV;
    if (tpm->version == TPM_VERSION_20) (void)tpm2_get_property(tpm, TPM2_PT_FIRMWARE_VERSION_1, &fw);
    return sysfs_emit(buf, "0x%08x\n", fw);
}

/* Show a human-readable TPM description. */
static ssize_t description_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    tpm_device_t *tpm = tpm_dev_from(dev);
    (void)attr;
    if (!tpm) return -ENODEV;
    return sysfs_emit(buf, "%s\n", tpm->version == TPM_VERSION_20 ? "TPM 2.0 Device" : "TPM 1.2 Device");
}

/* Show the TPM manufacturer, PCR count, and revision. */
static ssize_t caps_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    tpm_device_t *tpm          = tpm_dev_from(dev);
    uint32_t      manufacturer = 0, pcr_count = 0, revision = 0;
    (void)attr;
    if (!tpm) return -ENODEV;
    if (tpm->version == TPM_VERSION_20) {
        (void)tpm2_get_property(tpm, TPM2_PT_MANUFACTURER, &manufacturer);
        (void)tpm2_get_property(tpm, TPM2_PT_PCR_COUNT, &pcr_count);
        (void)tpm2_get_property(tpm, TPM2_PT_REVISION, &revision);
    } else {
        manufacturer = tpm->did_vid & 0xFFFF;
        pcr_count    = 24;
    }
    return sysfs_emit(buf, "Manufacturer: 0x%08x\nPCR count: %u\nRevision: 0x%08x\n", manufacturer, pcr_count, revision);
}

/* Dump the first 24 PCR register values. */
static ssize_t pcrs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    tpm_device_t *tpm = tpm_dev_from(dev);
    int           at  = 0;
    (void)attr;
    if (!tpm) return -ENODEV;
    if (tpm->version != TPM_VERSION_20) return -EOPNOTSUPP;
    for (uint32_t i = 0; i < 24; i++) {
        uint8_t digest[32];
        int     len = tpm2_pcr_read(tpm, i, digest);
        if (len < 0) continue;
        at += sysfs_emit_at(buf, at, "PCR-%02u: ", i);
        for (int j = 0; j < len && j < (int)sizeof(digest); j++) at += sysfs_emit_at(buf, at, "%02x", digest[j]);
        at += sysfs_emit_at(buf, at, "\n");
        if (at > SYSFS_PAGE_SIZE - 128) break;
    }
    return at;
}

/* Show the TPM timeout values. */
static ssize_t timeouts_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    tpm_device_t *tpm = tpm_dev_from(dev);
    (void)attr;
    if (!tpm) return -ENODEV;
    return sysfs_emit(buf, "%u %u %u %u\n", tpm->timeout_a, tpm->timeout_b, tpm->timeout_c, tpm->timeout_d);
}

static DEVICE_ATTR(tpm_version_major, 0444, tpm_version_major_show, NULL);
static DEVICE_ATTR(tpm_version_minor, 0444, tpm_version_minor_show, NULL);
static DEVICE_ATTR(tpm_fwver, 0444, tpm_fwver_show, NULL);
static DEVICE_ATTR(description, 0444, description_show, NULL);
static DEVICE_ATTR(caps, 0444, caps_show, NULL);
static DEVICE_ATTR(pcrs, 0444, pcrs_show, NULL);
static DEVICE_ATTR(timeouts, 0444, timeouts_show, NULL);

static struct attribute *tpm_dev_attributes[] = {
    &dev_attr_tpm_version_major.attr, &dev_attr_tpm_version_minor.attr, &dev_attr_tpm_fwver.attr, &dev_attr_description.attr, &dev_attr_caps.attr, &dev_attr_pcrs.attr, &dev_attr_timeouts.attr, NULL,
};

static struct attribute_group tpm_dev_group = {
    .attrs = tpm_dev_attributes,
};

static const struct attribute_group *tpm_dev_groups[] = {
    &tpm_dev_group,
    NULL,
};

static struct class tpm_class   = {.name = "tpm", .dev_groups = tpm_dev_groups};
static struct class tpmrm_class = {.name = "tpmrm"};

/* Register the TPM class devices. */
void tpm_sysfs_init(void)
{
#if CONFIG_SYSFS
    tpm_device_t *tpm = tpm_get_device();
    if (!tpm) return;

    if (class_register(&tpm_class) == EOK) (void)device_create(&tpm_class, NULL, MKDEV(TPM_DEV_MAJOR, TPM0_MINOR), tpm, "tpm0");
    if (class_register(&tpmrm_class) == EOK) (void)device_create(&tpmrm_class, NULL, MKDEV(TPM_DEV_MAJOR, TPMRM0_MINOR), tpm, "tpmrm0");
    plogk("tpm_sysfs: registered /sys/class/tpm/tpm0 and /sys/class/tpmrm/tpmrm0\n");
#endif
}
