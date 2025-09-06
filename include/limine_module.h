/*
 *
 *      limine_module.h
 *      Limine loader-provided resource modules header file
 *
 *      2025/8/2 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_LIMINE_MODULE_H_
#define INCLUDE_LIMINE_MODULE_H_

#include "stddef.h"
#include "stdint.h"

typedef struct {
        char     name[32];
        char    *path;
        uint8_t *data;
        size_t   size;
} lmodule_t;

/* Find resource modules by module name */
lmodule_t *get_lmodule(const char *lmodule_name);

/* Initialize the passed-in resource module list */
void lmodule_init(void);

#endif // INCLUDE_LIMINE_MODULE_H_
