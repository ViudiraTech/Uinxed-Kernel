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
#include "cmdline.h"
#include "serial.h"
#include "stdlib.h"
#include "string.h"
#include "video.h"

static char boot_tty_buf[16] = {0}; // Persistent Buffer
static char *boot_tty        = 0;
#define TTY_BUF_SIZE 4096
static char tty_buff[TTY_BUF_SIZE] = {0};
static char *tty_buff_ptr          = tty_buff;

/* Parsing command line arguments */
static int arg_parse(char *arg_str, char **argv, char delim)
{
    int argc = 0;

    while (*arg_str && argc < MAX_ARGC) {
        while (*arg_str == delim) arg_str++;
        if (*arg_str == '\0') break;

        argv[argc++] = arg_str;

        while (*arg_str && *arg_str != delim) arg_str++;
        if (*arg_str) *arg_str++ = '\0'; // Replace delimiter with '\0' (It seems hard to undertand)
    }
    return argc;
}

/* Obtain the tty number provided at startup */
char *get_boot_tty(void)
{
    if (boot_tty) return boot_tty;

    const char *cmdline = get_cmdline();
    if (!cmdline) { return DEFAULT_TTY; }

    char bootarg[MAX_CMDLINE];
    memset(bootarg, 0, MAX_CMDLINE); // This is important
    strncpy(bootarg, cmdline, MAX_CMDLINE);
    bootarg[MAX_CMDLINE - 1] = '\0';

    char **argv = (char **)malloc(MAX_ARGC * sizeof(char *));
    if (!argv) { return DEFAULT_TTY; }

    int argc = arg_parse(bootarg, argv, ' ');
    for (int i = 0; i < argc; ++i) {
        if (strncmp(argv[i], "console=", 8) == 0) {
            const char *tty_str = argv[i] + 8;

            if (strlen(tty_str) < sizeof(boot_tty_buf)) {
                strncpy(boot_tty_buf, tty_str, sizeof(boot_tty_buf));
                boot_tty_buf[sizeof(boot_tty_buf) - 1] = '\0';
                boot_tty                               = boot_tty_buf;
                break;
            }
        }
    }
    free((void *)argv);

    if (!boot_tty) {
        strncpy(boot_tty_buf, DEFAULT_TTY, sizeof(boot_tty_buf));
        boot_tty = boot_tty_buf;
    }
    return boot_tty;
}

void tty_buff_flush()
{
    tty_buff_ptr = tty_buff;
    if (strcmp(get_boot_tty(), "ttyS0") == 0) {
        while (*tty_buff_ptr != '\0') {
            write_serial(*tty_buff_ptr);
            tty_buff_ptr++;
        }
        tty_buff_ptr = tty_buff;
    } else {
        tty_buff[TTY_BUF_SIZE - 1] = '\0';
        video_put_string(tty_buff);
    }
}

static void tty_buff_add(const char ch)
{
    if (ch == '\0') { return; }
    *tty_buff_ptr = ch;
    tty_buff_ptr++;
    if (tty_buff_ptr - tty_buff >= TTY_BUF_SIZE - 1) {
        // Flush
        *tty_buff_ptr = '\0';
        tty_buff_flush();
    }
    *tty_buff_ptr = '\0';
}

/* Print characters to tty */
void tty_print_ch(const char ch)
{
    tty_buff_add(ch);
}

/* Print string to tty */
void tty_print_str(const char *str)
{
    const char *str_clone = str;
    while (*str_clone != '\0') {
        tty_buff_add(*str_clone);
        str_clone++;
    }
}
