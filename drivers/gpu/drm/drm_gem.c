/*
 *
 *      drm_gem.c
 *      DRM GEM object management
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drm/drm_device.h>
#include <drm/drm_mode.h>
#include <drm/drm_idr.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <alloc.h>
#include <errno.h>
#include <string.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Global GEM name table (simple counter-based)                        */
/* ------------------------------------------------------------------ */

#define GEM_MAX_NAMES 1024

static struct gem_name_entry {
    uint32_t             name;
    struct drm_gem_object *obj;
} gem_name_table[GEM_MAX_NAMES];

static uint32_t gem_name_counter = 1;
static spinlock_t gem_name_lock = { .lock = 0, .rflags = 0 };

/* ------------------------------------------------------------------ */
/* Lookup a GEM object by global flink name                            */
/* ------------------------------------------------------------------ */

static struct drm_gem_object *gem_find_by_name(uint32_t name)
{
    int i;

    for (i = 0; i < GEM_MAX_NAMES; i++) {
        if (gem_name_table[i].name == name && gem_name_table[i].obj) {
            return gem_name_table[i].obj;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Allocate a global flink name and store the object                   */
/* ------------------------------------------------------------------ */

static int gem_alloc_name(struct drm_gem_object *obj, uint32_t *name_out)
{
    int i;

    spin_lock(&gem_name_lock);

    /* Find free slot */
    for (i = 0; i < GEM_MAX_NAMES; i++) {
        if (gem_name_table[i].obj == NULL) {
            break;
        }
    }

    if (i >= GEM_MAX_NAMES) {
        spin_unlock(&gem_name_lock);
        return -ENOMEM;
    }

    gem_name_table[i].name = gem_name_counter;
    gem_name_table[i].obj = obj;
    *name_out = gem_name_counter;
    gem_name_counter++;

    spin_unlock(&gem_name_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Free a global flink name entry                                      */
/* ------------------------------------------------------------------ */

static void __attribute__((unused)) gem_free_name(uint32_t name)
{
    int i;

    spin_lock(&gem_name_lock);

    for (i = 0; i < GEM_MAX_NAMES; i++) {
        if (gem_name_table[i].name == name) {
            gem_name_table[i].name = 0;
            gem_name_table[i].obj = NULL;
            break;
        }
    }

    spin_unlock(&gem_name_lock);
}

/* ------------------------------------------------------------------ */
/* drm_gem_object_init: initialize a GEM object                        */
/* ------------------------------------------------------------------ */

int drm_gem_object_init(struct drm_device *dev,
                         struct drm_gem_object *obj, size_t size)
{
    if (!obj) {
        return -EINVAL;
    }

    obj->dev = dev;
    obj->size = (uint32_t)size;
    obj->refcount = 1;
    obj->ref_lock.lock = 0;
    obj->ref_lock.rflags = 0;
    obj->handle_count = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_private_object_init: same as drm_gem_object_init             */
/* ------------------------------------------------------------------ */

int drm_gem_private_object_init(struct drm_device *dev,
                                 struct drm_gem_object *obj, size_t size)
{
    return drm_gem_object_init(dev, obj, size);
}

/* ------------------------------------------------------------------ */
/* drm_gem_object_get: increment refcount                               */
/* ------------------------------------------------------------------ */

void drm_gem_object_get(struct drm_gem_object *obj)
{
    if (!obj) {
        return;
    }

    spin_lock(&obj->ref_lock);
    obj->refcount++;
    spin_unlock(&obj->ref_lock);
}

/* ------------------------------------------------------------------ */
/* drm_gem_object_put: decrement refcount, free if zero                 */
/* ------------------------------------------------------------------ */

void drm_gem_object_put(struct drm_gem_object *obj)
{
    int refcount;

    if (!obj) {
        return;
    }

    spin_lock(&obj->ref_lock);
    refcount = --obj->refcount;
    spin_unlock(&obj->ref_lock);

    if (refcount == 0) {
        struct drm_device *dev = obj->dev;

        /* Free PRIME fd if assigned */
        if (obj->prime_fd > 0) {
            drm_gem_prime_fd_free(obj->prime_fd);
            obj->prime_fd = -1;
        }

        /* Call driver's free hook if available */
        if (dev && dev->driver && dev->driver->gem_free_object) {
            dev->driver->gem_free_object(obj);
        } else {
            free(obj->backing);
            free(obj);
        }
    }
}

/* ------------------------------------------------------------------ */
/* drm_gem_handle_create: create a per-file handle for a GEM object    */
/* ------------------------------------------------------------------ */

int drm_gem_handle_create(struct drm_file *file_priv,
                           struct drm_gem_object *obj,
                           uint32_t *handle_out)
{
    int ret;

    if (!file_priv || !obj || !handle_out) {
        return -EINVAL;
    }

    spin_lock(&file_priv->table_lock);

    ret = drm_idr_alloc(&file_priv->object_idr, obj, 1, 0, handle_out);
    if (ret < 0) {
        spin_unlock(&file_priv->table_lock);
        return ret;
    }

    spin_unlock(&file_priv->table_lock);

    obj->handle_count++;

    /* Add to file's object list */
    ilist_insert_after(&file_priv->object_list, &obj->handle_list_node);

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_handle_delete: delete a per-file handle                      */
/* ------------------------------------------------------------------ */

int drm_gem_handle_delete(struct drm_file *file_priv, uint32_t handle)
{
    struct drm_gem_object *obj;

    if (!file_priv) {
        return -EINVAL;
    }

    spin_lock(&file_priv->table_lock);

    obj = drm_idr_remove(&file_priv->object_idr, handle);

    spin_unlock(&file_priv->table_lock);

    if (obj) {
        ilist_remove(&obj->handle_list_node);
        obj->handle_count--;
        drm_gem_object_put(obj);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_object_lookup: find a GEM object by handle                  */
/* ------------------------------------------------------------------ */

struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file_priv,
                                              uint32_t handle)
{
    struct drm_gem_object *obj;

    if (!file_priv) {
        return NULL;
    }

    spin_lock(&file_priv->table_lock);

    obj = drm_idr_find(&file_priv->object_idr, handle);
    if (obj) {
        drm_gem_object_get(obj);
    }

    spin_unlock(&file_priv->table_lock);

    return obj;
}

/* ------------------------------------------------------------------ */
/* drm_gem_open_ioctl: handle DRM_IOCTL_GEM_OPEN                        */
/* ------------------------------------------------------------------ */

int drm_gem_open_ioctl(struct drm_device *dev, void *data,
                        struct drm_file *file_priv)
{
    struct drm_gem_open *args = (struct drm_gem_open *)data;
    struct drm_gem_object *obj;
    uint32_t handle;
    int ret;

    (void)dev;

    /* Look up by global flink name */
    spin_lock(&gem_name_lock);
    obj = gem_find_by_name(args->name);
    if (obj) {
        drm_gem_object_get(obj);
    }
    spin_unlock(&gem_name_lock);

    if (!obj) {
        return -ENOENT;
    }

    ret = drm_gem_handle_create(file_priv, obj, &handle);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    args->handle = handle;
    args->size = obj->size;

    drm_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_close_ioctl: handle DRM_IOCTL_GEM_CLOSE                      */
/* ------------------------------------------------------------------ */

int drm_gem_close_ioctl(struct drm_device *dev, void *data,
                         struct drm_file *file_priv)
{
    struct drm_gem_close *args = (struct drm_gem_close *)data;

    (void)dev;

    return drm_gem_handle_delete(file_priv, args->handle);
}

/* ------------------------------------------------------------------ */
/* drm_gem_flink_ioctl: handle DRM_IOCTL_GEM_FLINK                      */
/* ------------------------------------------------------------------ */

int drm_gem_flink_ioctl(struct drm_device *dev, void *data,
                         struct drm_file *file_priv)
{
    struct drm_gem_flink *args = (struct drm_gem_flink *)data;
    struct drm_gem_object *obj;
    uint32_t name;
    int ret;

    (void)dev;

    obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!obj) {
        return -ENOENT;
    }

    ret = gem_alloc_name(obj, &name);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    args->name = name;

    drm_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_dumb_create: handle DRM_IOCTL_MODE_CREATE_DUMB               */
/* ------------------------------------------------------------------ */

int drm_gem_dumb_create(struct drm_file *file_priv,
                         struct drm_device *dev,
                         struct drm_mode_create_dumb *args)
{
    struct drm_gem_object *obj;
    uint32_t handle;
    size_t size;
    int ret;
    int bpp;

    if (!file_priv || !dev || !args) {
        return -EINVAL;
    }

    /* Validate parameters */
    if (args->width == 0 || args->height == 0 || args->bpp == 0) {
        return -EINVAL;
    }

    bpp = args->bpp;
    if (bpp % 8 != 0) {
        return -EINVAL;
    }

    /* Calculate pitch and size */
    args->pitch = args->width * (bpp / 8);
    size = (size_t)args->pitch * args->height;
    args->size = (uint64_t)size;

    /* Allocate GEM object */
    obj = malloc(sizeof(*obj));
    if (!obj) {
        return -ENOMEM;
    }
    memset(obj, 0, sizeof(*obj));

    drm_gem_object_init(dev, obj, size);
    obj->size = (uint32_t)size;

    /* Allocate backing memory for the dumb buffer */
    if (size > 0) {
        obj->backing = aligned_alloc(4096, size);
        if (!obj->backing) {
            free(obj);
            return -ENOMEM;
        }
        memset(obj->backing, 0, size);
    }

    obj->prime_fd = -1;

    /* Create handle for userspace */
    ret = drm_gem_handle_create(file_priv, obj, &handle);
    if (ret < 0) {
        free(obj->backing);
        free(obj);
        return ret;
    }

    args->handle = handle;

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_dumb_map_offset: handle DRM_IOCTL_MODE_MAP_DUMB              */
/* ------------------------------------------------------------------ */

int drm_gem_dumb_map_offset(struct drm_file *file_priv,
                             struct drm_device *dev,
                             uint32_t handle,
                             uint64_t *offset)
{
    struct drm_gem_object *obj;

    (void)dev;

    obj = drm_gem_object_lookup(file_priv, handle);
    if (!obj) {
        return -ENOENT;
    }

    /* Return the handle as the mmap offset (unique per buffer).
     * A future DRM mmap implementation would use this to look up
     * the backing memory and map it into userspace. */
    *offset = (uint64_t)handle;

    drm_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_dumb_destroy: handle DRM_IOCTL_MODE_DESTROY_DUMB             */
/* ------------------------------------------------------------------ */

int drm_gem_dumb_destroy(struct drm_file *file_priv,
                          struct drm_device *dev,
                          uint32_t handle)
{
    (void)dev;

    return drm_gem_handle_delete(file_priv, handle);
}

/* ------------------------------------------------------------------ */
/* PRIME fd table: maps integer PRIME fds to GEM objects               */
/* ------------------------------------------------------------------ */

#define PRIME_FD_MAX 1024

static struct {
    struct drm_gem_object *obj;
    int                   in_use;
} prime_fd_table[PRIME_FD_MAX];

static spinlock_t prime_fd_lock = { .lock = 0, .rflags = 0 };

static int prime_fd_alloc(struct drm_gem_object *obj, int *fd_out)
{
    int i;

    spin_lock(&prime_fd_lock);

    for (i = 0; i < PRIME_FD_MAX; i++) {
        if (!prime_fd_table[i].in_use) {
            prime_fd_table[i].obj = obj;
            prime_fd_table[i].in_use = 1;
            *fd_out = i + 1; /* fd numbers start at 1 */
            obj->prime_fd = *fd_out;
            drm_gem_object_get(obj);
            spin_unlock(&prime_fd_lock);
            return 0;
        }
    }

    spin_unlock(&prime_fd_lock);
    return -ENOMEM;
}

static struct drm_gem_object *prime_fd_lookup(int fd)
{
    struct drm_gem_object *obj = NULL;
    int idx = fd - 1;

    if (idx < 0 || idx >= PRIME_FD_MAX) {
        return NULL;
    }

    spin_lock(&prime_fd_lock);
    if (prime_fd_table[idx].in_use) {
        obj = prime_fd_table[idx].obj;
        if (obj) {
            drm_gem_object_get(obj);
        }
    }
    spin_unlock(&prime_fd_lock);

    return obj;
}

void drm_gem_prime_fd_free(int fd)
{
    int idx = fd - 1;

    if (idx < 0 || idx >= PRIME_FD_MAX) {
        return;
    }

    spin_lock(&prime_fd_lock);
    if (prime_fd_table[idx].in_use) {
        prime_fd_table[idx].obj = NULL;
        prime_fd_table[idx].in_use = 0;
    }
    spin_unlock(&prime_fd_lock);
}

/* ------------------------------------------------------------------ */
/* drm_gem_prime_handle_to_fd: handle DRM_IOCTL_PRIME_HANDLE_TO_FD      */
/* ------------------------------------------------------------------ */

int drm_gem_prime_handle_to_fd(struct drm_device *dev,
                                struct drm_file *file_priv,
                                uint32_t handle, uint32_t flags,
                                int *prime_fd)
{
    struct drm_gem_object *obj;
    int fd;
    int ret;

    (void)dev;
    (void)flags;

    obj = drm_gem_object_lookup(file_priv, handle);
    if (!obj) {
        return -ENOENT;
    }

    /* If already exported, return existing fd */
    if (obj->prime_fd > 0) {
        *prime_fd = obj->prime_fd;
        drm_gem_object_put(obj);
        return 0;
    }

    ret = prime_fd_alloc(obj, &fd);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    *prime_fd = fd;
    drm_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_gem_prime_fd_to_handle: handle DRM_IOCTL_PRIME_FD_TO_HANDLE      */
/* ------------------------------------------------------------------ */

int drm_gem_prime_fd_to_handle(struct drm_device *dev,
                                struct drm_file *file_priv,
                                int prime_fd, uint32_t *handle)
{
    struct drm_gem_object *obj;
    uint32_t new_handle;
    int ret;

    (void)dev;

    obj = prime_fd_lookup(prime_fd);
    if (!obj) {
        return -ENOENT;
    }

    ret = drm_gem_handle_create(file_priv, obj, &new_handle);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    *handle = new_handle;
    drm_gem_object_put(obj);
    return 0;
}