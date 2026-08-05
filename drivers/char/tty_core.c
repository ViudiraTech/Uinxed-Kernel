/*
 *
 *      tty_core.c
 *      TTY core implementation
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/char/tty_core.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/uaccess.h>
#include <sync/signal.h>
#include <syscall/fcntl.h>

#define POLLIN                   0x001
#define POLLOUT                  0x004
#define POLLHUP                  0x010
#define TTY_TICKS_PER_DECISECOND 25
#define TTY_INPUT_EOF            1

static void tty_default_termios(struct termios *termios)
{
    memset(termios, 0, sizeof(*termios));
    termios->c_iflag      = ICRNL | IXON;
    termios->c_oflag      = OPOST | ONLCR;
    termios->c_cflag      = B38400 | CS8 | CREAD;
    termios->c_lflag      = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;
    termios->c_cc[VINTR]  = 3;
    termios->c_cc[VQUIT]  = 28;
    termios->c_cc[VERASE] = 127;
    termios->c_cc[VKILL]  = 21;
    termios->c_cc[VEOF]   = 4;
    termios->c_cc[VMIN]   = 1;
    termios->c_cc[VSTART] = 17;
    termios->c_cc[VSTOP]  = 19;
    termios->c_cc[VSUSP]  = 26;
}

void tty_core_init(tty_core_t *tty, const tty_core_ops_t *ops, void *context)
{
    memset(tty, 0, sizeof(*tty));
    wait_queue_init(&tty->read_wait);
    wait_queue_init(&tty->input_space_wait);
    wait_queue_init(&tty->write_wait);
    tty_default_termios(&tty->termios);
    tty->winsize.ws_row = 25;
    tty->winsize.ws_col = 80;
    tty->vt_mode.mode   = VT_AUTO;
    tty->kd_mode        = KD_TEXT;
    tty->kb_mode        = K_UNICODE;
    if (ops) tty->ops = *ops;
    tty->context = context;
}

void tty_core_mark_virtual_console(tty_core_t *tty)
{
    if (!tty) return;
    spin_lock(&tty->lock);
    tty->is_vt = true;
    spin_unlock(&tty->lock);
}

bool tty_core_graphics_mode(tty_core_t *tty)
{
    if (!tty) return false;
    spin_lock(&tty->lock);
    bool graphics = tty->is_vt && tty->kd_mode == KD_GRAPHICS;
    spin_unlock(&tty->lock);
    return graphics;
}

uint8_t tty_core_keyboard_mode(tty_core_t *tty)
{
    if (!tty) return K_OFF;
    spin_lock(&tty->lock);
    uint8_t mode = tty->kb_mode;
    spin_unlock(&tty->lock);
    return mode;
}

void tty_core_retain(tty_core_t *tty)
{
    if (tty && tty->ops.retain) tty->ops.retain(tty->context);
}

void tty_core_release(tty_core_t *tty)
{
    if (tty && tty->ops.release) tty->ops.release(tty->context);
}

static bool tty_current_associated(tty_core_t *tty, process_t *current)
{
    tty_core_t *ctty  = process_ctty_get(current);
    bool        match = ctty == tty;
    if (ctty) tty_core_release(ctty);
    return match;
}

static int tty_job_control_check(tty_core_t *tty, int signal)
{
    process_t *current = process_current();
    if (!current || current->pgid <= 0 || !tty_current_associated(tty, current)) return 0;

    spin_lock(&tty->lock);
    bool background = tty->session == current->sid && tty->foreground_pgid != current->pgid;
    spin_unlock(&tty->lock);
    if (!background) return 0;

    bool blocked_or_ignored = signal_is_blocked_or_ignored(current, signal);
    if (signal == SIGTTOU && blocked_or_ignored) return 0;
    if (blocked_or_ignored || process_pgrp_is_orphaned(current->pgid, current->sid)) return -EIO;
    signal_send_pgrp_session(current->pgid, current->sid, signal);
    return -ERESTARTSYS;
}

static bool tty_signal_pending(process_t *current)
{
    if (!current) return false;
    spin_lock(&current->signal.lock);
    bool pending = signal_has_pending(&current->signal);
    spin_unlock(&current->signal.lock);
    return pending;
}

static bool tty_prepare_interruptible_wait(tty_core_t *tty, wait_queue_t *queue)
{
    process_t *current = process_current();
    if (!current) {
        spin_unlock(&tty->lock);
        return false;
    }

    spin_lock(&current->signal.lock);
    if (signal_has_pending(&current->signal)) {
        spin_unlock(&current->signal.lock);
        spin_unlock(&tty->lock);
        return false;
    }
    wait_queue_prepare(queue);
    spin_unlock(&current->signal.lock);
    spin_unlock(&tty->lock);
    return true;
}

void tty_core_auto_acquire(tty_core_t *tty, uint64_t flags)
{
    process_t *current = process_current();
    if (!tty || !current || !current->task || (flags & (O_NOCTTY | O_PATH)) || (flags & O_ACCMODE) == O_WRONLY || current->sid <= 0
        || current->sid != (pid_t)current->task->pid) {
        return;
    }

    tty_core_t *ctty = process_ctty_get(current);
    if (ctty) {
        tty_core_release(ctty);
        return;
    }

    process_ctty_acquire(current, tty, false, NULL, NULL);
}

void tty_core_set_winsize(tty_core_t *tty, uint16_t rows, uint16_t cols)
{
    if (!tty) return;
    spin_lock(&tty->lock);
    bool changed            = tty->winsize.ws_row != rows || tty->winsize.ws_col != cols;
    tty->winsize.ws_row     = rows;
    tty->winsize.ws_col     = cols;
    int64_t foreground_pgid = tty->foreground_pgid;
    int64_t session         = tty->session;
    spin_unlock(&tty->lock);
    if (changed && foreground_pgid > 0 && session > 0) signal_send_pgrp_session(foreground_pgid, session, SIGWINCH);
}

static bool tty_cc_matches(const struct termios *termios, size_t index, uint8_t ch)
{
    return termios->c_cc[index] != 0 && ch == termios->c_cc[index];
}

static bool tty_is_delimiter(const struct termios *termios, uint8_t ch)
{
    return ch == '\n' || tty_cc_matches(termios, VEOL, ch) || tty_cc_matches(termios, VEOL2, ch);
}

static void tty_echo(tty_core_t *tty, uint8_t ch, tcflag_t lflag)
{
    uint8_t out[2];
    size_t  count = 1;

    if (!tty->ops.emit) return;
    out[0] = ch;
    if ((lflag & ECHOCTL) && ch < 32 && ch != '\n' && ch != '\t') {
        out[0] = '^';
        out[1] = (uint8_t)(ch + '@');
        count  = 2;
    }
    tty->ops.emit(tty->context, out, count, O_NONBLOCK);
}

static void tty_echo_erase(tty_core_t *tty)
{
    static const uint8_t erase[] = {'\b', ' ', '\b'};
    if (tty->ops.emit) tty->ops.emit(tty->context, erase, sizeof(erase), O_NONBLOCK);
}

static void tty_flush_input_locked(tty_core_t *tty)
{
    tty->input_head = tty->input_tail = tty->input_count = 0;
    tty->canon_ready = tty->edit_count = tty->eof_count = 0;
}

void tty_core_flush_input(tty_core_t *tty)
{
    spin_lock(&tty->lock);
    tty_flush_input_locked(tty);
    spin_unlock(&tty->lock);
    wait_queue_wake_all(&tty->input_space_wait);
    wait_queue_wake_all(&tty->read_wait);
}

static bool tty_put_locked(tty_core_t *tty, uint8_t ch, uint8_t marker)
{
    if (tty->input_count == TTY_CORE_BUFFER_SIZE) return false;
    tty->input[tty->input_head]       = ch;
    tty->input_flags[tty->input_head] = marker;
    if (marker) tty->eof_count++;
    tty->input_head = (tty->input_head + 1) % TTY_CORE_BUFFER_SIZE;
    tty->input_count++;
    return true;
}

static void tty_signal_foreground(tty_core_t *tty, int signal)
{
    spin_lock(&tty->lock);
    int64_t pgid = tty->foreground_pgid;
    int64_t sid  = tty->session;
    spin_unlock(&tty->lock);
    if (pgid > 0 && sid > 0) signal_send_pgrp_session(pgid, sid, signal);
}

int64_t tty_core_receive(tty_core_t *tty, const uint8_t *data, size_t size, uint64_t flags)
{
    size_t accepted = 0;

    for (size_t i = 0; i < size; i++) {
retry_character:;
        uint8_t ch = data[i];
        spin_lock(&tty->lock);
        if (tty->termios.c_iflag & ISTRIP) ch &= 0x7f;
        if (ch == '\r') {
            if (tty->termios.c_iflag & IGNCR) {
                spin_unlock(&tty->lock);
                accepted++;
                continue;
            }
            if (tty->termios.c_iflag & ICRNL) ch = '\n';
        } else if (ch == '\n' && (tty->termios.c_iflag & INLCR)) {
            ch = '\r';
        }

        if ((tty->termios.c_iflag & IXON) && tty_cc_matches(&tty->termios, VSTOP, ch)) {
            tty->output_stopped = true;
            spin_unlock(&tty->lock);
            if (tty->ops.event) tty->ops.event(tty->context, TIOCPKT_STOP);
            accepted++;
            continue;
        }
        if ((tty->termios.c_iflag & IXON) && tty_cc_matches(&tty->termios, VSTART, ch)) {
            tty->output_stopped = false;
            spin_unlock(&tty->lock);
            if (tty->ops.event) tty->ops.event(tty->context, TIOCPKT_START);
            wait_queue_wake_all(&tty->write_wait);
            accepted++;
            continue;
        }
        if ((tty->termios.c_iflag & (IXON | IXANY)) == (IXON | IXANY) && tty->output_stopped) {
            tty->output_stopped = false;
            spin_unlock(&tty->lock);
            if (tty->ops.event) tty->ops.event(tty->context, TIOCPKT_START);
            wait_queue_wake_all(&tty->write_wait);
            goto retry_character;
        }

        if (tty->termios.c_lflag & ISIG) {
            int signal = tty_cc_matches(&tty->termios, VINTR, ch) ? SIGINT :
                         tty_cc_matches(&tty->termios, VQUIT, ch) ? SIGQUIT :
                         tty_cc_matches(&tty->termios, VSUSP, ch) ? SIGTSTP :
                                                                    0;
            if (signal) {
                tcflag_t lflag = tty->termios.c_lflag;
                bool     flush = !(lflag & NOFLSH);
                if (flush) tty_flush_input_locked(tty);
                spin_unlock(&tty->lock);
                if (lflag & ECHO) tty_echo(tty, ch, lflag);
                if (flush && tty->ops.event) tty->ops.event(tty->context, TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE);
                tty_signal_foreground(tty, signal);
                wait_queue_wake_all(&tty->read_wait);
                if (flush) wait_queue_wake_all(&tty->input_space_wait);
                accepted++;
                continue;
            }
        }

        if (tty->termios.c_lflag & ICANON) {
            if (tty_cc_matches(&tty->termios, VERASE, ch)) {
                tcflag_t lflag  = tty->termios.c_lflag;
                bool     erased = false;
                if (tty->edit_count) {
                    tty->input_head = (tty->input_head + TTY_CORE_BUFFER_SIZE - 1) % TTY_CORE_BUFFER_SIZE;
                    tty->input_count--;
                    tty->edit_count--;
                    erased = true;
                }
                spin_unlock(&tty->lock);
                if (erased) {
                    if (lflag & ECHOE)
                        tty_echo_erase(tty);
                    else if (lflag & ECHO)
                        tty_echo(tty, ch, lflag);
                    wait_queue_wake_all(&tty->input_space_wait);
                }
                accepted++;
                continue;
            }
            if (tty_cc_matches(&tty->termios, VKILL, ch)) {
                tcflag_t lflag  = tty->termios.c_lflag;
                size_t   erased = tty->edit_count;
                while (tty->edit_count) {
                    tty->input_head = (tty->input_head + TTY_CORE_BUFFER_SIZE - 1) % TTY_CORE_BUFFER_SIZE;
                    tty->input_count--;
                    tty->edit_count--;
                }
                spin_unlock(&tty->lock);
                if (lflag & ECHO) {
                    if ((lflag & (ECHOKE | ECHOE)) == (ECHOKE | ECHOE)) {
                        for (size_t i = 0; i < erased; i++) tty_echo_erase(tty);
                    } else {
                        tty_echo(tty, ch, lflag);
                        if (lflag & ECHOK) tty_echo(tty, '\n', lflag);
                    }
                }
                if (erased) wait_queue_wake_all(&tty->input_space_wait);
                accepted++;
                continue;
            }
            if (tty_cc_matches(&tty->termios, VEOF, ch)) {
                if (tty_put_locked(tty, 0, TTY_INPUT_EOF)) {
                    tty->canon_ready += tty->edit_count + 1;
                    tty->edit_count = 0;
                } else {
                    if (!(flags & O_NONBLOCK)) {
                        if (!tty_prepare_interruptible_wait(tty, &tty->input_space_wait)) return accepted ? (int64_t)accepted : -ERESTARTSYS;
                        wait_queue_sleep();
                        if (tty_signal_pending(process_current())) return accepted ? (int64_t)accepted : -ERESTARTSYS;
                        goto retry_character;
                    }
                    spin_unlock(&tty->lock);
                    return accepted ? (int64_t)accepted : -EAGAIN;
                }
                spin_unlock(&tty->lock);
                wait_queue_wake_all(&tty->read_wait);
                accepted++;
                continue;
            }
        }

        bool delimiter = tty_is_delimiter(&tty->termios, ch);
        if ((tty->termios.c_lflag & ICANON) && !delimiter && tty->input_count >= TTY_CORE_BUFFER_SIZE - 1) {
            spin_unlock(&tty->lock);
            accepted++;
            continue;
        }
        if (tty_put_locked(tty, ch, 0)) {
            tcflag_t lflag = tty->termios.c_lflag;
            accepted++;
            if (lflag & ICANON) {
                tty->edit_count++;
                if (delimiter) {
                    tty->canon_ready += tty->edit_count;
                    tty->edit_count = 0;
                }
            }
            bool wake = !(lflag & ICANON) || delimiter;
            spin_unlock(&tty->lock);
            if ((lflag & ECHO) || (ch == '\n' && (lflag & ECHONL))) tty_echo(tty, ch, lflag);
            if (wake) wait_queue_wake_all(&tty->read_wait);
        } else {
            if (!(flags & O_NONBLOCK)) {
                if (!tty_prepare_interruptible_wait(tty, &tty->input_space_wait)) return accepted ? (int64_t)accepted : -ERESTARTSYS;
                wait_queue_sleep();
                if (tty_signal_pending(process_current())) return accepted ? (int64_t)accepted : -ERESTARTSYS;
                goto retry_character;
            }
            spin_unlock(&tty->lock);
            return accepted ? (int64_t)accepted : -EAGAIN;
        }
    }
    return (int64_t)accepted;
}

static bool tty_read_ready_locked(tty_core_t *tty, size_t target)
{
    if (tty->hung_up) return tty->input_count != 0;
    if (tty->termios.c_lflag & ICANON) return tty->canon_ready != 0;
    if (!target) return tty->input_count != 0;
    return tty->input_count >= target;
}

static bool tty_wait(tty_core_t *tty, uint64_t deadline)
{
    if (!tty_prepare_interruptible_wait(tty, &tty->read_wait)) return false;
    if (deadline)
        wait_queue_wait_timed(&tty->read_wait, deadline);
    else
        wait_queue_sleep();
    return !tty_signal_pending(process_current());
}

int64_t tty_core_read(tty_core_t *tty, void *buffer, size_t size, uint64_t flags)
{
    uint8_t *out    = buffer;
    size_t   copied = 0;
    size_t   minimum;
    uint8_t  vtime;
    uint64_t deadline = 0;
    size_t   observed = 0;

    if (!size) return 0;
    int job_control = tty_job_control_check(tty, SIGTTIN);
    if (job_control) return job_control;
    spin_lock(&tty->lock);
    minimum = tty->termios.c_lflag & ICANON ? 1 : tty->termios.c_cc[VMIN];
    if (minimum > size) minimum = size;
    vtime = tty->termios.c_cc[VTIME];

    if (!(tty->termios.c_lflag & ICANON) && !minimum && !vtime && !tty->input_count) {
        spin_unlock(&tty->lock);
        return 0;
    }

    for (;;) {
        bool ready = tty_read_ready_locked(tty, minimum);
        if ((flags & O_NONBLOCK) && !(tty->termios.c_lflag & ICANON) && tty->input_count) ready = true;
        if (!(tty->termios.c_lflag & ICANON) && !minimum && tty->input_count) ready = true;
        if (ready || (tty->hung_up && !tty->input_count)) break;
        if (flags & O_NONBLOCK) {
            spin_unlock(&tty->lock);
            return -EAGAIN;
        }
        if (!(tty->termios.c_lflag & ICANON) && vtime) {
            if (!minimum) {
                if (!deadline) deadline = sched_ticks() + (uint64_t)vtime * TTY_TICKS_PER_DECISECOND;
            } else if (tty->input_count) {
                if (tty->input_count != observed) {
                    observed = tty->input_count;
                    deadline = sched_ticks() + (uint64_t)vtime * TTY_TICKS_PER_DECISECOND;
                }
            }
            if (deadline && sched_ticks() >= deadline) break;
        }
        if (!tty_wait(tty, deadline)) return copied ? (int64_t)copied : -ERESTARTSYS;
        spin_lock(&tty->lock);
    }

    if (!(tty->termios.c_lflag & ICANON) && !minimum && !tty->input_count) {
        spin_unlock(&tty->lock);
        return 0;
    }
    while (copied < size && tty->input_count) {
        uint8_t ch      = tty->input[tty->input_tail];
        uint8_t marker  = tty->input_flags[tty->input_tail];
        tty->input_tail = (tty->input_tail + 1) % TTY_CORE_BUFFER_SIZE;
        tty->input_count--;
        if ((tty->termios.c_lflag & ICANON) && tty->canon_ready) tty->canon_ready--;
        if ((tty->termios.c_lflag & ICANON) && marker == TTY_INPUT_EOF) {
            tty->eof_count--;
            break;
        }
        out[copied++] = ch;
        if ((tty->termios.c_lflag & ICANON) && tty_is_delimiter(&tty->termios, ch)) break;
    }
    spin_unlock(&tty->lock);
    wait_queue_wake_all(&tty->input_space_wait);
    return (int64_t)copied;
}

int64_t tty_core_write(tty_core_t *tty, const void *buffer, size_t size, uint64_t flags)
{
    const uint8_t *input = buffer;
    size_t         done  = 0;

    if (!tty->ops.emit) {
        plogk("tty: write to a tty without an output backend\n");
        return -EIO;
    }
    if (!size) return 0;
    spin_lock(&tty->lock);
    bool tostop = (tty->termios.c_lflag & TOSTOP) != 0;
    spin_unlock(&tty->lock);
    if (tostop) {
        int job_control = tty_job_control_check(tty, SIGTTOU);
        if (job_control) return job_control;
    }
    while (done < size) {
        uint8_t out[2];
        size_t  count = 1;
        spin_lock(&tty->lock);
        while (tty->output_stopped && !tty->hung_up) {
            if (flags & O_NONBLOCK) {
                spin_unlock(&tty->lock);
                return done ? (int64_t)done : -EAGAIN;
            }
            if (!tty_prepare_interruptible_wait(tty, &tty->write_wait)) return done ? (int64_t)done : -ERESTARTSYS;
            wait_queue_sleep();
            if (tty_signal_pending(process_current())) return done ? (int64_t)done : -ERESTARTSYS;
            spin_lock(&tty->lock);
        }
        if (tty->hung_up) {
            spin_unlock(&tty->lock);
            return done ? (int64_t)done : -EIO;
        }
        tcflag_t oflag = tty->termios.c_oflag;
        spin_unlock(&tty->lock);
        out[0] = input[done];
        if ((oflag & (OPOST | ONLCR)) == (OPOST | ONLCR) && out[0] == '\n') {
            out[0] = '\r';
            out[1] = '\n';
            count  = 2;
        }
        int emitted = tty->ops.emit(tty->context, out, count, flags);
        if (emitted < 0) { return done ? (int64_t)done : emitted; }
        if (emitted < (int)count) break;
        done++;
    }
    return (int64_t)done;
}

size_t tty_core_readable(tty_core_t *tty)
{
    spin_lock(&tty->lock);
    size_t value = tty->termios.c_lflag & ICANON ? tty->canon_ready : tty->input_count;
    if ((tty->termios.c_lflag & ICANON) && tty->eof_count && value >= tty->eof_count) value -= tty->eof_count;
    spin_unlock(&tty->lock);
    return value;
}

static bool tty_is_vt_ioctl(size_t request)
{
    return (request >= KDGETLED && request <= KDSKBMODE) || (request >= VT_OPENQRY && request <= VT_DISALLOCATE);
}

int tty_core_ioctl_terminal(tty_core_t *tty, uint64_t flags, size_t request, void *user_arg, bool virtual_console)
{
    struct termios termios;
    struct winsize winsize;
    process_t     *current = process_current();
    int            value;

    if (!tty) return -EINVAL;
    if (tty_is_vt_ioctl(request) && (!tty->is_vt || !virtual_console)) return -ENOTTY;

    switch (request) {
        case TCSETS :
        case TCSETSW :
        case TCSETSF :
        case TCSETS2 :
        case TCSETSW2 :
        case TCSETSF2 :
        case TIOCSWINSZ :
        case TCFLSH :
        case TCXONC :
        case TIOCSETD :
        case TIOCSPGRP : {
            int job_control = tty_job_control_check(tty, SIGTTOU);
            if (job_control) return job_control;
            break;
        }
        default :
            break;
    }

    switch (request) {
        case TCGETS :
            spin_lock(&tty->lock);
            termios = tty->termios;
            spin_unlock(&tty->lock);
            return copy_to_user(user_arg, &termios, sizeof(termios)) ? -EFAULT : 0;
        case TCSETS :
        case TCSETSW :
        case TCSETSF :
            if (copy_from_user(&termios, user_arg, sizeof(termios))) return -EFAULT;
            if (termios.c_line != N_TTY) return -EINVAL;
            spin_lock(&tty->lock);
            bool mode_changed = (tty->termios.c_lflag ^ termios.c_lflag) & ICANON;
            if (request == TCSETSF || mode_changed) tty_flush_input_locked(tty);
            bool old_ixon = (tty->termios.c_iflag & IXON) != 0;
            bool restart  = old_ixon && !(termios.c_iflag & IXON) && tty->output_stopped;
            if (restart) tty->output_stopped = false;
            tty->termios = termios;
            spin_unlock(&tty->lock);
            if (request == TCSETSF || mode_changed) wait_queue_wake_all(&tty->input_space_wait);
            if (restart) wait_queue_wake_all(&tty->write_wait);
            if (tty->ops.event) {
                if (restart) tty->ops.event(tty->context, TIOCPKT_START);
                if (old_ixon != ((termios.c_iflag & IXON) != 0)) tty->ops.event(tty->context, old_ixon ? TIOCPKT_NOSTOP : TIOCPKT_DOSTOP);
                tty->ops.event(tty->context, TIOCPKT_IOCTL);
            }
            wait_queue_wake_all(&tty->read_wait);
            return 0;
        case TIOCGWINSZ :
            spin_lock(&tty->lock);
            winsize = tty->winsize;
            spin_unlock(&tty->lock);
            return copy_to_user(user_arg, &winsize, sizeof(winsize)) ? -EFAULT : 0;
        case KDGETMODE :
            spin_lock(&tty->lock);
            value = tty->kd_mode;
            spin_unlock(&tty->lock);
            return copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0;
        case KDSETMODE :
            value = (int)(uintptr_t)user_arg;
            if (value != KD_TEXT && value != KD_GRAPHICS) return -EINVAL;
            spin_lock(&tty->lock);
            tty->kd_mode = (uint8_t)value;
            spin_unlock(&tty->lock);
            return 0;
        case KDGKBMODE :
            spin_lock(&tty->lock);
            value = tty->kb_mode;
            spin_unlock(&tty->lock);
            return copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0;
        case KDSKBMODE :
            value = (int)(uintptr_t)user_arg;
            if (value < K_RAW || value > K_OFF) return -EINVAL;
            spin_lock(&tty->lock);
            tty->kb_mode = (uint8_t)value;
            spin_unlock(&tty->lock);
            return 0;
        case KDGETLED : {
            spin_lock(&tty->lock);
            uint8_t leds = tty->led_state;
            spin_unlock(&tty->lock);
            return copy_to_user(user_arg, &leds, sizeof(leds)) ? -EFAULT : 0;
        }
        case KDSETLED :
            value = (int)(uintptr_t)user_arg;
            if (value != 0xff && (value < 0 || value > 7)) return -EINVAL;
            spin_lock(&tty->lock);
            tty->led_state = (uint8_t)value;
            spin_unlock(&tty->lock);
            return 0;
        case KDGKBTYPE : {
            uint8_t keyboard_type = KB_101;
            return copy_to_user(user_arg, &keyboard_type, sizeof(keyboard_type)) ? -EFAULT : 0;
        }
        case VT_GETMODE : {
            spin_lock(&tty->lock);
            struct vt_mode mode = tty->vt_mode;
            spin_unlock(&tty->lock);
            return copy_to_user(user_arg, &mode, sizeof(mode)) ? -EFAULT : 0;
        }
        case VT_SETMODE : {
            struct vt_mode mode;
            if (copy_from_user(&mode, user_arg, sizeof(mode))) return -EFAULT;
            if (mode.mode != VT_AUTO && mode.mode != VT_PROCESS) return -EINVAL;
            if (mode.mode == VT_PROCESS
                && (mode.relsig <= 0 || mode.relsig > SIGRTMAX || mode.acqsig <= 0 || mode.acqsig > SIGRTMAX || mode.frsig < 0
                    || mode.frsig > SIGRTMAX))
                return -EINVAL;
            mode.waitv = mode.waitv ? 1 : 0;
            spin_lock(&tty->lock);
            tty->vt_mode      = mode;
            tty->vt_owner_pid = mode.mode == VT_PROCESS && current && current->task ? (int64_t)current->task->pid : 0;
            spin_unlock(&tty->lock);
            return 0;
        }
        case VT_GETSTATE : {
            struct vt_stat state = {.v_active = 1, .v_signal = 0, .v_state = (uint16_t)(1U << 1)};
            return copy_to_user(user_arg, &state, sizeof(state)) ? -EFAULT : 0;
        }
        case VT_OPENQRY :
            value = 1;
            return copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0;
        case VT_ACTIVATE :
        case VT_WAITACTIVE :
            return (int)(uintptr_t)user_arg == 1 ? 0 : -ENXIO;
        case VT_RELDISP :
            return (int)(uintptr_t)user_arg == VT_ACKACQ ? 0 : -EINVAL;
        case VT_DISALLOCATE :
            value = (int)(uintptr_t)user_arg;
            return value == 0 || value == 1 ? 0 : -ENXIO;
        case TIOCSWINSZ :
            if (copy_from_user(&winsize, user_arg, sizeof(winsize))) return -EFAULT;
            spin_lock(&tty->lock);
            bool changed            = memcmp(&tty->winsize, &winsize, sizeof(winsize)) != 0;
            tty->winsize            = winsize;
            int64_t foreground_pgid = tty->foreground_pgid;
            int64_t winsize_session = tty->session;
            spin_unlock(&tty->lock);
            if (changed && foreground_pgid > 0 && winsize_session > 0) signal_send_pgrp_session(foreground_pgid, winsize_session, SIGWINCH);
            return 0;
        case FIONREAD :
            value = (int)tty_core_readable(tty);
            return copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0;
        case TIOCOUTQ :
            value = 0;
            return copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0;
        case TCFLSH :
            value = (int)(uintptr_t)user_arg;
            if (value != TCIFLUSH && value != TCOFLUSH && value != TCIOFLUSH) return -EINVAL;
            if (value != TCOFLUSH) tty_core_flush_input(tty);
            if (tty->ops.event)
                tty->ops.event(tty->context, value == TCOFLUSH ? TIOCPKT_FLUSHWRITE :
                                             value == TCIFLUSH ? TIOCPKT_FLUSHREAD :
                                                                 TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE);
            return 0;
        case TCXONC :
            value = (int)(uintptr_t)user_arg;
            if (value != TCOOFF && value != TCOON && value != TCIOFF && value != TCION) return -EINVAL;
            spin_lock(&tty->lock);
            if (value == TCOOFF || value == TCOON) tty->output_stopped = value == TCOOFF;
            uint8_t flow      = value == TCOOFF ? TIOCPKT_STOP : value == TCOON ? TIOCPKT_START : 0;
            uint8_t flow_char = value == TCIOFF ? tty->termios.c_cc[VSTOP] : tty->termios.c_cc[VSTART];
            spin_unlock(&tty->lock);
            if (flow && tty->ops.event) tty->ops.event(tty->context, flow);
            if (value == TCOON) wait_queue_wake_all(&tty->write_wait);
            if ((value == TCIOFF || value == TCION) && tty->ops.emit) return tty->ops.emit(tty->context, &flow_char, 1, 0) == 1 ? 0 : -EIO;
            return 0;
        case TIOCGETD :
            value = N_TTY;
            return copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0;
        case TIOCSETD :
            if (copy_from_user(&value, user_arg, sizeof(value))) return -EFAULT;
            return value == N_TTY ? 0 : -EINVAL;
        case TIOCGPGRP : {
            if (!current) return -ENOTTY;
            int64_t session = tty->session;
            if (session != current->sid && tty->session == 0 && current->task && current->sid == (pid_t)current->task->pid
                && current->uid == 0) {
                /* Unattached tty claimed by a root session leader (this
                 * kernel has no getty; the shell respawned via setsid). */
                int result = process_ctty_acquire(current, tty, false, NULL, NULL);
                if (result) return result;
                session = tty->session;
            }
            if (session != current->sid) return -ENOTTY;
            spin_lock(&tty->lock);
            value = (int)tty->foreground_pgid;
            spin_unlock(&tty->lock);
            return value > 0 ? (copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0) : -ENOTTY;
        }
        case TIOCSPGRP : {
            if (!current) return -ESRCH;
            if (copy_from_user(&value, user_arg, sizeof(value))) return -EFAULT;
            int64_t session = tty->session;
            if (session != current->sid && tty->session == 0 && current->task && current->sid == (pid_t)current->task->pid
                && current->uid == 0) {
                /* Unattached tty claimed by a root session leader. */
                int result = process_ctty_acquire(current, tty, false, NULL, NULL);
                if (result) return result;
                session = tty->session;
            }
            if (session != current->sid || value <= 0) return -EPERM;
            return process_ctty_set_foreground(tty, session, value);
        }
        case TIOCGSID :
            if (!current || !tty_current_associated(tty, current)) return -ENOTTY;
            spin_lock(&tty->lock);
            value = tty->session == current->sid ? (int)tty->session : 0;
            spin_unlock(&tty->lock);
            return value > 0 ? (copy_to_user(user_arg, &value, sizeof(value)) ? -EFAULT : 0) : -ENOTTY;
        case TIOCSCTTY : {
            if (!current || !current->task || current->sid <= 0 || current->sid != (int64_t)current->task->pid) return -EPERM;
            if ((flags & O_PATH) || (flags & O_ACCMODE) == O_WRONLY) return -EBADF;
            value = (int)(uintptr_t)user_arg;
            if (value != 0 && value != 1) return -EINVAL;

            tty_core_t *ctty = process_ctty_get(current);
            if (ctty && ctty != tty) {
                tty_core_release(ctty);
                return -EPERM;
            }
            if (ctty) tty_core_release(ctty);

            int64_t old_session = 0;
            int64_t old_pgid    = 0;
            int     result      = process_ctty_acquire(current, tty, value == 1 && current->uid == 0, &old_session, &old_pgid);
            if (result) return result;
            if (old_session > 0 && old_session != current->sid && old_pgid > 0) {
                signal_send_pgrp_session(old_pgid, old_session, SIGHUP);
                signal_send_pgrp_session(old_pgid, old_session, SIGCONT);
            }
            return 0;
        }
        case TIOCNOTTY : {
            if (!current) return -ESRCH;
            if (!tty_current_associated(tty, current)) return -ENOTTY;
            if (!current->task || current->sid != (pid_t)current->task->pid) {
                process_ctty_clear(current);
                return 0;
            }

            int64_t session = current->sid;
            int64_t pgid    = process_ctty_disassociate(tty, session);
            if (pgid < 0) return -ENOTTY;
            if (pgid > 0) {
                signal_send_pgrp_session(pgid, session, SIGHUP);
                signal_send_pgrp_session(pgid, session, SIGCONT);
            }
            return 0;
        }
        default :
            return -ENOTTY;
    }
}

int tty_core_ioctl(tty_core_t *tty, uint64_t flags, size_t request, void *user_arg)
{
    return tty_core_ioctl_terminal(tty, flags, request, user_arg, tty && tty->is_vt);
}

int tty_core_poll(tty_core_t *tty, size_t events)
{
    int result = 0;
    spin_lock(&tty->lock);
    if ((events & POLLIN) && tty_read_ready_locked(tty, tty->termios.c_lflag & ICANON ? 1 : tty->termios.c_cc[VMIN])) result |= POLLIN;
    if ((events & POLLOUT) && !tty->hung_up && !tty->output_stopped) result |= POLLOUT;
    if (tty->hung_up) result |= POLLHUP;
    spin_unlock(&tty->lock);
    return result;
}

void tty_core_hangup(tty_core_t *tty)
{
    tty_core_retain(tty);
    spin_lock(&tty->lock);
    tty->hung_up         = true;
    int64_t pgid         = tty->foreground_pgid;
    int64_t session      = tty->session;
    tty->session         = 0;
    tty->foreground_pgid = 0;
    spin_unlock(&tty->lock);
    if (pgid > 0 && session > 0) {
        signal_send_pgrp_session(pgid, session, SIGHUP);
        signal_send_pgrp_session(pgid, session, SIGCONT);
    }
    process_ctty_clear_all(tty);
    wait_queue_wake_all(&tty->read_wait);
    wait_queue_wake_all(&tty->write_wait);
    tty_core_release(tty);
}
