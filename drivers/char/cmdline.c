/*
 *
 *      cmdline.c
 *      Kernel command line
 *
 *      2025/3/9 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "limine.h"
#include "uinxed.h"

/* Get the kernel command line */
const char *get_cmdline(void) {
  return kernel_file_request.response->kernel_file->cmdline;
}
