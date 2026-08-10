/*
 *
 *      video.c
 *      Basic video
 *
 *      2024/9/16 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <boot/limine.h>
#include <chipset/common.h>
#include <drivers/gpu/fbdev/fbcon.h>
#include <drivers/gpu/fbdev/fbdev.h>
#include <drivers/gpu/fbdev/klogo.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/tty/tty.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <libs/gfxs/gfx_proc.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>

/* Active scanout state.  The backing is replaced once when KMS takes over. */
static video_flush_fn_t video_flush_cb;
static video_info_t     video_active_info;
static spinlock_t       video_state_lock;
static uint64_t         video_generation;
static bool             video_refresh_worker_started;

uint64_t  width;  // Screen width
uint64_t  height; // Screen height
uint64_t  stride; // Frame buffer line spacing
uint32_t *buffer; // Video Memory (We think BPP is 32. If BPP is other value, you have to change it)

uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

uint32_t font_width;  // Font width
uint32_t font_height; // Font height

/* Get video information */
video_info_t video_get_info(void)
{
    video_info_t info;

    spin_lock(&video_state_lock);
    info            = video_active_info;
    info.c_width    = font_width ? info.width / font_width : 0;
    info.c_height   = font_height ? info.height / font_height : 0;
    info.cx         = cx;
    info.cy         = cy;
    info.fore_color = fore_color;
    info.back_color = back_color;

    spin_unlock(&video_state_lock);
    return info;
}

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void)
{
    if (!framebuffer_request.response || !framebuffer_request.response->framebuffer_count) return NULL;
    return framebuffer_request.response->framebuffers[0];
}

/* Read raw bytes from the primary framebuffer */
size_t video_fb_read(void *ctx, void *addr, size_t offset, size_t size)
{
    uint8_t *src;
    size_t   fb_size;
    size_t   bytes_per_pixel;

    (void)ctx;
    if (!addr || !buffer) return 0;

    video_info_t info = video_get_info();
    bytes_per_pixel   = (info.bpp + 7U) / 8U;
    if (!bytes_per_pixel || info.stride > SIZE_MAX / info.height || info.stride * info.height > SIZE_MAX / bytes_per_pixel) return 0;
    fb_size = (size_t)(info.stride * info.height * bytes_per_pixel);
    if (offset >= fb_size) return 0;

    if (size > fb_size - offset) size = fb_size - offset;
    src = (uint8_t *)buffer + offset;
    memcpy(addr, src, size);
    return size;
}

/* Write raw bytes to the primary framebuffer */
size_t video_fb_write(void *ctx, const void *addr, size_t offset, size_t size)
{
    uint8_t *dst;
    size_t   fb_size;
    size_t   row_bytes;
    size_t   start_row;
    size_t   end_row;

    (void)ctx;
    if (!addr || !buffer) return 0;

    video_info_t info            = video_get_info();
    size_t       bytes_per_pixel = (info.bpp + 7U) / 8U;
    if (!bytes_per_pixel || info.stride > SIZE_MAX / info.height || info.stride * info.height > SIZE_MAX / bytes_per_pixel) return 0;
    fb_size = (size_t)(info.stride * info.height * bytes_per_pixel);
    if (offset >= fb_size) return 0;

    if (size > fb_size - offset) size = fb_size - offset;
    dst = (uint8_t *)buffer + offset;
    memcpy(dst, addr, size);

    /*
     * /dev/fb0 writes are byte ranges.  Convert them to one conservative
     * rectangle so DRM-backed consoles do not retransmit the whole frame.
     */
    if (size) {
        row_bytes = (size_t)info.stride * bytes_per_pixel;
        start_row = offset / row_bytes;
        end_row   = (offset + size - 1) / row_bytes;
        if (start_row == end_row) {
            size_t x0 = (offset % row_bytes) / bytes_per_pixel;
            size_t x1 = ((offset % row_bytes) + size + bytes_per_pixel - 1) / bytes_per_pixel;
            video_flush_rect((uint32_t)x0, (uint32_t)start_row, (uint32_t)(x1 - x0), 1);
        } else {
            video_flush_rect(0, (uint32_t)start_row, (uint32_t)width, (uint32_t)(end_row - start_row + 1));
        }
    }
    return size;
}

static size_t video_fb_size(const video_info_t *info)
{
    size_t bytes_per_pixel = (info->bpp + 7U) / 8U;
    if (!bytes_per_pixel || !info->height || info->stride > SIZE_MAX / info->height || info->stride * info->height > SIZE_MAX / bytes_per_pixel)
        return 0;
    return (size_t)(info->stride * info->height * bytes_per_pixel);
}

static fbdev_var_screeninfo_t video_fb_var(const video_info_t *info)
{
    fbdev_var_screeninfo_t var;
    uint64_t               htotal;
    uint64_t               vtotal;
    uint64_t               clock_khz;

    memset(&var, 0, sizeof(var));
    var.xres           = (uint32_t)info->width;
    var.yres           = (uint32_t)info->height;
    var.xres_virtual   = (uint32_t)info->stride;
    var.yres_virtual   = (uint32_t)info->height;
    var.bits_per_pixel = info->bpp;
    var.red.offset     = info->red_mask_shift;
    var.red.length     = info->red_mask_size;
    var.green.offset   = info->green_mask_shift;
    var.green.length   = info->green_mask_size;
    var.blue.offset    = info->blue_mask_shift;
    var.blue.length    = info->blue_mask_size;
    var.width          = UINT32_MAX;
    var.height         = UINT32_MAX;

    /* Stable 60 Hz timing metadata for the fixed virtio scanout mode. */
    var.right_margin = 80;
    var.hsync_len    = 80;
    var.left_margin  = 160;
    var.lower_margin = 3;
    var.vsync_len    = 3;
    var.upper_margin = 26;
    var.vmode        = FB_VMODE_NONINTERLACED;
    htotal           = info->width + var.right_margin + var.hsync_len + var.left_margin;
    vtotal           = info->height + var.lower_margin + var.vsync_len + var.upper_margin;
    clock_khz        = htotal * vtotal * 60U / 1000U;
    if (clock_khz) var.pixclock = (uint32_t)(1000000000ULL / clock_khz);
    return var;
}

static int video_fb_validate_mode(const video_info_t *info, const fbdev_var_screeninfo_t *requested)
{
    fbdev_var_screeninfo_t current  = video_fb_var(info);
    uint32_t               activate = requested->activate & FB_ACTIVATE_MASK;

    if (activate != FB_ACTIVATE_NOW && activate != FB_ACTIVATE_TEST) return -EINVAL;
    if (requested->xres != current.xres || requested->yres != current.yres || requested->xres_virtual != current.xres_virtual
        || requested->yres_virtual != current.yres_virtual || requested->bits_per_pixel != current.bits_per_pixel || requested->xoffset
        || requested->yoffset || requested->grayscale || requested->nonstd || requested->red.length != current.red.length
        || requested->green.length != current.green.length || requested->blue.length != current.blue.length
        || requested->right_margin != current.right_margin || requested->hsync_len != current.hsync_len
        || requested->left_margin != current.left_margin || requested->lower_margin != current.lower_margin
        || requested->vsync_len != current.vsync_len || requested->upper_margin != current.upper_margin || requested->sync != current.sync
        || (requested->vmode & FB_VMODE_MASK) != current.vmode)
        return -EINVAL;
    return EOK;
}

/* Query framebuffer device metadata */
int video_fb_ioctl(void *ctx, size_t req, void *arg)
{
    video_info_t info;

    (void)ctx;
    info = video_get_info();
    switch (req) {
        case FBDEV_IOCTL_GET_INFO : {
            if (!arg) return -EFAULT;
            size_t fb_size = video_fb_size(&info);
            if (!fb_size) return -EOVERFLOW;
            fbdev_info_t fb_info = {
                .width            = info.width,
                .height           = info.height,
                .stride           = info.stride,
                .bpp              = info.bpp,
                .size             = fb_size,
                .red_mask_size    = info.red_mask_size,
                .red_mask_shift   = info.red_mask_shift,
                .green_mask_size  = info.green_mask_size,
                .green_mask_shift = info.green_mask_shift,
                .blue_mask_size   = info.blue_mask_size,
                .blue_mask_shift  = info.blue_mask_shift,
            };
            return copy_to_user(arg, &fb_info, sizeof(fb_info)) ? -EFAULT : EOK;
        }
        case FBIOGET_FSCREENINFO : {
            if (!arg) return -EFAULT;
            size_t fb_size = video_fb_size(&info);
            if (!fb_size || fb_size > UINT32_MAX) return -EOVERFLOW;
            fbdev_fix_screeninfo_t fix;
            memset(&fix, 0, sizeof(fix));
            memcpy(fix.id, "Uinxed virtiofb", sizeof("Uinxed virtiofb"));
            fix.smem_len    = (uint32_t)fb_size;
            fix.type        = FB_TYPE_PACKED_PIXELS;
            fix.visual      = FB_VISUAL_TRUECOLOR;
            fix.line_length = (uint32_t)(info.stride * ((info.bpp + 7U) / 8U));
            fix.accel       = FB_ACCEL_NONE;
            return copy_to_user(arg, &fix, sizeof(fix)) ? -EFAULT : EOK;
        }
        case FBIOGET_VSCREENINFO : {
            if (!arg) return -EFAULT;
            fbdev_var_screeninfo_t var = video_fb_var(&info);
            return copy_to_user(arg, &var, sizeof(var)) ? -EFAULT : EOK;
        }
        case FBIOPUT_VSCREENINFO : {
            if (!arg) return -EFAULT;
            fbdev_var_screeninfo_t var;
            if (copy_from_user(&var, arg, sizeof(var))) return -EFAULT;
            int status = video_fb_validate_mode(&info, &var);
            if (status) return status;
            fbdev_var_screeninfo_t normalized = video_fb_var(&info);
            normalized.activate               = var.activate;
            return copy_to_user(arg, &normalized, sizeof(normalized)) ? -EFAULT : EOK;
        }
        case FBIOPAN_DISPLAY : {
            if (!arg) return -EFAULT;
            fbdev_var_screeninfo_t var;
            if (copy_from_user(&var, arg, sizeof(var))) return -EFAULT;
            return (!var.xoffset && !var.yoffset) ? EOK : -EINVAL;
        }
        case FBIOGETCMAP :
            return -EINVAL;
        case FBIOPUTCMAP : {
            if (!arg) return -EFAULT;
            fbdev_cmap_t cmap;
            uint16_t     scratch[256];
            if (copy_from_user(&cmap, arg, sizeof(cmap))) return -EFAULT;
            if (cmap.start > 256 || cmap.len > 256 - cmap.start || (cmap.len && (!cmap.red || !cmap.green || !cmap.blue))) return -EINVAL;
            size_t bytes = (size_t)cmap.len * sizeof(uint16_t);
            if (bytes
                && (copy_from_user(scratch, cmap.red, bytes) || copy_from_user(scratch, cmap.green, bytes)
                    || copy_from_user(scratch, cmap.blue, bytes) || (cmap.transp && copy_from_user(scratch, cmap.transp, bytes))))
                return -EFAULT;
            return EOK;
        }
        case FBIOBLANK :
            /* The current virtio KMS path has no DPMS primitive. */
            (void)arg;
            return -EINVAL;
        default :
            return -ENOTTY;
    }
}

void *video_fb_mmap(void *ctx, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    video_info_t info    = video_get_info();
    size_t       fb_size = video_fb_size(&info);
    size_t       mapped_size;

    (void)ctx;
    (void)private_data;
    if (!(flags & VM_SHARED)) return NULL;
    (void)vma;
    if (!fb_size || ((uintptr_t)buffer & (PAGE_4K_SIZE - 1)) || (offset & (PAGE_4K_SIZE - 1))) return NULL;
    mapped_size = ALIGN_UP(fb_size, PAGE_4K_SIZE);
    if (offset > mapped_size || size > mapped_size - offset) return NULL;
    return (uint8_t *)buffer + offset;
}

static void video_refresh_worker(void *arg)
{
    uint8_t *snapshot            = NULL;
    size_t   snapshot_size       = 0;
    uint64_t snapshot_generation = 0;

    (void)arg;
    for (;;) {
        task_sleep_ticks((TIMER_HZ + 59) / 60);

        /* Flip the block cursor phase; the frame-diff below flushes it. */
        fbcon_cursor_tick(sched_ticks());

        spin_lock(&video_state_lock);
        uint8_t         *front      = (uint8_t *)buffer;
        uint32_t         active_w   = (uint32_t)width;
        uint32_t         active_h   = (uint32_t)height;
        uint32_t         active_row = (uint32_t)(stride * sizeof(uint32_t));
        uint64_t         generation = video_generation;
        video_flush_fn_t flush      = video_flush_cb;
        spin_unlock(&video_state_lock);
        if (!front || !active_w || !active_h || !flush || active_h > SIZE_MAX / active_row) continue;

        size_t active_size = (size_t)active_row * active_h;
        if (!snapshot || snapshot_size != active_size || snapshot_generation != generation) {
            free(snapshot);
            snapshot = malloc(active_size);
            if (!snapshot) {
                snapshot_size = 0;
                flush(0, 0, active_w, active_h);
                continue;
            }
            memcpy(snapshot, front, active_size);
            snapshot_size       = active_size;
            snapshot_generation = generation;
            flush(0, 0, active_w, active_h);
            continue;
        }

        uint32_t first_row = active_h;
        uint32_t last_row  = 0;
        for (uint32_t row = 0; row < active_h; row++) {
            uint8_t *current = front + (size_t)row * active_row;
            uint8_t *old     = snapshot + (size_t)row * active_row;
            if (!memcmp(current, old, active_row)) continue;
            memcpy(old, current, active_row);
            if (first_row == active_h) first_row = row;
            last_row = row;
        }
        if (first_row != active_h) flush(0, first_row, active_w, last_row - first_row + 1);
    }
}

void video_start_refresh_worker(void)
{
    spin_lock(&video_state_lock);
    bool should_start = video_flush_cb && !video_refresh_worker_started;
    if (should_start) video_refresh_worker_started = true;
    spin_unlock(&video_state_lock);
    if (should_start && !kthread_create("virtgpu-refresh", video_refresh_worker, NULL)) {
        spin_lock(&video_state_lock);
        video_refresh_worker_started = false;
        spin_unlock(&video_state_lock);
    }
}

/* Initialize Video */
void video_init(void)
{
    struct limine_framebuffer *framebuffer = get_framebuffer();

    memset(&video_active_info, 0, sizeof(video_active_info));
    /*
     * Keep color conversion and the later DRM handoff deterministic even on
     * systems whose firmware does not expose a GOP framebuffer.
     */
    video_active_info.bpp              = 32;
    video_active_info.memory_model     = 1;
    video_active_info.red_mask_size    = 8;
    video_active_info.red_mask_shift   = 16;
    video_active_info.green_mask_size  = 8;
    video_active_info.green_mask_shift = 8;
    video_active_info.blue_mask_size   = 8;
    video_active_info.blue_mask_shift  = 0;
    video_generation                   = 1;

    if (!framebuffer) {
        buffer = NULL;
        width = height = stride = 0;
        fbcon_init();
        return;
    }

    buffer = framebuffer->address;
    width  = framebuffer->width;
    height = framebuffer->height;
    stride = framebuffer->pitch / (framebuffer->bpp / 8);

    video_active_info.framebuffer      = framebuffer->address;
    video_active_info.width            = framebuffer->width;
    video_active_info.height           = framebuffer->height;
    video_active_info.stride           = stride;
    video_active_info.bpp              = framebuffer->bpp;
    video_active_info.memory_model     = framebuffer->memory_model;
    video_active_info.red_mask_size    = framebuffer->red_mask_size;
    video_active_info.red_mask_shift   = framebuffer->red_mask_shift;
    video_active_info.green_mask_size  = framebuffer->green_mask_size;
    video_active_info.green_mask_shift = framebuffer->green_mask_shift;
    video_active_info.blue_mask_size   = framebuffer->blue_mask_size;
    video_active_info.blue_mask_shift  = framebuffer->blue_mask_shift;
    video_active_info.edid_size        = framebuffer->edid_size;
    video_active_info.edid             = framebuffer->edid;
    fbcon_init();
    video_clear();
}

/* Clear screen */
void video_clear(void)
{
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    if (buffer)
        for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    cx = cy = 0;
    video_flush_rect(0, 0, (uint32_t)width, (uint32_t)height);
}

/* Clear screen with color */
void video_clear_color(uint32_t color)
{
    back_color = color;
    if (buffer)
        for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    cx = cy = 0;
    video_flush_rect(0, 0, (uint32_t)width, (uint32_t)height);
}

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!buffer || x >= stride || y >= height) return;
    (buffer)[y * stride + x] = color;
}

/* Get a pixel at the specified coordinates on the screen */
uint32_t video_get_pixel(uint32_t x, uint32_t y)
{
    if (!buffer || x >= stride || y >= height) return 0;
    return (buffer)[y * stride + x];
}

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p))
{
    position_t p;
    for (p.y = p0.y; p.y <= p1.y; p.y++)
        for (p.x = p0.x; p.x <= p1.x; p.x++) callback(p);
}

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position_t p0, position_t p1, uint32_t color)
{
    uint32_t x0 = p0.x;
    uint32_t y0 = p0.y;
    uint32_t x1 = p1.x;
    uint32_t y1 = p1.y;
    if (!buffer || x0 >= stride || y0 >= height) return;
    if (x1 >= stride) x1 = (uint32_t)stride - 1;
    if (y1 >= height) y1 = (uint32_t)height - 1;
    if (x1 < x0 || y1 < y0) return;
    for (uint32_t y = y0; y <= y1; y++) {
        /* Draw horizontal line */
#if defined(__x86_64__) || defined(__i386__)
        uint32_t *line  = buffer + y * stride + x0;
        size_t    count = x1 - x0 + 1;
        __asm__ volatile("rep stosl" : "+D"(line), "+c"(count) : "a"(color) : "memory");
#else
        for (uint32_t x = x0; x <= x1; x++) video_draw_pixel(x, y, color);
#endif
    }
    video_flush_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

void video_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    spin_lock(&video_state_lock);
    video_flush_fn_t flush    = video_flush_cb;
    uint32_t         active_w = (uint32_t)width;
    uint32_t         active_h = (uint32_t)height;
    spin_unlock(&video_state_lock);
    if (!flush || !w || !h || x >= active_w || y >= active_h) return;
    if (w > active_w - x) w = active_w - x;
    if (h > active_h - y) h = active_h - y;
    flush(x, y, w, h);
}

/*
 * video_switch_framebuffer - redirect fbcon output to a DRM GEM backing buffer.
 *
 * After this call all printk / tty output renders into the DRM buffer
 * instead of the boot-time Limine framebuffer.  The @flush callback is
 * invoked after each batch draw to push pixels to the host GPU.
 *
 * When the new resolution matches the boot framebuffer the previous frame
 * (boot logo and everything already rendered) is carried over pixel-for-
 * pixel into the new buffer: no clear, no logo redraw.  The console just
 * continues on the DRM surface and the logo is eventually covered by
 * normal scrolling after fbcon_release_logo().
 */
void video_switch_framebuffer(void *backing, uint32_t w, uint32_t h, uint32_t pitch, video_flush_fn_t flush)
{
    uint32_t *old_buffer;
    uint64_t  old_width;
    uint64_t  old_height;
    uint64_t  old_stride;

    if (!backing || !flush) return;

    if ((uintptr_t)backing & (PAGE_4K_SIZE - 1) || pitch < w * sizeof(uint32_t) || (pitch & (sizeof(uint32_t) - 1))) return;

    tty_buff_flush();
    fbcon_handoff_begin();

    spin_lock(&video_state_lock);
    old_buffer = buffer;
    old_width  = width;
    old_height = height;
    old_stride = stride;

    buffer                             = (uint32_t *)backing;
    width                              = w;
    height                             = h;
    stride                             = pitch / sizeof(uint32_t); // pixels per line
    video_flush_cb                     = flush;
    video_active_info.framebuffer      = backing;
    video_active_info.width            = w;
    video_active_info.height           = h;
    video_active_info.stride           = stride;
    video_active_info.bpp              = 32;
    video_active_info.memory_model     = 1;
    video_active_info.red_mask_size    = 8;
    video_active_info.red_mask_shift   = 16;
    video_active_info.green_mask_size  = 8;
    video_active_info.green_mask_shift = 8;
    video_active_info.blue_mask_size   = 8;
    video_active_info.blue_mask_shift  = 0;
    video_active_info.edid_size        = 0;
    video_active_info.edid             = NULL;
    video_generation++;
    spin_unlock(&video_state_lock);

    /*
     * Rebuild the fbcon character grid for the new resolution.  When the
     * resolution is unchanged the grids and cursor state are preserved.
     */
    fbcon_resize();

    if (old_buffer && old_width == w && old_height == h) {
        /*
         * Seamless handoff: move the previous frame into the new buffer so
         * the logo and all boot messages survive without being redrawn.
         */
        if (old_stride == stride) {
            memcpy(buffer, old_buffer, (size_t)stride * h * sizeof(uint32_t));
        } else {
            for (uint32_t y = 0; y < h; y++)
                memcpy(buffer + (size_t)y * stride, old_buffer + (size_t)y * old_stride, (size_t)w * sizeof(uint32_t));
        }
    } else {
        /* Resolution changed: rebuild the display from scratch. */
        video_clear();
#if BOOT_LOGO
        video_redraw_logo();
#endif
    }

    fbcon_handoff_end();

    /* Push the carried-over frame (or the rebuilt one) to the host. */
    video_flush_rect(0, 0, w, h);

    plogk("video: Switched to framebuffer %ux%u stride=%u\n", w, h, (uint32_t)stride);
}
