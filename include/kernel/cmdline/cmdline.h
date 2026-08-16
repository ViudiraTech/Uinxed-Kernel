/*
 *
 *      cmdline.h
 *      Kernel command line header file
 *
 *      2025/3/9 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CMDLINE_H_
#define INCLUDE_CMDLINE_H_

#include <libs/std/stddef.h>

/* Get the kernel command line */
const char *get_cmdline(void);

/* Fetch the first `key=value` value of `key` into buf, or NULL when absent. */
const char *cmdline_get_option(const char *key, char *buf, size_t buflen);

/* Call cb(value, ctx) for every `key=value` occurrence of `key`. */
size_t cmdline_foreach_option(const char *key, void (*cb)(const char *value, void *ctx), void *ctx, char *buf, size_t buflen);

#endif // INCLUDE_CMDLINE_H_
