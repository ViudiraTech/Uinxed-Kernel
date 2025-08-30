/*
 *
 *      tty.h
 *      Teletype header file
 *
 *      2025/4/12 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_TTY_H_
#define INCLUDE_TTY_H_

#include "stdint.h"
#include "stdlib.h"

#define MAX_ARGC    1024
#define MAX_CMDLINE 256

#ifndef TTY_BUF_SIZE
#    define TTY_BUF_SIZE 4096
#endif

#ifndef TTY_DEFAULT_DEV
#    define TTY_DEFAULT_DEV "tty0"
#endif

extern Writer tty_writer;

typedef enum {
    TTY_DEVICE_VGA,
    TTY_DEVICE_SERIAL,
} tty_device_kind_t;

typedef enum {
    MET_TYPE,
    MET_PORT,
    MET_NOTHING,
} parse_state_t;

typedef struct {
        tty_device_kind_t type;
        uint32_t          port;
} tty_device_t;

/* Parse boot_tty string to tty_device_t */
tty_device_t parse_boot_tty_str(char *boot_tty_str);

/* Obtain the tty device provided at startup */
tty_device_t *get_boot_tty(void);

/* Print characters to tty */
void tty_print_ch(const char ch);

/* Print string to tty */
void tty_print_str(const char *str);

/* Flush tty buffer */
void tty_buff_flush(void);

#endif // INCLUDE_TTY_H_
