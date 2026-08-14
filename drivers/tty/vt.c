/*
 *
 *      vt.c
 *      Virtual console driver (Linux drivers/tty/vt/vt.c analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/fbdev/fbcon.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/gpu/fbdev/vt_ansi.h>
#include <drivers/tty/console.h>
#include <drivers/tty/serial/serial_core.h>
#include <drivers/tty/tty.h>
#include <drivers/tty/tty_core.h>
#include <drivers/tty/tty_driver.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>

tty_device_t  boot_tty     = {0, 0};
tty_device_t *boot_tty_ptr = 0;

#define TTY_VGA_QUEUE_SIZE ((size_t)TTY_BUF_SIZE * 8)

#ifndef CONFIG_VT
#    define CONFIG_VT 1
#endif
#ifndef CONFIG_VT_COUNT
#    define CONFIG_VT_COUNT 8
#endif
#define VT_TTY_COUNT CONFIG_VT_COUNT

static char           tty_buff[TTY_BUF_SIZE]            = {0};
static volatile char *tty_buff_ptr                      = tty_buff;
static char           tty_vga_queue[TTY_VGA_QUEUE_SIZE] = {0};
static char           tty_vga_flush_buf[TTY_BUF_SIZE]   = {0};
static size_t         tty_vga_head                      = 0;
static size_t         tty_vga_tail                      = 0;
static tty_core_t     console_tty;
#if CONFIG_VT
static tty_core_t virtual_ttys[VT_TTY_COUNT - 2];
#endif
static bool       console_tty_ready;
static spinlock_t console_tty_init_lock;
static spinlock_t console_emit_lock;

#if CONFIG_VT
/*
 * tty0 and tty1 alias VT 1.  The remaining advertised terminals must
 * nevertheless keep independent line-discipline and job-control state.
 */
static tty_file_endpoint_t vt_endpoints[VT_TTY_COUNT - 1];
#endif

/* Queue one output byte, dropping the oldest byte on overflow. */
static void tty_vga_queue_push(char ch)
{
    size_t next = (tty_vga_head + 1) % TTY_VGA_QUEUE_SIZE;

    if (next == tty_vga_tail) {
        static uint64_t last_log;
        uint64_t        now = sched_ticks();
        if (now - last_log >= 1000) {
            plogk("tty: VGA output queue overflow, dropping console data.\n");
            last_log = now;
        }
        tty_vga_tail = (tty_vga_tail + 1) % TTY_VGA_QUEUE_SIZE;
    }
    tty_vga_queue[tty_vga_head] = ch;
    tty_vga_head                = next;
}

static size_t tty_vga_queue_used(void)
{
    return (tty_vga_head + TTY_VGA_QUEUE_SIZE - tty_vga_tail) % TTY_VGA_QUEUE_SIZE;
}

/* Drain the queued output through fbcon; caller holds the flush lock. */
static void tty_vga_flush_locked(void)
{
    size_t out = 0;

    if (tty_vga_tail == tty_vga_head) return;
    if (!fbcon_is_ready()) return;
    while (tty_vga_tail != tty_vga_head && out < TTY_BUF_SIZE - 1) {
        tty_vga_flush_buf[out++] = tty_vga_queue[tty_vga_tail];
        tty_vga_tail             = (tty_vga_tail + 1) % TTY_VGA_QUEUE_SIZE;
    }

    tty_vga_flush_buf[out] = '\0';
    fbcon_ansi_write((const uint8_t *)tty_vga_flush_buf, out);
}

spinlock_t tty_flush_spinlock = {
    .lock   = 0,
    .rflags = 0,
};

writer tty_writer = {
    .data    = 0,
    .handler = tty_writer_handler,
};

/* Directs character write operations to terminal output */
uint8_t tty_writer_handler(writer *writer, char c)
{
    (void)writer;
    tty_print_ch(c);
    return 1;
}

/* Obtain the tty device provided at startup */
tty_device_t *get_boot_tty(void)
{
    if (!boot_tty_ptr) {
        boot_tty     = console_derive_boot_tty();
        boot_tty_ptr = &boot_tty;
    }
    return boot_tty_ptr;
}

/* Update the TTY device type (e.g., switch to DRM after virtio-gpu init) */
void tty_set_device_type(tty_device_kind_t type)
{
    /* A requested serial console remains the diagnostic console. */
    if (boot_tty_ptr && boot_tty.type == TTY_DEVICE_SERIAL) return;
    boot_tty.type = type;
}

/* Flush the buffered console output to every registered console driver. */
void tty_buff_flush(void)
{
    spin_lock(&tty_flush_spinlock);
    size_t len = (size_t)((const char *)tty_buff_ptr - tty_buff);
    if (len) {
        *tty_buff_ptr = '\0';
        console_write_all((const uint8_t *)tty_buff, len);
        tty_buff_ptr = tty_buff;
    }
    /* Render any queued VGA output once the framebuffer console is ready. */
    if (tty_vga_tail != tty_vga_head) tty_vga_flush_locked();
    spin_unlock(&tty_flush_spinlock);
}

/* Flush only the queued VGA output (used while the console tty is held). */
void tty_deferred_flush(void)
{
    spin_lock(&tty_flush_spinlock);
    if (tty_vga_tail != tty_vga_head) tty_vga_flush_locked();
    spin_unlock(&tty_flush_spinlock);
}

/* Add character data to the teletype buffer */
static void tty_buff_add(const char ch)
{
    if (ch == '\0') return;

    spin_lock(&tty_flush_spinlock);
    if (console_tty_ready && tty_core_graphics_mode(&console_tty)) {
        spin_unlock(&tty_flush_spinlock);
        return;
    }
    *tty_buff_ptr++ = ch;

    if (ch == '\n' || (size_t)(tty_buff_ptr - tty_buff) >= TTY_BUF_SIZE - 1) {
        *tty_buff_ptr = '\0';
        console_write_all((const uint8_t *)tty_buff, (size_t)((const char *)tty_buff_ptr - tty_buff));
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

/* tty_core emit for the VT console: render to the framebuffer console only.
 * printk broadcasts through console_write_all() to every enabled console, but
 * a VT's line-discipline output (keyboard echo, writes to /dev/ttyN) belongs
 * solely to the framebuffer console.  Routing it through console_write_all()
 * would leak it onto a serial console selected with console=ttyS<N>.
 */
static int console_emit(void *context, const uint8_t *data, size_t size, uint64_t flags)
{
    (void)context;
    (void)flags;
    spin_lock(&console_emit_lock);
    spin_lock(&tty_flush_spinlock);
    if (console_tty_ready && tty_core_graphics_mode(&console_tty)) {
        spin_unlock(&tty_flush_spinlock);
        spin_unlock(&console_emit_lock);
        return (int)size;
    }
    for (size_t i = 0; i < size; i++) {
        tty_vga_queue_push((char)data[i]);
        if (tty_vga_queue_used() >= TTY_BUF_SIZE) tty_vga_flush_locked();
    }
    /*
     * Drain a partial line immediately: a userspace write() without a trailing
     * newline - e.g. printf("Press any key..."); fflush(stdout) - must not sit
     * in the queue until the next newline or a panic, leaving the prompt blank.
     */
    tty_vga_flush_locked();
    spin_unlock(&tty_flush_spinlock);
    spin_unlock(&console_emit_lock);
    return (int)size;
}

/* tty_core emit for non-current virtual consoles: drop the output. */
static int inactive_vt_emit(void *context, const uint8_t *data, size_t size, uint64_t flags)
{
    (void)context;
    (void)data;
    (void)flags;
    return (int)size;
}

/* Create the console tty core and the virtual-ttys (once). */
static void vt_input_init(void)
{
    spin_lock(&console_tty_init_lock);
    if (console_tty_ready) {
        spin_unlock(&console_tty_init_lock);
        return;
    }
    static const tty_core_ops_t console_operations  = {.emit = console_emit, .event = NULL};
    static const tty_core_ops_t inactive_operations = {.emit = inactive_vt_emit, .event = NULL};
    tty_core_init(&console_tty, &console_operations, NULL);
    tty_core_mark_virtual_console(&console_tty);
#if CONFIG_VT
    vt_endpoints[0] = (tty_file_endpoint_t) {.core = &console_tty, .virtual_console = true};
    for (size_t i = 0; i < sizeof(virtual_ttys) / sizeof(virtual_ttys[0]); i++) {
        tty_core_init(&virtual_ttys[i], &inactive_operations, (void *)(uintptr_t)(i + 1));
        tty_core_mark_virtual_console(&virtual_ttys[i]);
        vt_endpoints[i + 1] = (tty_file_endpoint_t) {.core = &virtual_ttys[i], .virtual_console = true};
    }
#endif
    console_tty_ready = true;
    spin_unlock(&console_tty_init_lock);
}

/* Propagate a framebuffer size change to the console tty's winsize. */
void tty_console_resize(uint16_t rows, uint16_t cols)
{
    vt_input_init();
    tty_core_set_winsize(&console_tty, rows, cols);
}

/* Legacy device write: output to the console tty core. */
size_t tty_dev_write(void *ctx, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)offset;
    vt_input_init();
    int64_t result = tty_core_write(&console_tty, addr, size, 0);
    return result < 0 ? 0 : (size_t)result;
}

#if CONFIG_VT
static bool tty_shift_pressed = false;
static bool tty_ctrl_pressed  = false;
static bool tty_caps_active   = false;

static const unsigned char tty_keymap[128] = {
    0,    0,   '1', '2',  '3', '4', '5', '6', '7', '8', // 0-9
    '9',  '0', '-', '=',  0,   0,   'q', 'w', 'e', 'r', // 10-19
    't',  'y', 'u', 'i',  'o', 'p', '[', ']', 0,   0,   // 20-29
    'a',  's', 'd', 'f',  'g', 'h', 'j', 'k', 'l', ';', // 30-39
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', // 40-49
    'm',  ',', '.', '/',  0,   '*', 0,   ' ', 0,        // 50-58
};

static const unsigned char tty_keymap_shift[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', // 0-9
    '(', ')', '_', '+', 0,   0,   'Q', 'W', 'E', 'R', // 10-19
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,   0,   // 20-29
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', // 30-39
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', // 40-49
    'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,        // 50-58
};

/* Feed a scancode into the TTY line discipline */
void tty_handle_scancode(uint8_t scancode, bool pressed)
{
    vt_input_init();
    if (tty_core_keyboard_mode(&console_tty) == K_OFF) return;

    switch (scancode) {
        case 42 : // LSHIFT
        case 54 : // RSHIFT
            tty_shift_pressed = pressed;
            return;
        case 29 : // LCTRL
        case 97 : // RCTRL
            tty_ctrl_pressed = pressed;
            return;
        case 58 : // CAPSLOCK
            if (pressed) tty_caps_active = !tty_caps_active;
            return;
        case 56 :  // LALT
        case 100 : // RALT
            return;
        default :
            break;
    }

    if (!pressed) return;

    char ch = 0;

    switch (scancode) {
        case 14 : // BACKSPACE
            ch = '\b';
            break;
        case 28 : // ENTER
            ch = '\r';
            break;
        case 15 : // TAB
            ch = '\t';
            break;
        case 57 : // SPACE
            ch = ' ';
            break;
        case 1 : // ESC
            ch = 0x1B;
            break;
        default :
            if (scancode >= 128) return;
            ch = (char)tty_keymap[scancode];
            if (!ch) return;
            {
                bool shift = tty_shift_pressed;
                if (tty_caps_active && ch >= 'a' && ch <= 'z') shift = !shift;
                if (shift) ch = (char)tty_keymap_shift[scancode];
            }
            if (!ch) return;
            if (tty_ctrl_pressed && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 1);
            break;
    }

    if (ch == '\b') ch = 127;
    tty_core_receive(&console_tty, (const uint8_t *)&ch, 1, O_NONBLOCK);
}
#else
void tty_handle_scancode(uint8_t scancode, bool pressed)
{
    (void)scancode;
    (void)pressed;
}
#endif /* CONFIG_VT */

/* Legacy device read: read from the console tty core. */
size_t tty_dev_read(void *ctx, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)offset;
    vt_input_init();
    int64_t result = tty_core_read(&console_tty, addr, size, 0);
    return result < 0 ? 0 : (size_t)result;
}

/* Legacy device poll: report the console tty's readiness. */
int tty_dev_poll(void *ctx, size_t events)
{
    (void)ctx;
    vt_input_init();
    return tty_core_poll(&console_tty, events);
}

/* Validate that a session leader may acquire the console. */
int tty_console_acquire(struct process *proc, uint64_t flags)
{
    if (!proc || !proc->task || (flags & (O_NOCTTY | O_PATH)) || (flags & O_ACCMODE) == O_WRONLY) return -EINVAL;

    pid_t pid = (pid_t)proc->task->pid;
    if (pid <= 0 || proc->sid != pid || proc->pgid != pid) return -EPERM;

    vt_input_init();
    return 0;
}

#if CONFIG_VT
/* Virtual console tty driver (major 4, tty0-ttyN) */

/* Map a ttyN index to a virtual-console slot (tty0/tty1 share VT 1). */
static int vt_slot_for_index(int index)
{
    return (index <= 1) ? 0 : index - 1;
}

/* tty driver open: bind the node to its virtual-console endpoint. */
static int vt_driver_open(tty_driver_t *drv, int index, uint64_t flags, void **private_data)
{
    int slot;
    (void)drv;
    vt_input_init();
    if (index < 0 || index >= VT_TTY_COUNT) return -ENXIO;
    slot = vt_slot_for_index(index);
    tty_core_auto_acquire(vt_endpoints[slot].core, flags);
    *private_data = &vt_endpoints[slot];
    return 0;
}

static int vt_driver_release(tty_driver_t *drv, int index, void *private_data)
{
    (void)drv;
    (void)index;
    (void)private_data;
    return 0;
}

/* tty driver read from the virtual-console core. */
static int64_t vt_driver_read(tty_driver_t *drv, int index, void *private_data, uint64_t flags, void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_read(ep->core, addr, size, flags) : -ENXIO;
}

/* tty driver write to the virtual-console core. */
static int64_t vt_driver_write(tty_driver_t *drv, int index, void *private_data, uint64_t flags, const void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_write(ep->core, addr, size, flags) : -ENXIO;
}

/* tty driver ioctl for a virtual console. */
static int vt_driver_ioctl(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t req, void *arg)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    if (!ep || !ep->core) return -ENXIO;
    return tty_core_ioctl_terminal(ep->core, flags, req, arg, ep->virtual_console);
}

/* tty driver poll for a virtual console. */
static int vt_driver_poll(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t events)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    (void)flags;
    return ep && ep->core ? tty_core_poll(ep->core, events) : 0;
}

static tty_driver_t vt_tty_driver = {
    .name        = "tty",
    .major       = 4,
    .minor_start = 0,
    .num         = VT_TTY_COUNT,
    .node_type   = file_stream,
    .open        = vt_driver_open,
    .release     = vt_driver_release,
    .read        = vt_driver_read,
    .write       = vt_driver_write,
    .ioctl       = vt_driver_ioctl,
    .poll        = vt_driver_poll,
};
#endif /* CONFIG_VT */

/* Auxiliary tty driver (major 5: /dev/tty, /dev/console) */

/* Open /dev/tty (controlling tty) or /dev/console. */
static int aux_driver_open(tty_driver_t *drv, int index, uint64_t flags, void **private_data)
{
    tty_file_endpoint_t *ep;
    (void)drv;
    vt_input_init();

    ep = malloc(sizeof(*ep));
    if (!ep) return -ENOMEM;

    if (index == 0) { // /dev/tty : current controlling terminal
        tty_core_t *ctty = process_ctty_get(process_current());
        if (!ctty) {
            free(ep);
            return -ENXIO;
        }
        ep->core            = ctty;
        ep->virtual_console = ctty->is_vt;
    } else if (index == 1) { // /dev/console
        ep->core = console_get_tty();
        if (!ep->core) {
            free(ep);
            return -ENXIO;
        }
        ep->virtual_console = (ep->core == &console_tty);
        tty_core_auto_acquire(ep->core, flags);
    } else {
        free(ep);
        return -ENXIO;
    }
    *private_data = ep;
    return 0;
}

/* Release an aux tty endpoint and its retained core. */
static int aux_driver_release(tty_driver_t *drv, int index, void *private_data)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    if (!ep) return -EINVAL;
    if (index == 0 && ep->core) tty_core_release(ep->core);
    free(ep);
    return 0;
}

/* Read from the resolved aux tty core. */
static int64_t aux_driver_read(tty_driver_t *drv, int index, void *private_data, uint64_t flags, void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_read(ep->core, addr, size, flags) : -ENXIO;
}

/* Write to the resolved aux tty core. */
static int64_t aux_driver_write(tty_driver_t *drv, int index, void *private_data, uint64_t flags, const void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_write(ep->core, addr, size, flags) : -ENXIO;
}

/* Forward ioctls to the resolved aux tty core. */
static int aux_driver_ioctl(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t req, void *arg)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    if (!ep || !ep->core) return -ENXIO;
    return tty_core_ioctl_terminal(ep->core, flags, req, arg, ep->virtual_console);
}

/* Poll the resolved aux tty core. */
static int aux_driver_poll(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t events)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    (void)flags;
    return ep && ep->core ? tty_core_poll(ep->core, events) : 0;
}

static tty_driver_t aux_tty_driver = {
    .name        = "aux",
    .major       = 5,
    .minor_start = 0,
    .num         = 2,
    .node_type   = file_stream,
    .mode        = 0666,
    .open        = aux_driver_open,
    .release     = aux_driver_release,
    .read        = aux_driver_read,
    .write       = aux_driver_write,
    .ioctl       = aux_driver_ioctl,
    .poll        = aux_driver_poll,
};

/* Console drivers (output through the framebuffer console) */

/* Console write: queue output to the framebuffer console. */
static void vga_console_write(console_t *c, const uint8_t *buf, size_t len)
{
    /*
     * Queue the bytes and drain the queue to the framebuffer console once
     * TTY_BUF_SIZE bytes have accumulated.  The rolling drain keeps boot
     * output flowing even before the scheduler timer starts, and prevents
     * the queue from overflowing during heavy init (an overflow while the
     * tty lock is held would recurse back into printk).  The hand-off is
     * safe before fbcon is initialised because tty_vga_flush_locked() skips
     * the render while the framebuffer console is not ready.
     */
    (void)c;
    for (size_t i = 0; i < len; i++) {
        tty_vga_queue_push((char)buf[i]);
        if (tty_vga_queue_used() >= TTY_BUF_SIZE) tty_vga_flush_locked();
    }
}

static tty_core_t *vt_console_get_tty(console_t *c)
{
    (void)c;
    return &console_tty;
}

static console_t vt_console = {
    .name    = "tty",
    .index   = 0,
    .write   = vga_console_write,
    .get_tty = vt_console_get_tty,
};

static console_t drm_console = {
    .name    = "ttyD",
    .index   = 0,
    .write   = vga_console_write,
    .get_tty = vt_console_get_tty,
};

/* Register the vt/drm console drivers. Must run before the first printk. */
void vt_console_init(void)
{
    (void)register_console(&vt_console);
    (void)register_console(&drm_console);
}

/* Register the vt and aux tty drivers with their /dev nodes. */
void vt_driver_init(void)
{
#if CONFIG_VT
    (void)tty_register_driver(&vt_tty_driver);
    for (int i = 0; i < VT_TTY_COUNT; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "tty%d", i);
        (void)tty_register_device(&vt_tty_driver, i, name);
    }
#endif
    (void)tty_register_driver(&aux_tty_driver);
    (void)tty_register_device(&aux_tty_driver, 0, "tty");
    (void)tty_register_device(&aux_tty_driver, 1, "console");
}
