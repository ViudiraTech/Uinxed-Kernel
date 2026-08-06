/*
 *
 *      sound_sysfs.c
 *      /sys/class/sound - sound card class integration
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/core/device.h>
#include <fs/sysfs/sound_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/audio.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>

static bool sound_class_ready;

static audio_card_t *sound_card_from(struct device *dev)
{
    return dev ? dev->driver_data : NULL;
}

static ssize_t id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    audio_card_t *card = sound_card_from(dev);
    (void)attr;
    return sysfs_emit(buf, "%s\n", card ? card->name : "");
}

static ssize_t number_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    audio_card_t *card = sound_card_from(dev);
    (void)attr;
    return sysfs_emit(buf, "%u\n", card ? card->id : 0);
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

static struct class sound_class = {.name = "sound", .dev_groups = card_groups};

/* Sub-device (controlC0, pcmC0D0p, ...): report the owning card number. */
static ssize_t device_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct device *parent = dev ? dev->parent : NULL;
    audio_card_t  *card   = sound_card_from(parent);
    (void)attr;
    return sysfs_emit(buf, "%u\n", card ? card->id : 0);
}

static DEVICE_ATTR(device, 0444, device_show, NULL);

static struct attribute *node_attributes[] = {
    &dev_attr_device.attr,
    NULL,
};

static struct attribute_group node_group = {
    .attrs = node_attributes,
};

static const struct attribute_group *node_groups[] = {
    &node_group,
    NULL,
};

static void sound_node_release(struct device *dev)
{
    free(dev);
}

static struct device *sound_create_node(struct device *parent, audio_card_t *card, const char *name)
{
    struct device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->class       = &sound_class;
    dev->parent      = parent;
    dev->driver_data = card;
    dev->groups      = node_groups;
    dev->release     = sound_node_release;
    if (kobject_set_name(&dev->kobj, "%s", name) != EOK || device_register(dev) != EOK) {
        free(dev);
        return NULL;
    }
    return dev;
}

void sound_sysfs_init(void)
{
#if CONFIG_SYSFS
    size_t         cards;
    struct device *card_devs[AUDIO_MAX_CARDS];

    if (sound_class_ready) return;
    if (class_register(&sound_class) != EOK) {
        plogk("sound_sysfs: class_register(sound) failed\n");
        return;
    }
    sound_class_ready = true;

    cards = audio_card_count();
    if (cards > AUDIO_MAX_CARDS) cards = AUDIO_MAX_CARDS;

    for (size_t c = 0; c < cards; c++) {
        audio_card_t *card = audio_get_card((uint32_t)c);
        if (!card) continue;
        card_devs[c] = device_create(&sound_class, NULL, 0, card, "card%u", card->id);
    }

    /* Publish each ALSA-style node name (controlC0, pcmC0D0p, ...) as a
     * sub-device of its card, matching the /dev/snd/<name> nodes. */
    for (size_t n = 0; n < audio_device_node_count(); n++) {
        audio_device_node_t *node = audio_get_device_node(n);
        if (!node || !node->card || node->card->id >= cards) continue;
        if (!card_devs[node->card->id]) continue;
        (void)sound_create_node(card_devs[node->card->id], node->card, node->name);
    }

    plogk("sound_sysfs: %zu sound card(s) exported to /sys/class/sound\n", cards);
#endif
}
