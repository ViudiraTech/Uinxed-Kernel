/*
 *
 *      mem_sysfs.h
 *      Memory character-device class header
 *
 *      2026/8/6 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MEM_SYSFS_H_
#define INCLUDE_MEM_SYSFS_H_

/* Register the "mem" class and its standard device nodes in sysfs. */
void mem_sysfs_init(void);

#endif // INCLUDE_MEM_SYSFS_H_
