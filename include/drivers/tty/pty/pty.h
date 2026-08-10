/*
 *
 *      pty.h
 *      Pseudoterminal driver definitions
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_PTY_H_
#define INCLUDE_DRIVERS_PTY_H_

#include <fs/virtual/tmpfs.h>

#define PTMX_MAJOR 5
#define PTMX_MINOR 2

#if CONFIG_UNIX98_PTYS
/*
 * ptmx device operations table.  devtmpfs registers /dev/ptmx with this
 * table during boot (see devtmpfs_create_ptmx_node()).
 */
extern const tmpfs_device_ops_t pty_ptmx_operations;
#endif

#endif
