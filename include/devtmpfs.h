/*
 *
 *      devtmpfs.h
 *      Device tmpfs population helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DEVTMPFS_H_
#define INCLUDE_DEVTMPFS_H_

/*
 * Initialize /dev on top of tmpfs and populate built-in device nodes.
 *
 * This currently includes block devices discovered from IDE and the PS/2
 * keyboard event device at /dev/input/event0.
 */
void devtmpfs_init(void);

#endif // INCLUDE_DEVTMPFS_H_
