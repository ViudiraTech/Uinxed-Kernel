/*
 *
 *      boot_process.c
 *      Boot process
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <proc/boot_process.h>

void boot_start_init_before_debug(boot_process_start_t start_init, boot_process_start_t start_debug)
{
    if (start_init) start_init();
    if (start_debug) start_debug();
}
