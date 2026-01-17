/*
 *
 *      tty.c
 *      Teletype
 *
 *      2025/4/12 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cmdline.h>
#include <heap.h>
#include <serial.h>
#include <spin_lock.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tty.h>
#include <video.h>

tty_device_t  boot_tty     = {0, 0};
tty_device_t *boot_tty_ptr = 0;

static char           boot_tty_str_buf[16]   = {0}; // Persistent buffer
static char           tty_buff[TTY_BUF_SIZE] = {0};
static volatile char *tty_buff_ptr           = tty_buff;

spinlock_t tty_flush_spinlock = {0};

writer tty_writer = {
    .data    = 0,
    .handler = tty_writer_handler,
};

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

/* Directs character write operations to terminal output */
uint8_t tty_writer_handler(writer *writer, char c)
{
    (void)writer;
    tty_print_ch(c);
    return 1; // Always success? :(
}

/* Parse boot_tty string to tty_device_t */
tty_device_t parse_boot_tty_str(char *boot_tty_str)
{
    tty_device_t  tty_dev = {0, 0};
    parse_state_t state   = MET_NOTHING;

    char   type_str[16] = {0};
    char   port_str[16] = {0};
    char  *type_str_ptr = type_str;
    char  *port_str_ptr = port_str;
    char  *input_ptr    = boot_tty_str;
    char **write_ptr    = 0;

    /* Example: ttyS0, tty0 */
    while (*input_ptr) {
        switch (state) {
            case MET_NOTHING : {
                if (IS_ALPHA(*input_ptr)) {
                    state     = MET_TYPE;
                    write_ptr = &type_str_ptr;
                }
                break;
            }
            case MET_TYPE : {
                if ((uintptr_t)(*write_ptr - type_str) >= sizeof(type_str)) write_ptr = 0;
                write_ptr = &type_str_ptr;
                if (!IS_ALPHA(*input_ptr)) write_ptr = 0;
                if (IS_DIGIT(*input_ptr)) {
                    state     = MET_PORT;
                    write_ptr = &port_str_ptr;
                }
                break;
            }
            case MET_PORT : {
                if ((uintptr_t)(*write_ptr - port_str) >= sizeof(port_str)) write_ptr = 0;
                write_ptr = &port_str_ptr;
                if (!IS_DIGIT(*input_ptr)) write_ptr = 0;
                break;
            }
        }
        if (write_ptr) {
            **write_ptr = *input_ptr;
            (*write_ptr)++;
        }
        input_ptr++;
    }

    if (!strcmp(type_str, "tty")) {
        tty_dev.type = TTY_DEVICE_VGA;
    } else if (!strcmp(type_str, "ttyS")) {
        tty_dev.type = TTY_DEVICE_SERIAL;
    }
    tty_dev.port = atoi(port_str); // NOLINT(cert-err34-c)
    return tty_dev;
}

/* Obtain the tty device provided at startup */
tty_device_t *get_boot_tty(void)
{
    if (boot_tty_ptr) return boot_tty_ptr;

    /* Parse default boot_tty string */
    char *boot_tty_str = TTY_DEFAULT_DEV;
    boot_tty           = parse_boot_tty_str(boot_tty_str);
    boot_tty_ptr       = &boot_tty;

    const char *cmdline = get_cmdline();
    if (!cmdline) return boot_tty_ptr;

    char bootarg[MAX_CMDLINE];
    memset(bootarg, 0, MAX_CMDLINE); // This is important
    strncpy(bootarg, cmdline, MAX_CMDLINE);
    bootarg[MAX_CMDLINE - 1] = '\0';

    char **argv = (char **)malloc(MAX_ARGC * sizeof(char *));
    if (!argv) return boot_tty_ptr;

    int argc = arg_parse(bootarg, argv, ' ');
    for (int i = 0; i < argc; ++i) {
        if (!strncmp(argv[i], "console=", 8)) {
            const char *tty_str = argv[i] + 8;

            if (strlen(tty_str) < sizeof(boot_tty_str_buf)) {
                strncpy(boot_tty_str_buf, tty_str, sizeof(boot_tty_str_buf));
                boot_tty_str_buf[sizeof(boot_tty_str_buf) - 1] = '\0';
                boot_tty_str                                   = boot_tty_str_buf;

                /* Parse boot_tty string */
                tty_device_t tmp_tty = parse_boot_tty_str(boot_tty_str);

                /* Determine the legality of the device */
                int valid = 0;
                if (tmp_tty.type == TTY_DEVICE_VGA) {
                    if (!tmp_tty.port) valid = 1;
                } else if (tmp_tty.type == TTY_DEVICE_SERIAL) {
                    if (tmp_tty.port <= 3) valid = 1;
                }

                if (valid) {
                    boot_tty     = tmp_tty;
                    boot_tty_ptr = &boot_tty;
                } else {
                    /* If it is illegal, it will fall back to the default device. */
                    boot_tty     = parse_boot_tty_str(TTY_DEFAULT_DEV);
                    boot_tty_ptr = &boot_tty;
                }
                break;
            }
        }
    }
    free((void *)argv);
    return boot_tty_ptr;
}

/* Output the buffer data to the specified device according to the configuration */
void tty_buff_flush(void)
{
    uint64_t flags            = spin_lock(&tty_flush_spinlock);
    tty_buff_ptr              = tty_buff;
    tty_device_t *tty_device  = get_boot_tty();
    uint16_t      serial_port = 0;
    uint8_t       early_break = 0;

    for (int attempt = 0; attempt < 2 && !early_break; ++attempt) {
        early_break = 1;
        switch (tty_device->type) {
            case TTY_DEVICE_VGA :
                if (tty_device->port == 0) {
                    tty_buff[TTY_BUF_SIZE - 1] = '\0';
                    video_put_string(tty_buff);
                } else {
                    /* Bad port number */
                    early_break = 0;
                    boot_tty    = parse_boot_tty_str(TTY_DEFAULT_DEV);
                    tty_device  = &boot_tty;
                    continue;
                }
                break;
            case TTY_DEVICE_SERIAL :
                switch (tty_device->port) {
                    case 0 :
                        serial_port = SERIAL_PORT_1;
                        break;
                    case 1 :
                        serial_port = SERIAL_PORT_2;
                        break;
                    case 2 :
                        serial_port = SERIAL_PORT_3;
                        break;
                    case 3 :
                        serial_port = SERIAL_PORT_4;
                        break;
                    default :
                        /* Bad port number */
                        early_break = 0;
                        boot_tty    = parse_boot_tty_str(TTY_DEFAULT_DEV);
                        tty_device  = &boot_tty;
                        continue;
                        break;
                }
                while (*tty_buff_ptr != '\0') write_serial(serial_port, *tty_buff_ptr++);
                break;
            default :
                /* Unreachable */
                break;
        }
    }
    tty_buff_ptr = tty_buff;
    tty_buff[0]  = '\0';
    spin_unlock(&tty_flush_spinlock, flags);
}

/* Add character data to the teletype buffer */
static void tty_buff_add(const char ch)
{
    if (ch == '\0') return;
    *tty_buff_ptr++ = ch;

    if (tty_buff_ptr - tty_buff >= TTY_BUF_SIZE - 1 || ch == '\n') {
        /* Flush */
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
