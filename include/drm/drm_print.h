/*
 *
 *      drm_print.h
 *      DRM printing infrastructure
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux DRM printer API (include/drm/drm_print.h).
 *  Implemented on top of the kernel printk/plogk backends.
 *
 */

#ifndef INCLUDE_DRM_DRM_PRINT_H_
#define INCLUDE_DRM_DRM_PRINT_H_

#include <printk.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

struct drm_device;

enum drm_debug_category {
    DRM_UT_CORE      = 0x01,
    DRM_UT_DRIVER    = 0x02,
    DRM_UT_KMS       = 0x04,
    DRM_UT_MODE      = 0x08,
    DRM_UT_STATE     = 0x10,
    DRM_UT_LEASE     = 0x20,
    DRM_UT_DP        = 0x40,
    DRM_UT_DRMRES    = 0x80,
};

struct drm_printer {
    void (*printfn)(void *arg, const char *fmt, va_list args);
    void (*hex)(void *arg, ...); // optional, unused
    void  *arg;
    void  *extra;
};

#define DRM_PRINTK_FMT "[drm] "

/* Construct a printk-backed printer. */
struct drm_printer drm_printk_printer(const char *prefix);

/* Format-agnostic printf through a printer. */
void drm_vprintf(struct drm_printer *p, const char *fmt, va_list args);

/* printf through a printer. */
void drm_printf(struct drm_printer *p, const char *fmt, ...);

/* Device-level printk helpers. */
void drm_dev_printk(const struct drm_device *dev, const char *level, const char *fmt, ...);

/* Convenience macros wrapping plogk. */
#define DRM_INFO(fmt, ...)  plogk("[drm] " fmt, ##__VA_ARGS__)
#define DRM_ERROR(fmt, ...) plogk("[drm:ERROR] " fmt, ##__VA_ARGS__)
#define DRM_DEBUG(fmt, ...) plogk("[drm:DEBUG] " fmt, ##__VA_ARGS__)
#define DRM_DEBUG_KMS(fmt, ...) plogk("[drm:KMS] " fmt, ##__VA_ARGS__)
#define DRM_DEBUG_DRIVER(fmt, ...) plogk("[drm:DRV] " fmt, ##__VA_ARGS__)
#define DRM_WARN(fmt, ...) plogk("[drm:WARN] " fmt, ##__VA_ARGS__)
#define DRM_DEV_ERROR(dev, fmt, ...) drm_dev_printk(dev, "error", fmt, ##__VA_ARGS__)
#define DRM_DEV_INFO(dev, fmt, ...)  drm_dev_printk(dev, "info", fmt, ##__VA_ARGS__)
#define DRM_DEV_WARN(dev, fmt, ...)  drm_dev_printk(dev, "warn", fmt, ##__VA_ARGS__)

#endif /* INCLUDE_DRM_DRM_PRINT_H_ */
