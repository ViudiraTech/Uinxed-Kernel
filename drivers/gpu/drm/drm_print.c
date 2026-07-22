/*
 *
 *      drm_print.c
 *      DRM printing infrastructure
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_print.h>
#include <printk.h>
#include <alloc.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>

/*
 * Printer callback for drm_printk_printer: renders the formatted message into a
 * stack buffer with the configured prefix prepended, then emits it via plogk.
 */
static void __drm_printk_printer(void *arg, const char *fmt, va_list args)
{
    const char *prefix = (const char *)arg;
    char buf[512];
    size_t plen;

    plen = prefix ? strlen(prefix) : 0;
    if (plen >= sizeof(buf))
        plen = sizeof(buf) - 1;
    if (plen)
        memcpy(buf, prefix, plen);

    vsnprintf(buf + plen, sizeof(buf) - plen, fmt, args);
    plogk("%s", buf);
}

/*
 * Construct a printk-backed printer. The prefix is duplicated with malloc and
 * stored in ->extra (owned storage the caller may free once the printer is no
 * longer needed); the same pointer is also placed in ->arg so the printfn
 * callback receives it. On allocation failure the printer degrades to a
 * prefix-less printer that still emits messages correctly.
 */
struct drm_printer drm_printk_printer(const char *prefix)
{
    struct drm_printer p;

    memset(&p, 0, sizeof(p));
    p.printfn = __drm_printk_printer;

    if (prefix) {
        size_t len = strlen(prefix) + 1;
        char *copy = malloc(len);
        if (copy) {
            memcpy(copy, prefix, len);
            p.arg = copy;
            p.extra = copy;
        }
    }

    return p;
}

/* Forward a va_list message to the printer's printfn callback, if installed. */
void drm_vprintf(struct drm_printer *p, const char *fmt, va_list args)
{
    if (p && p->printfn)
        p->printfn(p->arg, fmt, args);
}

/* Variadic printf through a printer. */
void drm_printf(struct drm_printer *p, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    drm_vprintf(p, fmt, args);
    va_end(args);
}

/*
 * Device-level printk helper. Builds a "[drm:level]" prefix on the stack
 * (annotated with the device pointer when @dev is non-NULL) and forwards the
 * formatted message to the kernel log via plogk.
 */
void drm_dev_printk(const struct drm_device *dev, const char *level, const char *fmt, ...)
{
    char prefix[256];
    char msg[512];
    va_list args;

    if (dev)
        snprintf(prefix, sizeof(prefix), "[drm:%s] %p ", level, (const void *)dev);
    else
        snprintf(prefix, sizeof(prefix), "[drm:%s] ", level);

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    plogk("%s%s", prefix, msg);
}
