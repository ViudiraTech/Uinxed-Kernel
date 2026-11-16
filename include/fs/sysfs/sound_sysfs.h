/*
 *
 *      sound_sysfs.h
 *      Sound card sysfs integration header
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SOUND_SYSFS_H_
#define INCLUDE_SOUND_SYSFS_H_

struct ulsa_card;
struct ulsa_minor;

#if CONFIG_AUDIO
/* Register a sound card device under /sys/class/sound/cardN. */
void sound_sysfs_card_register(struct ulsa_card *card);

/* Unregister a sound card device from /sys/class/sound. */
void sound_sysfs_card_unregister(struct ulsa_card *card);

/* Register an ALSA minor device node under /sys/class/sound. */
void sound_sysfs_minor_register(struct ulsa_minor *minor);

/* Unregister an ALSA minor device node from /sys/class/sound. */
void sound_sysfs_minor_unregister(struct ulsa_minor *minor);
#else
static inline void sound_sysfs_card_register(struct ulsa_card *card)
{
    (void)card;
}
static inline void sound_sysfs_card_unregister(struct ulsa_card *card)
{
    (void)card;
}
static inline void sound_sysfs_minor_register(struct ulsa_minor *minor)
{
    (void)minor;
}
static inline void sound_sysfs_minor_unregister(struct ulsa_minor *minor)
{
    (void)minor;
}
#endif

#endif // INCLUDE_SOUND_SYSFS_H_
