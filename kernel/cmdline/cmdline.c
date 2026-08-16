/*
 *
 *      cmdline.c
 *      Kernel command line
 *
 *      2025/3/9 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/uinxed.h>
#include <libs/std/string.h>

/* Get the kernel command line */
const char *get_cmdline(void)
{
    return kernel_file_request.response->kernel_file->cmdline;
}

/* Advance to the next `key=value` occurrence of `key`, writing the value into buf. */
static int cmdline_next_option(const char **p, const char *key, size_t keylen, char *buf, size_t buflen)
{
    while (**p) {
        while (**p == ' ' || **p == '\t') (*p)++;
        if (!**p) break;
        if (!strncmp(*p, key, keylen) && (*p)[keylen] == '=') {
            const char *value = *p + keylen + 1;
            size_t      vlen  = 0;
            size_t      copy;

            while (value[vlen] && value[vlen] != ' ' && value[vlen] != '\t') vlen++;
            *p += keylen + 1 + vlen;

            if (!vlen) continue; /* skip an empty value */

            copy = vlen;
            if (copy >= buflen) copy = buflen - 1;
            memcpy(buf, value, copy);
            buf[copy] = '\0';
            return 1;
        }
        while (**p && **p != ' ' && **p != '\t') (*p)++;
    }
    return 0;
}

/* Call cb(value, ctx) for every `key=value` occurrence of `key`. */
size_t cmdline_foreach_option(const char *key, void (*cb)(const char *value, void *ctx), void *ctx, char *buf, size_t buflen)
{
    const char *p;
    size_t      count = 0;

    if (!key || !key[0] || !cb || !buf || !buflen) return 0;
    p = get_cmdline();

    if (!p) return 0;
    while (cmdline_next_option(&p, key, strlen(key), buf, buflen)) {
        cb(buf, ctx);
        count++;
    }
    return count;
}

/* Fetch the first `key=value` value of `key` into buf, or NULL when absent. */
const char *cmdline_get_option(const char *key, char *buf, size_t buflen)
{
    const char *p;

    if (!key || !key[0] || !buf || !buflen) return NULL;
    p = get_cmdline();

    if (!p) return NULL;
    return cmdline_next_option(&p, key, strlen(key), buf, buflen) ? buf : NULL;
}
