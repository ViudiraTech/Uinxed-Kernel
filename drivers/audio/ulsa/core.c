/*
 *
 *      core.c
 *      ULSA card registry and /dev/snd device nodes
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/sysfs/sound_sysfs.h>
#include <kernel/debug/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

#if CONFIG_AUDIO

#    define ULSA_MAX_CARDS ((CONFIG_AUDIO_MAX_CARDS) < SNDRV_CARDS ? (CONFIG_AUDIO_MAX_CARDS) : SNDRV_CARDS)

static ulsa_card_t  *cards[SNDRV_CARDS];
static ulsa_minor_t *minors[256];
static spinlock_t    cards_lock;
static bool          ulsa_ready;

/* Return the card at `number` if it is registered. */
ulsa_card_t *ulsa_card_get(int number)
{
    ulsa_card_t *card;
    uint64_t     flags;

    if (number < 0 || number >= SNDRV_CARDS) return NULL;
    flags = spin_lock_irqsave(&cards_lock);
    card  = cards[number];
    if (card && !card->registered) card = NULL;
    spin_unlock_irqrestore(&cards_lock, flags);
    return card;
}

/* Number of registered cards. */
int ulsa_card_count_registered(void)
{
    int n = 0;

    for (int i = 0; i < SNDRV_CARDS; i++)
        if (cards[i] && cards[i]->registered) n++;
    return n;
}

/* Return the published minor entry at `number`, or NULL. */
ulsa_minor_t *ulsa_minor_get(int number)
{
    ulsa_minor_t *minor;
    uint64_t      flags;

    if (number < 0 || number >= 256) return NULL;
    flags = spin_lock_irqsave(&cards_lock);
    minor = minors[number];
    spin_unlock_irqrestore(&cards_lock, flags);
    return minor;
}

/* True when another card already uses `id`. */
static bool card_id_in_use(const char *id, const ulsa_card_t *self)
{
    for (int i = 0; i < SNDRV_CARDS; i++)
        if (cards[i] && cards[i] != self && !strcmp(cards[i]->id, id)) return true;
    return false;
}

/* Derive a unique card id (alphanumerics, '_' and '-') from a driver name; clashes get a _<n> suffix as on Linux. */
static void card_set_id(ulsa_card_t *card, const char *src)
{
    char   base[ULSA_CARD_ID_LEN];
    size_t len = 0;

    for (const char *p = src ? src : ""; *p && len < sizeof(base) - 1; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') base[len++] = c;
    }
    if (!len) len = (size_t)snprintf(base, sizeof(base), "Card");
    base[len] = '\0';
    memcpy(card->id, base, len + 1);

    for (int suffix = 1; suffix < 100 && card_id_in_use(card->id, card); suffix++) {
        char   tail[8];
        size_t tlen = (size_t)snprintf(tail, sizeof(tail), "_%d", suffix);
        size_t keep = len + tlen < sizeof(card->id) ? len : sizeof(card->id) - 1 - tlen;

        memcpy(card->id, base, keep);
        memcpy(card->id + keep, tail, tlen + 1);
    }
}

/* Allocate a card and reserve its number. */
int ulsa_card_new(int idx, const char *id, ulsa_card_t **card_ret)
{
    ulsa_card_t *card;
    uint64_t     flags;

    if (!card_ret) return -EINVAL;
    *card_ret = NULL;
    card      = calloc(1, sizeof(*card));
    if (!card) return -ENOMEM;
    mutex_init(&card->controls_lock);

    flags = spin_lock_irqsave(&cards_lock);
    if (idx < 0) {
        for (idx = 0; idx < ULSA_MAX_CARDS; idx++)
            if (!cards[idx]) break;
    }
    if (idx >= ULSA_MAX_CARDS || cards[idx]) {
        spin_unlock_irqrestore(&cards_lock, flags);
        free(card);
        return -ENODEV;
    }
    card->number = idx;
    cards[idx]   = card;
    card_set_id(card, id);
    spin_unlock_irqrestore(&cards_lock, flags);

    *card_ret = card;
    return EOK;
}

/* Append a space-separated component tag. */
int ulsa_component_add(ulsa_card_t *card, const char *component)
{
    size_t len  = strlen(card->components);
    size_t clen = strlen(component);

    if (strstr(card->components, component)) return EOK;
    if (len + (len ? 1 : 0) + clen + 1 > sizeof(card->components)) return -ENOMEM;
    if (len) card->components[len++] = ' ';
    memcpy(card->components + len, component, clen + 1);
    return EOK;
}

/* Static minor for a device. */
static int minor_for(enum ulsa_device_type type, int card, int dev)
{
    switch (type) {
        case ULSA_DEVICE_CONTROL :
            return SNDRV_MINOR(card, SNDRV_MINOR_CONTROL);
        case ULSA_DEVICE_PCM_PLAYBACK :
            return SNDRV_MINOR(card, SNDRV_MINOR_PCM_PLAYBACK + dev);
        case ULSA_DEVICE_PCM_CAPTURE :
            return SNDRV_MINOR(card, SNDRV_MINOR_PCM_CAPTURE + dev);
        case ULSA_DEVICE_TIMER :
            return SNDRV_MINOR_TIMER;
    }
    return -EINVAL;
}

/* Publish /dev/snd/<name> with the static minor of (type, card, dev). */
int ulsa_register_device(enum ulsa_device_type type, ulsa_card_t *card, ulsa_pcm_t *pcm, int stream, int dev, const char *name, const tmpfs_device_ops_t *ops, ulsa_minor_t **minor_ret)
{
    ulsa_minor_t      *minor;
    tmpfs_device_ops_t node_ops;
    char               path[64];
    int                number = minor_for(type, card ? card->number : 0, dev);
    dev_t              devt;
    int                err;

    if (number < 0 || number >= 256) return -EINVAL;
    if (minors[number]) return -EBUSY;
    minor = calloc(1, sizeof(*minor));
    if (!minor) return -ENOMEM;
    minor->type   = type;
    minor->minor  = number;
    minor->card   = card;
    minor->pcm    = pcm;
    minor->stream = stream;
    strncpy(minor->name, name, sizeof(minor->name) - 1);

    node_ops     = *ops;
    node_ops.ctx = minor;
    devt         = MKDEV(SNDRV_MAJOR, (uint32_t)number);
    (void)snprintf(path, sizeof(path), "/dev/snd/%s", minor->name);
    err = devtmpfs_register_char_device(path, devt, devt, file_audio | file_stream, &node_ops);
    if (err != EOK) {
        plogk("ulsa: Failed to create %s: %d\n", path, err);
        free(minor);
        return err;
    }
    minor->published = true;
    minors[number]   = minor;
    sound_sysfs_minor_register(minor);
    if (minor_ret) *minor_ret = minor;
    return EOK;
}

/* Remove a /dev/snd node. */
void ulsa_unregister_device(ulsa_minor_t *minor)
{
    char path[64];

    if (!minor) return;
    sound_sysfs_minor_unregister(minor);
    if (minor->published) {
        (void)snprintf(path, sizeof(path), "/dev/snd/%s", minor->name);
        (void)devtmpfs_unregister_char_device(path);
    }
    if (minor->minor >= 0 && minor->minor < 256 && minors[minor->minor] == minor) minors[minor->minor] = NULL;
    free(minor);
}

/* Make a card and its devices visible. */
int ulsa_card_register(ulsa_card_t *card)
{
    int err;

    if (!card) return -EINVAL;
    if (card->registered) return EOK;
    if (!card->driver[0]) strncpy(card->driver, "ULSA", sizeof(card->driver) - 1);
    if (!card->shortname[0]) strncpy(card->shortname, card->driver, sizeof(card->shortname) - 1);
    if (!card->longname[0]) strncpy(card->longname, card->shortname, sizeof(card->longname) - 1);
    if (!card->mixername[0]) strncpy(card->mixername, card->shortname, sizeof(card->mixername) - 1);

    sound_sysfs_card_register(card);
    err = ulsa_ctl_register(card);
    if (err < 0) goto fail;
    err = ulsa_pcm_register_all(card);
    if (err < 0) goto fail;
    card->registered = true;
    (void)ulsa_info_card_register(card);
    plogk("ulsa: card%d: %s (%s)\n", card->number, card->longname, card->id);
    return EOK;

fail:
    plogk("ulsa: Failed to register card%d: %d\n", card->number, err);
    return err;
}

/* Tear a card down completely. */
void ulsa_card_free(ulsa_card_t *card)
{
    uint64_t flags;

    if (!card) return;
    ulsa_info_card_free(card);
    ulsa_pcm_free_all(card);
    ulsa_ctl_free_all(card);
    if (card->ctl_minor) ulsa_unregister_device(card->ctl_minor);
    card->ctl_minor = NULL;
    sound_sysfs_card_unregister(card);
    if (card->private_free) card->private_free(card);

    flags = spin_lock_irqsave(&cards_lock);
    if (card->number >= 0 && card->number < SNDRV_CARDS && cards[card->number] == card) cards[card->number] = NULL;
    spin_unlock_irqrestore(&cards_lock, flags);
    free(card);
}

/* Bring up the global parts of the sound core. */
void ulsa_init(void)
{
    if (ulsa_ready) return;
    ulsa_ready = true;
    (void)ulsa_timer_init();
    (void)ulsa_info_init();
}

#endif
