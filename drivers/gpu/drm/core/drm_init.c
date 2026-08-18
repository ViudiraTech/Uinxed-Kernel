/*
 *
 *      drm_init.c
 *      DRM subsystem initialization entry point
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/gpu/drm/drm.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_print.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <process/process.h>
#include <syscall/fcntl.h>

/* Global DRM device list (replaces singleton) */

static struct drm_device *drm_device_list[DRM_MAX_DEVICES];
static spinlock_t         drm_device_list_lock = {.lock = 0, .rflags = 0};

/* Register a device in the global DRM device list. */
void drm_device_list_add(struct drm_device *dev)
{
    bool added = false;

    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (!drm_device_list[i]) {
            drm_device_list[i] = dev;
            added              = true;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
    if (!added) DRM_ERROR("Device list full (%d slots), dropping device %s\n", DRM_MAX_DEVICES, dev && dev->driver && dev->driver->name ? dev->driver->name : "?");
}

/* Remove a device from the global DRM device list. */
void drm_device_list_remove(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i] == dev) {
            drm_device_list[i] = NULL;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

/*
 * Return the name of the DRM driver bound to the primary display (card0),
 * or the first registered device's driver name, or NULL when no DRM device
 * is registered.  Used to derive the fbdev identifier
 * "<driver>drmfb".  The driver structs are static and never freed, so the
 * returned pointer stays valid.
 */
const char *drm_active_driver_name(void)
{
    /*
     * The binding is fixed once any DRM device registers (at boot). Cache
     * the resolved driver name as a POINTER to the static driver string:
     * pointer-sized stores are atomic (no torn copy) and the string outlives
     * every caller. Only a still-empty device list re-scans, so early
     * callers (before GPU probing) still pick up the driver once it appears
     * without holding the list lock on every FBIOGET_FSCREENINFO /
     * /sys/class/graphics/fb0/name read.
     */
    static const char *cached;
    const char        *name = NULL;

    if (cached) return cached;

    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];
        if (!dev || !dev->driver || !dev->driver->name) continue;
        if (dev->primary && dev->primary->index == 0) {
            name = dev->driver->name;
            break;
        }
        if (!name) name = dev->driver->name;
    }
    spin_unlock(&drm_device_list_lock);

    if (name && name[0]) cached = name;
    return cached;
}

/*
 * Look up a registered device by its minor type and index.  The returned
 * device holds a caller reference which must be dropped with drm_dev_put().
 * Taking the reference under the list lock keeps the device alive even if
 * a concurrent final drm_dev_put is tearing it down.
 */
struct drm_device *drm_get_device_by_minor(int type, int index)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];
        if (!dev) continue;
        if (type == DRM_MINOR_PRIMARY && dev->primary && dev->primary->index == index) {
            if (drm_dev_get(dev)) {
                spin_unlock(&drm_device_list_lock);
                return dev;
            }
            break; // Minor indices are unique; a dying device is the only match.
        }
        if (type == DRM_MINOR_RENDER && dev->render && dev->render->index == index) {
            if (drm_dev_get(dev)) {
                spin_unlock(&drm_device_list_lock);
                return dev;
            }
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
    return NULL;
}

/*
 * Iterate the registered device list.  @idx starts at 0.  The returned
 * device holds a caller reference which must be dropped with drm_dev_put().
 */
struct drm_device *drm_device_list_iter(int *idx)
{
    struct drm_device *dev = NULL;
    int                i;

    if (!idx || *idx < 0) return NULL;

    spin_lock(&drm_device_list_lock);
    for (i = *idx; i < DRM_MAX_DEVICES; i++) {
        if (!drm_device_list[i]) continue;
        dev = drm_dev_get(drm_device_list[i]);
        if (!dev) continue;
        *idx = i + 1;
        break;
    }
    spin_unlock(&drm_device_list_lock);
    return dev;
}

/*
 * Collect up to @max registered devices into @out, each holding a caller
 * reference which must be dropped with drm_dev_put().  Returns the number
 * collected.  Takes the device list lock once instead of once per device,
 * which matters for per-tick callers like the vblank emulation timer.
 */
int drm_device_list_collect(struct drm_device **out, int max)
{
    int n = 0;

    if (!out || max <= 0) return 0;

    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES && n < max; i++) {
        struct drm_device *dev = drm_device_list[i];
        if (!dev) continue;
        dev = drm_dev_get(dev);
        if (!dev) continue;
        out[n++] = dev;
    }
    spin_unlock(&drm_device_list_lock);
    return n;
}

/* VFS ioctl wrapper: dispatch to the DRM ioctl handler. */

/* VFS read callback: deliver pending DRM events to the caller. */
size_t drm_dev_read(void *file, void *addr, size_t offset, size_t size)
{
    struct drm_file *file_priv = (struct drm_file *)file;
    size_t           position  = offset;

    if (!file_priv) return (size_t)-1;
    int ret = drm_read(file_priv, (char *)addr, size, &position, false);
    return ret < 0 ? (size_t)-1 : (size_t)ret;
}

/* VFS write wrapper for /dev/dri nodes. */
size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

/* VFS ioctl callback: dispatch to the DRM ioctl handler. */
int drm_dev_ioctl(void *file, size_t req, void *arg)
{
    struct drm_device *dev;
    struct drm_file   *file_priv = (struct drm_file *)file;

    if (!file_priv) return -ENODEV;

    /* Route to the device bound to this open file, not a global singleton. */
    dev = (struct drm_device *)file_priv->dev;
    if (!dev) return -ENODEV;

    return drm_ioctl(dev, (unsigned int)req, arg, file_priv);
}

/*
 * Parse a strictly numeric node suffix. Returns -1 for an empty,
 * non-numeric, or overflowing suffix instead of atoi()'s silent 0.
 */
static int drm_parse_minor(const char *s)
{
    int minor = 0;

    if (!s || !*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        minor = minor * 10 + (*s - '0');
        if (minor < 0) return -1; /* signed overflow */
    }
    return minor;
}

/*
 * Resolve the DRM device that owns a /dev/dri node from its name.  Nodes
 * are named "cardN" (primary minor N) or "renderD128+N" (render minor N)
 * by drm_dev_register(); no single-device assumption is made here.  The
 * returned device holds a caller reference which must be dropped with
 * drm_dev_put().
 */
static struct drm_device *drm_device_from_node(void *node_ptr)
{
    vfs_node_t node = (vfs_node_t)node_ptr;
    int        minor;

    if (!node || !node->name || !node->name[0]) return NULL;

    if (!strncmp(node->name, "card", 4)) {
        minor = drm_parse_minor(node->name + 4);
        if (minor < 0) return NULL;
        return drm_get_device_by_minor(DRM_MINOR_PRIMARY, minor);
    }
    if (!strncmp(node->name, "renderD", 7)) {
        minor = drm_parse_minor(node->name + 7);
        if (minor < 128) return NULL;
        return drm_get_device_by_minor(DRM_MINOR_RENDER, minor - 128);
    }
    return NULL;
}

/*
 * tmpfs/devtmpfs per-open bridge. A VFS node is shared by all processes, so
 * storing drm_file in node->handle is incorrect: one close could release
 * another client's state.
 */
int drm_dev_open(void *node_ptr, uint64_t flags, void **private_data)
{
    struct drm_device *dev;
    struct drm_file   *file;
    int                ret;

    (void)flags;
    if (!private_data) return -EINVAL;
    *private_data = NULL;

    /* Bind the open to whichever GPU registered this node. */
    dev = drm_device_from_node(node_ptr);
    if (!dev) {
        DRM_ERROR("Open: no DRM device owns node \"%s\"\n", node_ptr && ((vfs_node_t)node_ptr)->name ? ((vfs_node_t)node_ptr)->name : "?");
        return -ENODEV;
    }
    file = malloc(sizeof(*file));
    if (!file) {
        DRM_ERROR("Open: out of memory allocating drm_file.\n");
        drm_dev_put(dev);
        return -ENOMEM;
    }
    memset(file, 0, sizeof(*file));
    ret = drm_open(dev, file);

    /* Drop the lookup reference; the file holds its own via drm_open(). */
    drm_dev_put(dev);
    if (ret) {
        DRM_ERROR("Open: drm_open failed (ret=%d)\n", ret);
        free(file);
        return ret;
    }

    /*
     * A root compositor opening the primary node is already trusted for
     * DRM_AUTH ioctls.  Weston performs GETRESOURCES immediately after the
     * open (before issuing SET_MASTER); leaving this bit clear makes the
     * otherwise valid KMS device look absent to its DRM backend.  Render
     * nodes intentionally keep the normal unauthenticated state.
     */
    vfs_node_t node = (vfs_node_t)node_ptr;
    process_t *proc = process_current();
    if (node && node->name && !strncmp(node->name, "card", 4) && proc && proc->uid == 0) file->authenticated = true;
    /*
     * drm_send_event() uses this stable device node to wake the VFS poll
     * source watched by Weston's epoll loop.  Event readiness itself remains
     * per-open and is checked through drm_poll(file, ...).
     */
    file->filp    = node;
    *private_data = file;
    return 0;
}

/* VFS release callback for /dev/dri nodes. */
void drm_dev_release(void *node_ptr, void *private_data)
{
    (void)node_ptr;
    /* drm_release() tears down and frees the drm_file itself. */
    if (private_data) drm_release((struct drm_file *)private_data);
}

/* VFS ioctl wrapper for /dev/dri files. */
int drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    (void)ctx;
    (void)flags;
    return drm_dev_ioctl(private_data, req, arg);
}

/* VFS read wrapper for /dev/dri files. */
int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    size_t position = offset;
    return drm_read((struct drm_file *)private_data, (char *)addr, size, &position, (flags & O_NONBLOCK) != 0);
}

/* VFS write wrapper for /dev/dri files. */
int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;
    return (int64_t)drm_dev_write(private_data, addr, offset, size);
}

/* VFS poll wrapper for /dev/dri files. */
int drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;
    return drm_dev_poll(private_data, events);
}

/* VFS poll callback for /dev/dri nodes. */
int drm_dev_poll(void *file, size_t events)
{
    return (int)drm_poll((struct drm_file *)file, (unsigned int)events);
}

/* VMA teardown hook: drop the mapping-held GEM reference. */
static void drm_gem_vma_put(void *data)
{
    drm_gem_object_put((struct drm_gem_object *)data);
}

/* VMA fork-copy hook: take an extra reference for the child mapping. */
static void drm_gem_vma_get(void *data)
{
    drm_gem_object_get((struct drm_gem_object *)data);
}

/* DRM per-open mmap callback (VMA-aware GEM mmap) */
void *drm_dev_file_mmap(void *ctx, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    struct drm_device     *dev       = (struct drm_device *)ctx;
    struct drm_file       *file_priv = (struct drm_file *)private_data;
    struct drm_gem_object *obj;
    size_t                 map_len;

    (void)flags;

    /* ctx carries the owning device; fall back to the file's bound device. */
    if (!dev) dev = file_priv ? (struct drm_device *)file_priv->dev : NULL;
    if (!dev || !file_priv || !vma) return NULL;

    /* Look up the GEM object by its mmap offset. */
    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);
    if (!obj) {
        DRM_WARN("File_mmap: no GEM object for offset 0x%llx\n", (unsigned long long)offset);
        return NULL;
    }
    if (!obj->backing) {
        drm_gem_object_put(obj);
        DRM_WARN("File_mmap: no GEM backing for offset 0x%llx\n", (unsigned long long)offset);
        return NULL;
    }

    /*
     * Clamp the mapping to the object's full size a huge mmap
     * length must not expose physical memory past the buffer.  Backing is
     * page-rounded (see drm_gem_dumb_create / virtgpu_gem_alloc_object), so
     * ALIGN_UP(obj->size) is always mappable; rounding DOWN would leave the
     * final partial page of a non-4K buffer unmapped (user page fault) and
     * would reject sub-page buffers outright.
     */
    map_len = size;
    if (map_len > ALIGN_UP(obj->size, PAGE_4K_SIZE)) map_len = ALIGN_UP(obj->size, PAGE_4K_SIZE);
    if (!map_len) {
        drm_gem_object_put(obj);
        return NULL;
    }
    vma->end = vma->start + map_len;

    /*
     * Keep the GEM object alive for the lifetime of the mapping.  The
     * VMA teardown path (vm_area_free) calls vm_private_put() to drop
     * this reference when the mapping is unmapped or the process exits.
     */
    vma->vm_private_data = obj;
    vma->vm_private_put  = drm_gem_vma_put;
    vma->vm_private_get  = drm_gem_vma_get;

    /*
     * Identity-mapped physical memory: return the backing pointer.
     * The syscall mmap layer handles PTE creation using this pointer.
     */
    return obj->backing;
}
