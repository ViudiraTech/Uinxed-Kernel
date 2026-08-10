/*
 *
 *      parport.h
 *      Parallel port subsystem (Linux drivers/parport/ analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PARPORT_H_
#define INCLUDE_PARPORT_H_

#include <kernel/debug/ringlog.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

extern log_buffer_t parallel_log;

#define PARPORT_MAX_PORTS 4

/* SPP register offsets */
#define PARPORT_DATA_REG    0
#define PARPORT_STATUS_REG  1
#define PARPORT_CONTROL_REG 2

/* Status signals (interpreted, busy active-high). */
#define PARPORT_BUSY   0x80
#define PARPORT_ACK    0x40
#define PARPORT_PE     0x20
#define PARPORT_SELECT 0x10
#define PARPORT_ERROR  0x08

/* Control signals. */
#define PARPORT_CONTROL_STROBE 0x01
#define PARPORT_CONTROL_AUTOFD 0x02
#define PARPORT_CONTROL_INIT   0x04
#define PARPORT_CONTROL_SELECT 0x08

typedef struct parport parport_t;

typedef uint8_t (*parport_read_data_t)(parport_t *p);
typedef void (*parport_write_data_t)(parport_t *p, uint8_t v);
typedef uint8_t (*parport_read_status_t)(parport_t *p);
typedef uint8_t (*parport_read_control_t)(parport_t *p);
typedef void (*parport_write_control_t)(parport_t *p, uint8_t v);
typedef void (*parport_frob_control_t)(parport_t *p, uint8_t mask, uint8_t v);

struct parport {
        int      number; // parport0, parport1, ...
        char     name[16];
        uint16_t base;         // legacy I/O base address
        int      irq;          // assigned IRQ line, or -1 when polled
        uint32_t dev;          // dev_t (major 99, minor = number)
        bool     claimed;      // PPCLAIM ownership
        bool     exclusive;    // PPEXCL
        uint8_t  control;      // cached control register
        void    *private_data; // bus driver data

        parport_read_data_t     read_data;
        parport_write_data_t    write_data;
        parport_read_status_t   read_status;
        parport_read_control_t  read_control;
        parport_write_control_t write_control;
        parport_frob_control_t  frob_control;

        parport_t *next;
};

/* registry */
int        parport_register_port(const char *name, uint16_t base, int irq, void *private_data);
void       parport_unregister_port(parport_t *p);
int        parport_count(void);
parport_t *parport_get(int index);
parport_t *parport_find(uint16_t base);
parport_t *parport_find_by_number(int number);

/* port access */
uint8_t parport_read_data(parport_t *p);
void    parport_write_data(parport_t *p, uint8_t v);
uint8_t parport_read_status(parport_t *p);
uint8_t parport_read_control(parport_t *p);
void    parport_write_control(parport_t *p, uint8_t v);
void    parport_frob_control(parport_t *p, uint8_t mask, uint8_t v);
void    parport_data_reverse(parport_t *p, bool reverse);

/* drivers/parport/parport_pc.c */
int parport_pc_init(void);

/* drivers/parport/ppdev.c : /dev/parportN */
void ppdev_init(void);

#endif // INCLUDE_PARPORT_H_
