/*
 *
 *      cmdline.c
 *      Kernel command line
 *
 *      2025/3/9 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <kernel/uinxed.h>

/* Get the kernel command line */
const char *get_cmdline(void)
{
    return kernel_file_request.response->kernel_file->cmdline;
}
