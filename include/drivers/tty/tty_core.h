/*
 *
 *      tty_core.h
 *      TTY core definitions
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TTY_CORE_H_
#define INCLUDE_TTY_CORE_H_

#include <kernel/termios.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#ifndef TTY_CORE_BUFFER_SIZE
#    define TTY_CORE_BUFFER_SIZE 4096
#endif

typedef struct tty_core tty_core_t;

typedef int (*tty_core_emit_t)(void *context, const uint8_t *data, size_t size, uint64_t flags);
typedef void (*tty_core_event_t)(void *context, uint8_t event);
typedef void (*tty_core_owner_t)(void *context);

typedef struct tty_core_ops {
        tty_core_emit_t  emit;
        tty_core_event_t event;
        tty_core_owner_t retain;
        tty_core_owner_t release;
} tty_core_ops_t;

struct tty_core {
        spinlock_t     lock;
        wait_queue_t   read_wait;
        wait_queue_t   input_space_wait;
        wait_queue_t   write_wait;
        struct termios termios;
        struct winsize winsize;
        tty_core_ops_t ops;
        void          *context;
        uint8_t        input[TTY_CORE_BUFFER_SIZE];
        uint8_t        input_flags[TTY_CORE_BUFFER_SIZE];
        size_t         input_head;
        size_t         input_tail;
        size_t         input_count;
        size_t         canon_ready;
        size_t         edit_count;
        size_t         eof_count;
        int64_t        session;
        int64_t        foreground_pgid;
        struct vt_mode vt_mode;
        int64_t        vt_owner_pid;
        uint8_t        kd_mode;
        uint8_t        kb_mode;
        uint8_t        led_state;
        bool           is_vt;
        bool           output_stopped;
        bool           hung_up;
};

void    tty_core_init(tty_core_t *tty, const tty_core_ops_t *ops, void *context);
void    tty_core_mark_virtual_console(tty_core_t *tty);
bool    tty_core_graphics_mode(tty_core_t *tty);
uint8_t tty_core_keyboard_mode(tty_core_t *tty);
void    tty_core_retain(tty_core_t *tty);
void    tty_core_release(tty_core_t *tty);
void    tty_core_auto_acquire(tty_core_t *tty, uint64_t flags);
void    tty_core_set_winsize(tty_core_t *tty, uint16_t rows, uint16_t cols);
int64_t tty_core_receive(tty_core_t *tty, const uint8_t *data, size_t size, uint64_t flags);
int64_t tty_core_read(tty_core_t *tty, void *buffer, size_t size, uint64_t flags);
int64_t tty_core_write(tty_core_t *tty, const void *buffer, size_t size, uint64_t flags);
int     tty_core_ioctl(tty_core_t *tty, uint64_t flags, size_t request, void *user_arg);
int     tty_core_ioctl_terminal(tty_core_t *tty, uint64_t flags, size_t request, void *user_arg, bool virtual_console);
int     tty_core_poll(tty_core_t *tty, size_t events);
void    tty_core_hangup(tty_core_t *tty);
void    tty_core_flush_input(tty_core_t *tty);
size_t  tty_core_readable(tty_core_t *tty);

#endif // INCLUDE_TTY_CORE_H_
