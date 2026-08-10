/*
 *
 *      boot_process.h
 *      Boot process
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BOOT_PROCESS_H_
#define INCLUDE_BOOT_PROCESS_H_

typedef void (*boot_process_start_t)(void);

void boot_start_init_before_debug(boot_process_start_t start_init, boot_process_start_t start_debug);

#endif // INCLUDE_BOOT_PROCESS_H_
