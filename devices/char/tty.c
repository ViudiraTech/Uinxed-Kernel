/*
 *
 *      tty.c
 *      Teletype
 *
 *      2025/4/12 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "tty.h"
#include "alloc.h"
#include "cmdline.h"
#include "serial.h"
#include "stdint.h"
#include "string.h"
#include "video.h"

static char boot_tty_buf[16] = {0}; // Persistent Buffer
static char *boot_tty        = 0;

/* Parsing command line arguments */
static int arg_parse(char *arg_str, char **argv, char delim)
{
    int argc = 0;

    while (*arg_str && argc < MAX_ARGC) {
        while (*arg_str == delim) arg_str++;
        if (*arg_str == '\0') break;

        argv[argc++] = arg_str;

        while (*arg_str && *arg_str != delim) arg_str++;
        if (*arg_str) *arg_str++ = '\0';
    }
    return argc;
}

/* Obtain the tty number provided at startup */
char *get_boot_tty(void)
{
    if (boot_tty) return boot_tty;

    const char *cmdline = get_cmdline();
    if (!cmdline) return DEFAULT_TTY;

    char bootarg[MAX_CMDLINE];
    strncpy(bootarg, cmdline, MAX_CMDLINE - 1);
    bootarg[MAX_CMDLINE - 1] = '\0';

    char **argv = malloc(MAX_ARGC * sizeof(char *));
    if (!argv) return DEFAULT_TTY;

    int argc = arg_parse(bootarg, argv, ' ');
    for (int i = 0; i < argc; ++i) {
        if (strncmp(argv[i], "console=", 8) == 0) {
            const char *tty_str = argv[i] + 8;

            if (strlen(tty_str) < sizeof(boot_tty_buf)) {
                strncpy(boot_tty_buf, tty_str, sizeof(boot_tty_buf) - 1);
                boot_tty_buf[sizeof(boot_tty_buf) - 1] = '\0';
                boot_tty                               = boot_tty_buf;
                break;
            }
        }
    }
    free(argv);

    if (!boot_tty) {
        strncpy(boot_tty_buf, DEFAULT_TTY, sizeof(boot_tty_buf));
        boot_tty = boot_tty_buf;
    }
    return boot_tty;
}

/* Print characters to tty */
void tty_print_ch(const char ch)
{
    if (strcmp(get_boot_tty(), "ttyS0") == 0)
        write_serial(ch);
    else
        video_put_string((char[]) {ch, '\0'});
}

/* Print string to tty */
void tty_print_str(const char *str)
{
    if (strcmp(get_boot_tty(), "ttyS0") == 0)
        for (; *str; ++str) write_serial(*str);
    else
        video_put_string(str);
}
