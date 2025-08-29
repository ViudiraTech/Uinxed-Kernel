/*
 *
 *      limine_module.c
 *      Limine loader-provided resource modules
 *
 *      2025/8/2 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "limine_module.h"
#include "limine.h"
#include "printk.h"
#include "string.h"
#include "uinxed.h"

lmodule_t lmodule[128];
static size_t lmodule_count = 0;

/* Extract filename from module path */
static void extract_name(const char *input, char *output, size_t output_size) {
  const char *slash = strrchr(input, '/');
  const char *name = slash ? slash + 1 : input;
  size_t len = 0;

  while (name[len] && name[len] != '.' && len < output_size - 1) {
    output[len] = name[len];
    len++;
  }
  output[len] = '\0';
}

/* Find resource modules by module name */
lmodule_t *get_lmodule(const char *lmodule_name) {
  for (size_t i = 0; i < lmodule_count; i++)
    if (!strcmp(lmodule[i].name, lmodule_name))
      return &lmodule[i];
  return 0;
}

/* Initialize the passed-in resource module list */
void lmodule_init(void) {
  if (!module_request.response || !module_request.response->module_count)
    return;
  for (size_t i = 0; i < module_request.response->module_count; i++) {
    struct limine_file *file = module_request.response->modules[i];
    extract_name(file->path, lmodule[lmodule_count].name, sizeof(char) * 32);
    lmodule[lmodule_count].path = file->path;
    lmodule[lmodule_count].data = file->address;
    lmodule[lmodule_count].size = file->size;
    plogk("mod: %s (path: %s, size: %llu KiB, base %p)\n",
          lmodule[lmodule_count].name, file->path, (file->size / 1024),
          file->address);
    lmodule_count++;
  }
}
