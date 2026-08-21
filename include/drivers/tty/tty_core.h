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

#include <fs/core/vfs.h>
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
        spinlock_t lock;

        /* Serializes output callbacks without holding the line-discipline lock. */
        spinlock_t        output_lock;
        wait_queue_t      read_wait;
        wait_queue_t      input_space_wait;
        wait_queue_t      write_wait;
        vfs_poll_source_t poll_source; // poll/select/epoll readiness notification
        struct termios    termios;
        struct winsize    winsize;
        tty_core_ops_t    ops;
        void             *context;
        uint8_t           input[TTY_CORE_BUFFER_SIZE];
        uint8_t           input_flags[TTY_CORE_BUFFER_SIZE];
        size_t            input_head;
        size_t            input_tail;
        size_t            input_count;
        size_t            canon_ready;
        size_t            edit_count;
        size_t            eof_count;
        int64_t           session;
        int64_t           foreground_pgid;
        struct vt_mode    vt_mode;
        int64_t           vt_owner_pid;
        uint8_t           kd_mode;
        uint8_t           kb_mode;
        uint8_t           led_state;
        bool              is_vt;
        bool              output_stopped;
        bool              hung_up;
};

/* Initialize a tty core with default termios and the device's ops. */
void tty_core_init(tty_core_t *tty, const tty_core_ops_t *ops, void *context);

/* Mark the tty as a virtual console, enabling VT ioctls. */
void tty_core_mark_virtual_console(tty_core_t *tty);

/* True when this VT is in KD_GRAPHICS mode. */
bool tty_core_graphics_mode(tty_core_t *tty);

/* Return the current keyboard translation mode (K_UNICODE, K_RAW, ...). */
uint8_t tty_core_keyboard_mode(tty_core_t *tty);

/* Take a reference on the underlying device via ops.retain. */
void tty_core_retain(tty_core_t *tty);

/* Drop a reference on the underlying device via ops.release. */
void tty_core_release(tty_core_t *tty);

/* Adopt this tty as the session leader's controlling tty when appropriate. */
void tty_core_auto_acquire(tty_core_t *tty, uint64_t flags);

/* Update the window size and signal SIGWINCH to the foreground group. */
void tty_core_set_winsize(tty_core_t *tty, uint16_t rows, uint16_t cols);

/* Feed received bytes through the line discipline (editing, flow, ISIG). */
int64_t tty_core_receive(tty_core_t *tty, const uint8_t *data, size_t size, uint64_t flags);

/* Read from the input queue, honoring canonical/raw and VMIN/VTIME. */
int64_t tty_core_read(tty_core_t *tty, void *buffer, size_t size, uint64_t flags);

/* Write output through the device emit callback, applying OPOST. */
int64_t tty_core_write(tty_core_t *tty, const void *buffer, size_t size, uint64_t flags);

/* Dispatch terminal ioctls for a tty. */
int tty_core_ioctl(tty_core_t *tty, uint64_t flags, size_t request, void *user_arg);

/* Terminal ioctl dispatch; virtual_console also enables the VT ioctls. */
int tty_core_ioctl_terminal(tty_core_t *tty, uint64_t flags, size_t request, void *user_arg, bool virtual_console);

/* Report poll readiness from the input queue and output state. */
int tty_core_poll(tty_core_t *tty, size_t events);

/* Hang up the tty: detach processes, signal SIGHUP/SIGCONT, wake all. */
void tty_core_hangup(tty_core_t *tty);

/* Flush the input queue and wake all waiters. */
void tty_core_flush_input(tty_core_t *tty);

/* Number of input bytes a read can return right now (FIONREAD). */
size_t tty_core_readable(tty_core_t *tty);

#endif // INCLUDE_TTY_CORE_H_
