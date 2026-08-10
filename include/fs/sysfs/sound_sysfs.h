/*
 *
 *      sound_sysfs.h
 *      Sound card sysfs integration header
 *
 *      2026/8/6 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SOUND_SYSFS_H_
#define INCLUDE_SOUND_SYSFS_H_

/* Register the "sound" class and per-card devices (/sys/class/sound/cardN). */
void sound_sysfs_init(void);

#endif // INCLUDE_SOUND_SYSFS_H_
