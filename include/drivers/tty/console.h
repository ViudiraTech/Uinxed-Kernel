/*
 *
 *      console.h
 *      Console driver table (Linux kernel/printk/console_cmdline + console_drivers analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CONSOLE_H_
#define INCLUDE_CONSOLE_H_

#include <drivers/tty/tty.h>
#include <drivers/tty/tty_core.h>
#include <libs/std/stdint.h>

#define CON_ENABLED (1U << 0) // receives console output
#define CON_CONSDEV (1U << 1) // backs /dev/console

typedef struct console console_t;

typedef void (*console_write_t)(console_t *c, const uint8_t *buf, size_t len);
typedef tty_core_t *(*console_get_tty_t)(console_t *c);

struct console {
        const char       *name;  // "tty", "ttyD", "ttyS"
        uint32_t          index; // 0 for the vt console, port for serial
        uint32_t          flags;
        void             *data; // driver-private (e.g. uart_port *)
        console_write_t   write;
        console_get_tty_t get_tty; // line discipline for /dev/console
        console_t        *next;
};

/* Parsed `console=` command-line entries. */
#define NR_CONSOLES 4

typedef struct console_cmdline {
        char name[16];
        int  index;
        char options[32]; // e.g. "115200n8"
} console_cmdline_t;

extern console_cmdline_t console_cmdline[NR_CONSOLES];
extern int               console_cmdline_count;

/* Parse every `console=` argument from the kernel command line. */
void console_cmdline_parse(void);

/* Driver-side registration. */
int  register_console(console_t *c);
void unregister_console(console_t *c);

/* Emit a buffer to every enabled console. */
void console_write_all(const uint8_t *buf, size_t len);

/* The console backing /dev/console (CON_CONSDEV, or the default). */
console_t *console_get_active(void);

/* Line discipline backing /dev/console. */
tty_core_t *console_get_tty(void);

/* Derive the legacy {type, port} descriptor used by get_boot_tty(). */
tty_device_t console_derive_boot_tty(void);

#endif // INCLUDE_CONSOLE_H_
