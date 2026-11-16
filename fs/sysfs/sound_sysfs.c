/*
 *
 *      sound_sysfs.c
 *      /sys/class/sound - Linux-layout ULSA sysfs and uevent integration
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <drivers/base/device.h>
#include <fs/sysfs/sound_sysfs.h>
#include <kernel/debug/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#if CONFIG_AUDIO

#    if CONFIG_SYSFS

struct sound_minor_device {
        struct device dev;
        char          devnode[32];
};

static struct class sound_class = {.name = "sound"};
static bool sound_class_ready;

/* Ensure the "sound" device class is registered on first use. */
static bool sound_class_ensure(void)
{
    if (sound_class_ready) return true;
    if (class_register(&sound_class) != EOK) {
        plogk("sound_sysfs: class_register(sound) failed.\n");
        return false;
    }
    sound_class_ready = true;
    return true;
}

/* Return the ULSA card bound to a cardN sysfs device. */
static ulsa_card_t *sound_card_from(struct device *dev)
{
    return dev ? dev->driver_data : NULL;
}

/* Show the sound card identifier string. */
static ssize_t id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ulsa_card_t *card = sound_card_from(dev);
    (void)attr;
    return sysfs_emit(buf, "%s\n", card ? card->id : "");
}

/* Show the sound card index number. */
static ssize_t number_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ulsa_card_t *card = sound_card_from(dev);
    (void)attr;
    return sysfs_emit(buf, "%d\n", card ? card->number : 0);
}

static DEVICE_ATTR(id, 0444, id_show, NULL);
static DEVICE_ATTR(number, 0444, number_show, NULL);

static struct attribute *card_attributes[] = {
    &dev_attr_id.attr,
    &dev_attr_number.attr,
    NULL,
};

static struct attribute_group card_group = {
    .attrs = card_attributes,
};

static const struct attribute_group *card_groups[] = {
    &card_group,
    NULL,
};

/* Free a card sysfs device. */
static void sound_card_release(struct device *dev)
{
    free(dev);
}

/* Free a minor sysfs device along with its embedded devnode string. */
static void sound_minor_release(struct device *dev)
{
    struct sound_minor_device *sdev = container_of(dev, struct sound_minor_device, dev);
    free(sdev);
}

/* Register /sys/class/sound/cardN for a ULSA card. */
void sound_sysfs_card_register(ulsa_card_t *card)
{
    struct device *dev;

    if (!card || card->sysfs_priv || !sound_class_ensure()) return;
    dev = calloc(1, sizeof(*dev));
    if (!dev) return;
    dev->class       = &sound_class;
    dev->driver_data = card;
    dev->groups      = card_groups;
    dev->release     = sound_card_release;
    if (kobject_set_name(&dev->kobj, "card%d", card->number) != EOK) {
        free(dev);
        return;
    }
    if (device_register(dev) != EOK) {
        put_device(dev);
        return;
    }
    card->sysfs_priv = dev;
}

/* Unregister /sys/class/sound/cardN for a ULSA card. */
void sound_sysfs_card_unregister(ulsa_card_t *card)
{
    if (!card || !card->sysfs_priv) return;
    device_unregister(card->sysfs_priv);
    card->sysfs_priv = NULL;
}

/* Register /sys/class/sound/<name> (controlCN, pcmCNDMx, timer) with MAJOR/MINOR/DEVNAME. */
void sound_sysfs_minor_register(ulsa_minor_t *minor)
{
    struct sound_minor_device *sdev;
    struct device             *parent;

    if (!minor || minor->sysfs_dev || !sound_class_ensure()) return;
    parent = (minor->type != ULSA_DEVICE_TIMER && minor->card) ? minor->card->sysfs_priv : NULL;
    sdev   = calloc(1, sizeof(*sdev));
    if (!sdev) return;

    (void)snprintf(sdev->devnode, sizeof(sdev->devnode), "snd/%s", minor->name);
    sdev->dev.class       = &sound_class;
    sdev->dev.parent      = parent;
    sdev->dev.devt        = MKDEV(SNDRV_MAJOR, (uint32_t)minor->minor);
    sdev->dev.driver_data = minor;
    sdev->dev.devnode     = sdev->devnode;
    sdev->dev.release     = sound_minor_release;
    if (kobject_set_name(&sdev->dev.kobj, "%s", minor->name) != EOK) {
        free(sdev);
        return;
    }
    if (device_register(&sdev->dev) != EOK) {
        put_device(&sdev->dev);
        return;
    }
    minor->sysfs_dev = &sdev->dev;
}

/* Unregister /sys/class/sound/<name> for a ULSA minor device. */
void sound_sysfs_minor_unregister(ulsa_minor_t *minor)
{
    if (!minor || !minor->sysfs_dev) return;
    device_unregister(minor->sysfs_dev);
    minor->sysfs_dev = NULL;
}

#    else

/* No-op sysfs card registration when CONFIG_SYSFS is disabled. */
void sound_sysfs_card_register(ulsa_card_t *card)
{
    (void)card;
}

/* No-op sysfs card unregistration when CONFIG_SYSFS is disabled. */
void sound_sysfs_card_unregister(ulsa_card_t *card)
{
    (void)card;
}

/* No-op sysfs minor registration when CONFIG_SYSFS is disabled. */
void sound_sysfs_minor_register(ulsa_minor_t *minor)
{
    (void)minor;
}

/* No-op sysfs minor unregistration when CONFIG_SYSFS is disabled. */
void sound_sysfs_minor_unregister(ulsa_minor_t *minor)
{
    (void)minor;
}

#    endif

#endif
