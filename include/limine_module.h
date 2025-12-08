/*
 *
 *      limine_module.h
 *      Limine loader-provided resource modules header file
 *
 *      2025/8/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_LIMINE_MODULE_H_
#define INCLUDE_LIMINE_MODULE_H_

#include <ringlog.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
        char     name[32];
        char    *path;
        uint8_t *data;
        size_t   size;
} lmodule_t;

extern log_buffer_t lmodule_log;

/* Find resource modules by module name */
lmodule_t *get_lmodule(const char *lmodule_name);

/* Initialize the passed-in resource module list */
void lmodule_init(void);

#endif // INCLUDE_LIMINE_MODULE_H_
