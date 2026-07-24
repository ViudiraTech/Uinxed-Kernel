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

#include <fs/tmpfs.h>
#include <libs/std/stdint.h>

/*
 * Initialize /dev on top of tmpfs and populate built-in device nodes.
 *
 * This currently includes block devices discovered from IDE, input devices,
 * framebuffer devices, and registered audio devices under /dev/snd.
 */
void devtmpfs_init(void);

/*
 * Register a character device node under /dev.
 *
 * Creates parent directories as needed, allocates a tmpfs-backed VFS node,
 * binds the supplied device operations, and records the entry for later
 * unregistration.
 *
 * @path:      absolute path e.g. "/dev/dri/card0"
 * @dev:       device number (use MKDEV(major, minor) or encode manually)
 * @rdev:      raw device number (for stat)
 * @node_type: VFS type flags (file_stream, file_keyboard, etc.)
 * @ops:       device operations (copied into the tmpfs handle)
 * @ctx:       opaque context pointer stored in ops.ctx
 *
 * Returns 0 on success, negative errno on failure.
 */
int devtmpfs_register_char_device(const char *path, uint64_t dev, uint64_t rdev, uint16_t node_type, const tmpfs_device_ops_t *ops);

/*
 * Unregister and remove a character device node previously registered via
 * devtmpfs_register_char_device().
 *
 * @path: absolute path under /dev
 *
 * Returns 0 on success, negative errno on failure.
 */
int devtmpfs_unregister_char_device(const char *path);

#endif // INCLUDE_DEVTMPFS_H_
