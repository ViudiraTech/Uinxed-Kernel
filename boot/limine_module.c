/*
 *
 *      limine_module.c
 *      Limine loader-provided resource modules
 *
 *      2025/8/2 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <boot/limine_module.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/string.h>

lmodule_t     lmodule[128];
log_buffer_t  lmodule_log;
static size_t lmodule_count = 0;

/* Extract filename from module path */
static void extract_name(const char *input, char *output, size_t output_size)
{
    if (!input || !output || !output_size) return;

    const char *slash = strrchr(input, '/');
    const char *name  = slash ? slash + 1 : input;
    size_t      len   = 0;

    while (name[len] && name[len] != '.' && len < output_size - 1) {
        output[len] = name[len];
        len++;
    }
    output[len] = '\0';
}

/* Find resource modules by module name */
lmodule_t *get_lmodule(const char *lmodule_name)
{
    if (!lmodule_name) return 0;
    for (size_t i = 0; i < lmodule_count; i++)
        if (!strcmp(lmodule[i].name, lmodule_name)) return &lmodule[i];
    return 0;
}

/* Initialize the passed-in resource module list */
void lmodule_init(void)
{
    if (!module_request.response || !module_request.response->module_count) return;
    for (size_t i = 0; i < module_request.response->module_count && lmodule_count < 128; i++) {
        struct limine_file *file = module_request.response->modules[i];
        extract_name(file->path, lmodule[lmodule_count].name, sizeof(char) * 32);
        lmodule[lmodule_count].path = file->path;
        lmodule[lmodule_count].data = file->address;
        lmodule[lmodule_count].size = file->size;
        log_buffer_write(&lmodule_log, "mod: %s (path: %s, size: %llu KiB, base %p)\n", lmodule[lmodule_count].name, file->path,
                         (file->size / 1024), file->address);
        lmodule_count++;
    }
}
