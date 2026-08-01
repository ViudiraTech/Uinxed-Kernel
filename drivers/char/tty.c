/*
 *
 *      tty.c
 *      Teletype
 *
 *      2025/4/12 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/char/tty.h>
#include <drivers/char/tty_core.h>
#include <drivers/ports/serial.h>
#include <fs/core/vfs.h>
#include <kernel/cmdline.h>
#include <kernel/errno.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <proc/task.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>
#include <video/fbcon.h>
#include <video/video.h>
#include <video/vt_ansi.h>

tty_device_t  boot_tty     = {0, 0};
tty_device_t *boot_tty_ptr = 0;

#define TTY_VGA_QUEUE_SIZE (TTY_BUF_SIZE * 8)

static char           boot_tty_str_buf[16]              = {0}; // Persistent buffer
static char           tty_buff[TTY_BUF_SIZE]            = {0};
static volatile char *tty_buff_ptr                      = tty_buff;
static char           tty_vga_queue[TTY_VGA_QUEUE_SIZE] = {0};
static size_t         tty_vga_head                      = 0;
static size_t         tty_vga_tail                      = 0;
static tty_core_t     console_tty;
static bool           console_tty_ready;
static spinlock_t     console_tty_init_lock;
static spinlock_t     console_emit_lock;

static int tty_should_flush_now(const tty_device_t *tty_device, const char ch, size_t used)
{
    if (!tty_device) return 1;
    if (used >= TTY_BUF_SIZE - 1) return 1;
    if (tty_device->type == TTY_DEVICE_SERIAL) return ch == '\n';
    if (tty_device->type == TTY_DEVICE_VGA) return 0;
    if (tty_device->type == TTY_DEVICE_DRM) return 0;
    return ch == '\n';
}

static size_t tty_vga_queue_used(void)
{
    if (tty_vga_head >= tty_vga_tail) return tty_vga_head - tty_vga_tail;
    return TTY_VGA_QUEUE_SIZE - tty_vga_tail + tty_vga_head;
}

static void tty_vga_queue_push(char ch)
{
    size_t next = (tty_vga_head + 1) % TTY_VGA_QUEUE_SIZE;

    if (next == tty_vga_tail) tty_vga_tail = (tty_vga_tail + 1) % TTY_VGA_QUEUE_SIZE;
    tty_vga_queue[tty_vga_head] = ch;
    tty_vga_head                = next;
}

static void tty_vga_flush_locked(void)
{
    size_t out = 0;

    if (tty_vga_tail == tty_vga_head) return;
    while (tty_vga_tail != tty_vga_head && out < TTY_BUF_SIZE - 1) {
        tty_buff[out++] = tty_vga_queue[tty_vga_tail];
        tty_vga_tail    = (tty_vga_tail + 1) % TTY_VGA_QUEUE_SIZE;
    }

    tty_buff[out] = '\0';
    fbcon_ansi_write((const uint8_t *)tty_buff, out);
}

spinlock_t tty_flush_spinlock = {
    .lock   = 0,
    .rflags = 0,
};

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
    } else if (!strcmp(type_str, "ttyD")) {
        tty_dev.type = TTY_DEVICE_DRM;
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
                } else if (tmp_tty.type == TTY_DEVICE_DRM) {
                    if (!tmp_tty.port) valid = 1;
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

/* Update the TTY device type (e.g., switch to DRM after virtio-gpu init) */
void tty_set_device_type(tty_device_kind_t type)
{
    /* A requested serial console remains the diagnostic console. */
    if (boot_tty_ptr && boot_tty.type == TTY_DEVICE_SERIAL) return;
    boot_tty.type = type;
}

/* Output the buffer data to the specified device according to the configuration */
void tty_buff_flush(void)
{
    spin_lock(&tty_flush_spinlock);
    tty_device_t *tty_device  = get_boot_tty();
    uint16_t      serial_port = 0;
    uint8_t       early_break = 0;

    for (int attempt = 0; attempt < 2 && !early_break; ++attempt) {
        early_break = 1;
        switch (tty_device->type) {
            case TTY_DEVICE_VGA :
            case TTY_DEVICE_DRM :
                if (tty_device->port == 0) {
                    tty_vga_flush_locked();
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
                for (char *ch = tty_buff; ch < (char *)tty_buff_ptr && *ch != '\0'; ch++) write_serial(serial_port, *ch);
                tty_buff_ptr = tty_buff;
                tty_buff[0]  = '\0';
                break;
            default :
                /* Unreachable */
                break;
        }
    }
    spin_unlock(&tty_flush_spinlock);
}

void tty_deferred_flush(void)
{
    tty_device_t *tty_device = get_boot_tty();

    if (!tty_device || (tty_device->type != TTY_DEVICE_VGA && tty_device->type != TTY_DEVICE_DRM) || tty_device->port != 0) return;

    spin_lock(&tty_flush_spinlock);
    if (tty_vga_tail == tty_vga_head) {
        spin_unlock(&tty_flush_spinlock);
        return;
    }
    tty_vga_flush_locked();
    spin_unlock(&tty_flush_spinlock);
}

/* Add character data to the teletype buffer */
static void tty_buff_add(const char ch)
{
    tty_device_t *tty_device;

    if (ch == '\0') return;
    tty_device = get_boot_tty();

    spin_lock(&tty_flush_spinlock);
    if (tty_device && (tty_device->type == TTY_DEVICE_VGA || tty_device->type == TTY_DEVICE_DRM) && tty_device->port == 0) {
        /* Linux stops fbcon rendering while the active VT owns graphics. */
        if (console_tty_ready && tty_core_graphics_mode(&console_tty)) {
            spin_unlock(&tty_flush_spinlock);
            return;
        }
        tty_vga_queue_push(ch);
        if (tty_vga_queue_used() >= TTY_BUF_SIZE) tty_vga_flush_locked();
        spin_unlock(&tty_flush_spinlock);
        return;
    }

    *tty_buff_ptr++ = ch;

    if (tty_should_flush_now(tty_device, ch, (size_t)(tty_buff_ptr - tty_buff))) {
        *tty_buff_ptr        = '\0';
        uint16_t serial_port = tty_device && tty_device->port == 1 ? SERIAL_PORT_2 :
                               tty_device && tty_device->port == 2 ? SERIAL_PORT_3 :
                               tty_device && tty_device->port == 3 ? SERIAL_PORT_4 :
                                                                     SERIAL_PORT_1;
        for (char *ptr = tty_buff; ptr < (char *)tty_buff_ptr && *ptr != '\0'; ptr++) write_serial(serial_port, *ptr);
        tty_buff_ptr = tty_buff;
    }
    *tty_buff_ptr = '\0';
    spin_unlock(&tty_flush_spinlock);
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

static int console_emit(void *context, const uint8_t *data, size_t size, uint64_t flags)
{
    (void)context;
    (void)flags;
    spin_lock(&console_emit_lock);
    for (size_t i = 0; i < size; i++) tty_print_ch((char)data[i]);
    tty_deferred_flush();
    spin_unlock(&console_emit_lock);
    return (int)size;
}

static void tty_input_lazy_init(void)
{
    spin_lock(&console_tty_init_lock);
    if (console_tty_ready) {
        spin_unlock(&console_tty_init_lock);
        return;
    }
    static const tty_core_ops_t operations = {.emit = console_emit, .event = NULL};
    tty_core_init(&console_tty, &operations, NULL);
    tty_core_mark_virtual_console(&console_tty);
    console_tty_ready = true;
    spin_unlock(&console_tty_init_lock);
}

size_t tty_dev_write(void *ctx, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)offset;
    tty_input_lazy_init();
    int64_t result = tty_core_write(&console_tty, addr, size, 0);
    return result < 0 ? 0 : (size_t)result;
}

static bool tty_shift_pressed = false;
static bool tty_ctrl_pressed  = false;
static bool tty_caps_active   = false;

/*
 * US QWERTY keymap (Set? scancode ?ASCII).
 * scancodes that do not produce a printable character map to 0.
 */
static const unsigned char tty_keymap[128] = {
    0,    0,   '1', '2',  '3', '4', '5', '6', '7', '8', /* 0-9 */
    '9',  '0', '-', '=',  0,   0,   'q', 'w', 'e', 'r', /* 10-19 */
    't',  'y', 'u', 'i',  'o', 'p', '[', ']', 0,   0,   /* 20-29 */
    'a',  's', 'd', 'f',  'g', 'h', 'j', 'k', 'l', ';', /* 30-39 */
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', /* 40-49 */
    'm',  ',', '.', '/',  0,   '*', 0,   ' ', 0,        /* 50-58 */
};

static const unsigned char tty_keymap_shift[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', /* 0-9 */
    '(', ')', '_', '+', 0,   0,   'Q', 'W', 'E', 'R', /* 10-19 */
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,   0,   /* 20-29 */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', /* 30-39 */
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', /* 40-49 */
    'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,        /* 50-58 */
};

/* Feed a scancode into the TTY line discipline */
void tty_handle_scancode(uint8_t scancode, bool pressed)
{
    tty_input_lazy_init();
    if (tty_core_keyboard_mode(&console_tty) == K_OFF) return;

    /* Track modifier keys */
    switch (scancode) {
        case 42 : /* LSHIFT */
        case 54 : /* RSHIFT */
            tty_shift_pressed = pressed;
            return;
        case 29 : /* LCTRL */
        case 97 : /* RCTRL */
            tty_ctrl_pressed = pressed;
            return;
        case 58 : /* CAPSLOCK */
            if (pressed) tty_caps_active = !tty_caps_active;
            return;
        case 56 :  /* LALT */
        case 100 : /* RALT */
            return;
        default :
            break;
    }

    /* Only handle key-press events, not releases */
    if (!pressed) return;

    char ch = 0;

    /* Handle special (non-printable) keys */
    switch (scancode) {
        case 14 : /* BACKSPACE */
            ch = '\b';
            break;
        case 28 : /* ENTER - send CR; canonical mode converts to LF via ICRNL */
            ch = '\r';
            break;
        case 15 : /* TAB */
            ch = '\t';
            break;
        case 57 : /* SPACE */
            ch = ' ';
            break;
        case 1 : /* ESC */
            ch = 0x1B;
            break;
        default :
            /* Translate printable keys via keymap */
            if (scancode >= 128) return;
            ch = (char)tty_keymap[scancode];
            if (!ch) return;
            {
                bool shift = tty_shift_pressed;
                if (tty_caps_active && ch >= 'a' && ch <= 'z') shift = !shift;
                if (shift) ch = (char)tty_keymap_shift[scancode];
            }
            if (!ch) return;

            /* Ctrl+letter ?control code */
            if (tty_ctrl_pressed && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 1);
            break;
    }

    if (ch == '\b') ch = 127;
    tty_core_receive(&console_tty, (const uint8_t *)&ch, 1, O_NONBLOCK);
}

size_t tty_dev_read(void *ctx, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)offset;
    tty_input_lazy_init();
    int64_t result = tty_core_read(&console_tty, addr, size, 0);
    return result < 0 ? 0 : (size_t)result;
}

int tty_dev_poll(void *ctx, size_t events)
{
    (void)ctx;
    tty_input_lazy_init();
    return tty_core_poll(&console_tty, events);
}

int tty_dev_file_open(struct vfs_node *node, uint64_t flags, void **private_data)
{
    tty_input_lazy_init();
    tty_core_auto_acquire(&console_tty, flags);
    /* Non-NULL marks aliases that expose Linux virtual-console ioctls. */
    *private_data = node && (streq(node->name, "tty0") || streq(node->name, "tty1") || streq(node->name, "console")) ? (void *)(uintptr_t)1 : NULL;
    return 0;
}

int tty_console_acquire(struct process *proc, uint64_t flags)
{
    if (!proc || !proc->task || (flags & (O_NOCTTY | O_PATH)) || (flags & O_ACCMODE) == O_WRONLY) return -EINVAL;

    pid_t pid = (pid_t)proc->task->pid;
    if (pid <= 0 || proc->sid != pid || proc->pgid != pid) return -EPERM;

    tty_input_lazy_init();
    return process_ctty_acquire(proc, &console_tty, false, NULL, NULL);
}

int64_t tty_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)offset;
    tty_input_lazy_init();
    return tty_core_read(&console_tty, addr, size, flags);
}

int64_t tty_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)offset;
    tty_input_lazy_init();
    return tty_core_write(&console_tty, addr, size, flags);
}

int tty_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    return tty_dev_poll(NULL, events);
}

int tty_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *arg)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    tty_input_lazy_init();
    return tty_core_ioctl_terminal(&console_tty, flags, request, arg, private_data != NULL);
}

int tty_ctty_file_open(struct vfs_node *node, uint64_t flags, void **private_data)
{
    (void)node;
    (void)flags;
    tty_core_t *tty = process_ctty_get(process_current());
    if (!tty) return -ENXIO;
    *private_data = tty;
    return 0;
}

void tty_ctty_file_release(struct vfs_node *node, void *private_data)
{
    (void)node;
    tty_core_release(private_data);
}

int64_t tty_ctty_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)offset;
    return tty_core_read(private_data, addr, size, flags);
}

int64_t tty_ctty_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)offset;
    return tty_core_write(private_data, addr, size, flags);
}

int tty_ctty_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;
    return tty_core_poll(private_data, events);
}

int tty_ctty_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *arg)
{
    (void)ctx;
    return tty_core_ioctl(private_data, flags, request, arg);
}
