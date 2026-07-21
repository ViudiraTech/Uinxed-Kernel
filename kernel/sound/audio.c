/*
 *
 *      audio.c
 *      Generic audio subsystem
 *
 *      2026/7/20 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <printk.h>
#include <sound/audio.h>
#include <string.h>
#include <uaccess.h>

#define AUDIO_NODES_PER_CARD 3
#define AUDIO_MAX_NODES      (AUDIO_MAX_CARDS * AUDIO_NODES_PER_CARD)

static audio_card_t        audio_cards[AUDIO_MAX_CARDS];
static audio_device_node_t audio_nodes[AUDIO_MAX_NODES];
static char                audio_node_names[AUDIO_MAX_NODES][24];
static size_t              audio_cards_count;
static size_t              audio_nodes_count;

static const char *audio_node_suffix(audio_node_type_t type)
{
    switch (type) {
        case audio_node_control :
            return "controlC%u";
        case audio_node_pcm_playback :
            return "pcmC%uD0p";
        case audio_node_mixer :
            return "mixerC%uD0";
    }
    return "unknownC%u";
}

static int audio_add_node(audio_card_t *card, audio_node_type_t type)
{
    audio_device_node_t *node;

    if (audio_nodes_count >= AUDIO_MAX_NODES) return -ENOSPC;

    node = &audio_nodes[audio_nodes_count];
    snprintf(audio_node_names[audio_nodes_count], sizeof(audio_node_names[audio_nodes_count]), audio_node_suffix(type), card->id);

    node->card            = card;
    node->type            = type;
    node->name            = audio_node_names[audio_nodes_count];
    node->tmpfs_ops.read  = audio_device_read;
    node->tmpfs_ops.write = audio_device_write;
    node->tmpfs_ops.poll  = audio_device_poll;
    node->tmpfs_ops.ioctl = audio_device_ioctl;
    node->tmpfs_ops.ctx   = node;

    audio_nodes_count++;
    return EOK;
}

static void audio_fill_info(audio_card_t *card, audio_card_info_t *info)
{
    info->card        = card->id;
    info->sample_rate = card->format.sample_rate;
    info->bits        = card->format.bits;
    info->channels    = card->format.channels;
    strncpy(info->name, card->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
}

int audio_register_card(const char *name, const audio_pcm_format_t *format, const audio_card_ops_t *ops, void *driver_data)
{
    audio_card_t *card;
    int           status;
    size_t        node_base;

    if (!name || !format || !ops) return -EINVAL;
    if (audio_cards_count >= AUDIO_MAX_CARDS) return -ENOSPC;

    card = &audio_cards[audio_cards_count];
    memset(card, 0, sizeof(*card));

    card->id          = audio_cards_count;
    card->format      = *format;
    card->ops         = ops;
    card->driver_data = driver_data;
    strncpy(card->name, name, sizeof(card->name) - 1);

    node_base = audio_nodes_count;
    status    = audio_add_node(card, audio_node_control);
    if (status != EOK) goto rollback;
    status = audio_add_node(card, audio_node_pcm_playback);
    if (status != EOK) goto rollback;
    status = audio_add_node(card, audio_node_mixer);
    if (status != EOK) goto rollback;

    audio_cards_count++;
    plogk("audio: Registered card%u: %s\n", card->id, card->name);
    return (int)card->id;

rollback:
    audio_nodes_count = node_base;
    memset(card, 0, sizeof(*card));
    return status;
}

audio_card_t *audio_get_card(uint32_t card)
{
    if (card >= audio_cards_count) return 0;
    return &audio_cards[card];
}

size_t audio_card_count(void)
{
    return audio_cards_count;
}

size_t audio_device_node_count(void)
{
    return audio_nodes_count;
}

audio_device_node_t *audio_get_device_node(size_t index)
{
    if (index >= audio_nodes_count) return 0;
    return &audio_nodes[index];
}

size_t audio_device_read(void *ctx, void *addr, size_t offset, size_t size)
{
    audio_device_node_t *node = ctx;
    audio_card_info_t    info;

    if (!node || !node->card || !addr) return 0;

    if (node->type == audio_node_control) {
        audio_fill_info(node->card, &info);
        if (offset >= sizeof(info)) return 0;
        if (offset + size > sizeof(info)) size = sizeof(info) - offset;
        memcpy(addr, ((uint8_t *)&info) + offset, size);
        return size;
    }

    if (node->type == audio_node_pcm_playback && node->card->ops->pcm_read) { return node->card->ops->pcm_read(node->card, addr, offset, size); }
    return 0;
}

size_t audio_device_write(void *ctx, const void *addr, size_t offset, size_t size)
{
    audio_device_node_t *node = ctx;

    if (!node || !node->card || !addr) return 0;
    if (node->type != audio_node_pcm_playback || !node->card->ops->pcm_write) return 0;

    return node->card->ops->pcm_write(node->card, addr, offset, size);
}

int audio_device_poll(void *ctx, size_t events)
{
    (void)ctx;
    return (int)(events & 0x0005);
}

int audio_device_ioctl(void *ctx, size_t req, void *arg)
{
    audio_device_node_t *node = ctx;
    audio_card_info_t    info;
    audio_pcm_format_t   format;
    audio_volume_t       volume;
    int                  status;

    if (!node || !node->card) return -ENODEV;

    switch (req) {
        case AUDIO_IOCTL_GET_INFO :
            if (!arg) return -EFAULT;
            audio_fill_info(node->card, &info);
            if (copy_to_user(arg, &info, sizeof(info))) return -EFAULT;
            return EOK;
        case AUDIO_IOCTL_SET_FORMAT :
            if (!arg) return -EFAULT;
            if (!node->card->ops->set_format) return -EINVAL;
            if (copy_from_user(&format, arg, sizeof(format))) return -EFAULT;
            return node->card->ops->set_format(node->card, &format);
        case AUDIO_IOCTL_STOP :
            if (!node->card->ops->stop) return -ENOSYS;
            return node->card->ops->stop(node->card);
        case AUDIO_IOCTL_SET_VOLUME :
            if (!arg) return -EFAULT;
            if (!node->card->ops->set_volume) return -EINVAL;
            if (copy_from_user(&volume, arg, sizeof(volume))) return -EFAULT;
            return node->card->ops->set_volume(node->card, &volume);
        case AUDIO_IOCTL_GET_VOLUME :
            if (!arg) return -EFAULT;
            if (!node->card->ops->get_volume) return -EINVAL;
            status = node->card->ops->get_volume(node->card, &volume);
            if (status != EOK) return status;
            if (copy_to_user(arg, &volume, sizeof(volume))) return -EFAULT;
            return EOK;
        default :
            return -EINVAL;
    }
}
