/*
 *
 *      fb_sysfs.c
 *      Framebuffer sysfs integration.
 *
 *      2026/8/2 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/gpu/fbdev/fbdev.h>
#include <drivers/gpu/fbdev/video.h>
#include <fs/sysfs/fb_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/string.h>

static struct bus_type framebuffer_platform_bus = {.name = "platform"};
static struct device   framebuffer_platform_device;
static struct device  *framebuffer_class_device;
static bool            framebuffer_sysfs_ready;

/* Refresh rate of the fixed native mode, matching the fbcon var setup. */
#define FB_DEFAULT_REFRESH 60

/*
 * Linux fbsysfs.c mode_string(): "%c:%dx%d%c-%d\n" with the mode flag
 * ('U' user, 'D' detailed, 'V' VESA, 'S' standard) and the scan type
 * ('p' progressive, 'i' interlaced, 'd' double). The fixed console has one
 * native user mode.
 */
static int fb_mode_string(char *buf, uint64_t width, uint64_t height)
{
    return sysfs_emit(buf, "U:%llux%llup-%d\n", (unsigned long long)width, (unsigned long long)height, FB_DEFAULT_REFRESH);
}

/* Parse a leading unsigned decimal up to the first non-digit / newline. */
static int fb_parse_uint(const char *buf, size_t count, uint64_t *out)
{
    char   tmp[32];
    size_t len = 0;
    char  *end;

    if (!buf || !out) return -EINVAL;
    while (len < count && len < sizeof(tmp) - 1 && buf[len] >= '0' && buf[len] <= '9') {
        tmp[len] = buf[len];
        len++;
    }
    if (!len) return -EINVAL;
    tmp[len] = '\0';
    {
        int64_t v = strtol(tmp, &end, 10);
        if (v < 0) return -EINVAL;
        *out = (uint64_t)v;
    }
    return 0;
}

/* Match a write buffer against a NUL-terminated token (newline tolerant). */
static bool fb_sysfs_match(const char *buf, size_t count, const char *token)
{
    size_t token_len;

    if (!buf || !token) return false;
    while (count && (buf[count - 1] == '\n' || buf[count - 1] == '\r' || buf[count - 1] == ' ' || buf[count - 1] == '\t')) count--;
    token_len = strlen(token);
    return count == token_len && memcmp(buf, token, token_len) == 0;
}

/* Show the fixed framebuffer device name (matches fix.id from FBIOGET_FSCREENINFO). */
static ssize_t fb_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    char id[16];
    (void)dev;
    (void)attr;
    video_fix_id(id, sizeof(id));
    return sysfs_emit(buf, "%s\n", id);
}

/* Show the framebuffer stride in bytes. */
static ssize_t fb_stride_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    video_info_t info = video_get_info();
    return sysfs_emit(buf, "%llu\n", (unsigned long long)info.stride * (info.bpp / 8));
}

/* Show the framebuffer bits-per-pixel. */
static ssize_t fb_bpp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "%u\n", video_get_info().bpp);
}

/* Accept a requested bits-per-pixel (fixed console: no-op). */
static ssize_t fb_bpp_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    uint64_t value;
    (void)dev;
    (void)attr;
    if (fb_parse_uint(buf, count, &value)) return -EINVAL;
    return (ssize_t)count;
}

/* Show the framebuffer virtual resolution as "xres,yres" (). */
static ssize_t fb_virtual_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    video_info_t info = video_get_info();
    return sysfs_emit(buf, "%llu,%llu\n", (unsigned long long)info.width, (unsigned long long)info.height);
}

/* Accept a requested virtual resolution (fixed console: no-op). */
static ssize_t fb_virtual_size_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    uint64_t x = 0, y = 0;
    size_t   i = 0;
    (void)dev;
    (void)attr;

    while (i < count && buf[i] >= '0' && buf[i] <= '9') {
        x = x * 10 + (uint64_t)(buf[i] - '0');
        i++;
    }
    if (!i) return -EINVAL;
    while (i < count && (buf[i] == ',' || buf[i] == ' ' || buf[i] == 'x')) i++;
    if (i >= count) return -EINVAL;
    while (i < count && buf[i] >= '0' && buf[i] <= '9') {
        y = y * 10 + (uint64_t)(buf[i] - '0');
        i++;
    }
    (void)x;
    (void)y;
    return (ssize_t)count;
}

/* Show the single available video mode in Linux mode_string() format. */
static ssize_t fb_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    video_info_t info = video_get_info();
    return fb_mode_string(buf, info.width, info.height);
}

/* Accept a mode list write (fixed console: no-op). */
static ssize_t fb_modes_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    (void)dev;
    (void)attr;
    (void)buf;
    return (ssize_t)count;
}

/* Show the current mode (Linux show_mode(): one mode_string()). */
static ssize_t fb_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    video_info_t info = video_get_info();
    return fb_mode_string(buf, info.width, info.height);
}

/* Accept the current mode if it matches the native mode string. */
static ssize_t fb_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    video_info_t info = video_get_info();
    char         expected[64];
    int          n;
    size_t       elen;

    (void)dev;
    (void)attr;

    n = fb_mode_string(expected, info.width, info.height);
    if (n <= 0) return -EINVAL;
    elen = (size_t)n;
    if (elen && expected[elen - 1] == '\n') elen--;
    /*
     * NUL-terminate at the trimmed length so the match compares the mode
     * string without its trailing newline (mirroring the trimmed input).
     */
    expected[elen] = '\0';
    if (!fb_sysfs_match(buf, count, expected)) return -EINVAL;
    return (ssize_t)count;
}

/* Show the blank state ("0" = unblanked). */
static ssize_t fb_blank_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "0\n");
}

/* Accept a blank request (fixed console: no-op). */
static ssize_t fb_blank_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    uint64_t value;
    (void)dev;
    (void)attr;
    if (fb_parse_uint(buf, count, &value)) return -EINVAL;
    (void)value;
    return (ssize_t)count;
}

/* Show the pan offset (no panning on the fixed console). */
static ssize_t fb_pan_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "0,0\n");
}

/* Accept a pan request (fixed console: no-op). */
static ssize_t fb_pan_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    (void)dev;
    (void)attr;
    (void)buf;
    return (ssize_t)count;
}

/* Show the rotation state (no rotation). */
static ssize_t fb_rotate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "0\n");
}

/* Accept a rotation request (fixed console: no-op). */
static ssize_t fb_rotate_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    uint64_t value;
    (void)dev;
    (void)attr;
    if (fb_parse_uint(buf, count, &value)) return -EINVAL;
    (void)value;
    return (ssize_t)count;
}

/* Show the framebuffer state (FBINFO_STATE_RUNNING = 0). */
static ssize_t fb_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "0\n");
}

/* Accept a state suspend/resume request (fixed console: no-op). */
static ssize_t fb_state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    uint64_t value;
    (void)dev;
    (void)attr;
    if (fb_parse_uint(buf, count, &value)) return -EINVAL;
    (void)value;
    return (ssize_t)count;
}

/* Console / cursor toggles: keeps both empty no-ops. */
static ssize_t fb_console_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    (void)buf;
    return 0;
}

static ssize_t fb_console_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    (void)dev;
    (void)attr;
    (void)buf;
    (void)count;
    return 0;
}

static ssize_t fb_cursor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    (void)buf;
    return 0;
}

static ssize_t fb_cursor_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    (void)dev;
    (void)attr;
    (void)buf;
    (void)count;
    return 0;
}

static DEVICE_ATTR(name, 0444, fb_name_show, NULL);
static DEVICE_ATTR(stride, 0444, fb_stride_show, NULL);
static DEVICE_ATTR(bits_per_pixel, 0644, fb_bpp_show, fb_bpp_store);
static DEVICE_ATTR(virtual_size, 0644, fb_virtual_size_show, fb_virtual_size_store);
static DEVICE_ATTR(modes, 0644, fb_modes_show, fb_modes_store);
static DEVICE_ATTR(mode, 0644, fb_mode_show, fb_mode_store);
static DEVICE_ATTR(blank, 0644, fb_blank_show, fb_blank_store);
static DEVICE_ATTR(pan, 0644, fb_pan_show, fb_pan_store);
static DEVICE_ATTR(rotate, 0644, fb_rotate_show, fb_rotate_store);
static DEVICE_ATTR(state, 0644, fb_state_show, fb_state_store);
static DEVICE_ATTR(console, 0644, fb_console_show, fb_console_store);
static DEVICE_ATTR(cursor, 0644, fb_cursor_show, fb_cursor_store);

static struct attribute *framebuffer_attributes[] = {
    &dev_attr_name.attr,
    &dev_attr_stride.attr,
    &dev_attr_bits_per_pixel.attr,
    &dev_attr_virtual_size.attr,
    &dev_attr_modes.attr,
    &dev_attr_mode.attr,
    &dev_attr_blank.attr,
    &dev_attr_pan.attr,
    &dev_attr_rotate.attr,
    &dev_attr_state.attr,
    &dev_attr_console.attr,
    &dev_attr_cursor.attr,
    NULL,
};

static struct attribute_group framebuffer_group = {
    .attrs = framebuffer_attributes,
};

static const struct attribute_group *framebuffer_groups[] = {
    &framebuffer_group,
    NULL,
};

static struct class graphics_class = {.name = "graphics", .dev_groups = framebuffer_groups};

/* Register the framebuffer class device on the platform bus. */
void fb_sysfs_init(void)
{
#if CONFIG_SYSFS
    int status;

    if (framebuffer_sysfs_ready) return;
    status = bus_register(&framebuffer_platform_bus);
    if (status != EOK) {
        plogk("fb_sysfs: Platform bus registration failed: %d\n", status);
        return;
    }
    status = class_register(&graphics_class);
    if (status != EOK) {
        plogk("fb_sysfs: Graphics class registration failed: %d\n", status);
        bus_unregister(&framebuffer_platform_bus);
        return;
    }

    memset(&framebuffer_platform_device, 0, sizeof(framebuffer_platform_device));
    framebuffer_platform_device.bus   = &framebuffer_platform_bus;
    framebuffer_platform_device.devid = 0;
    if (kobject_set_name(&framebuffer_platform_device.kobj, "drm-framebuffer") != EOK || device_register(&framebuffer_platform_device) != EOK) {
        plogk("fb_sysfs: Physical framebuffer registration failed.\n");
        class_unregister(&graphics_class);
        bus_unregister(&framebuffer_platform_bus);
        return;
    }

    framebuffer_class_device = device_create(&graphics_class, &framebuffer_platform_device, MKDEV(FB_MAJOR, 0), NULL, "fb0");
    if (!framebuffer_class_device) {
        plogk("fb_sysfs: Fb0 class device registration failed.\n");
        device_unregister(&framebuffer_platform_device);
        class_unregister(&graphics_class);
        bus_unregister(&framebuffer_platform_bus);
        return;
    }
    framebuffer_sysfs_ready = true;
    plogk("fb_sysfs: registered /sys/class/graphics/fb0\n");
#endif
}