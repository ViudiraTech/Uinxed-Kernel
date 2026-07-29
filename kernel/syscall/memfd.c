/*
 *
 *      memfd.c
 *      Anonymous page-backed memory files
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>
#include <syscall/memfd.h>

#define FALLOC_FL_KEEP_SIZE  0x01U
#define FALLOC_FL_PUNCH_HOLE 0x02U
#define FALLOC_FL_ZERO_RANGE 0x10U

typedef struct {
        spinlock_t lock;
        uint64_t  *pages;
        size_t     page_count;
        size_t     page_capacity;
        uint64_t   size;
        uint32_t   seals;
        uint32_t   mappings;
        uint32_t   writable_mappings;
} memfd_file_t;

static int memfd_fsid;

static int memfd_expand_page_array(memfd_file_t *file, size_t count)
{
    if (count <= file->page_capacity) return EOK;
    if (count > SIZE_MAX / sizeof(*file->pages)) return -EFBIG;

    size_t capacity = file->page_capacity ? file->page_capacity : 16;
    while (capacity < count) {
        if (capacity > SIZE_MAX / 2) {
            capacity = count;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*file->pages)) return -EFBIG;

    uint64_t *pages = malloc(capacity * sizeof(*pages));
    if (!pages) return -ENOMEM;
    if (file->pages) {
        memcpy(pages, file->pages, file->page_capacity * sizeof(*pages));
        free(file->pages);
    }
    memset(pages + file->page_capacity, 0, (capacity - file->page_capacity) * sizeof(*pages));
    file->pages         = pages;
    file->page_capacity = capacity;
    return EOK;
}

static int memfd_allocate_page(memfd_file_t *file, size_t index)
{
    int ret = memfd_expand_page_array(file, index + 1);
    if (ret) return ret;
    if (!file->pages[index]) {
        file->pages[index] = alloc_frames(1);
        if (!file->pages[index]) return -ENOMEM;
        memset(phys_to_virt(file->pages[index]), 0, PAGE_4K_SIZE);
    }
    if (index >= file->page_count) file->page_count = index + 1;
    return EOK;
}

static int memfd_resize_locked(memfd_file_t *file, uint64_t size)
{
    if (file->seals & F_SEAL_WRITE) return -EPERM;
    if (size > file->size && (file->seals & F_SEAL_GROW)) return -EPERM;
    if (size < file->size && (file->seals & F_SEAL_SHRINK)) return -EPERM;
    if (size < file->size && file->mappings) return -EBUSY;

    if (size < file->size) {
        size_t first_unused = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
        for (size_t i = first_unused; i < file->page_count; i++) {
            if (file->pages[i]) {
                free_frames(file->pages[i], 1);
                file->pages[i] = 0;
            }
        }
        file->page_count = first_unused;
        if (size && (size & (PAGE_4K_SIZE - 1)) && first_unused) {
            uint64_t frame = file->pages[first_unused - 1];
            if (frame) memset((uint8_t *)phys_to_virt(frame) + (size & (PAGE_4K_SIZE - 1)), 0, PAGE_4K_SIZE - (size & (PAGE_4K_SIZE - 1)));
        }
    }
    file->size = size;
    return EOK;
}

static int64_t memfd_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)flags;
    memfd_file_t *file = node->handle;
    if (!file) return -EINVAL;

    spin_lock(&file->lock);
    if (offset >= file->size) {
        spin_unlock(&file->lock);
        return 0;
    }
    if (size > file->size - offset) size = file->size - offset;
    for (size_t done = 0; done < size;) {
        size_t page    = (offset + done) / PAGE_4K_SIZE;
        size_t in_page = (offset + done) & (PAGE_4K_SIZE - 1);
        size_t chunk   = PAGE_4K_SIZE - in_page;
        if (chunk > size - done) chunk = size - done;
        if (page < file->page_count && file->pages[page])
            memcpy((uint8_t *)addr + done, (uint8_t *)phys_to_virt(file->pages[page]) + in_page, chunk);
        else
            memset((uint8_t *)addr + done, 0, chunk);
        done += chunk;
    }
    spin_unlock(&file->lock);
    return (int64_t)size;
}

static int64_t memfd_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)private_data;
    (void)flags;
    memfd_file_t *file = node->handle;
    if (!file) return -EINVAL;
    if (size > SIZE_MAX - offset) return -EFBIG;

    uint64_t end = offset + size;
    spin_lock(&file->lock);
    if (file->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) {
        spin_unlock(&file->lock);
        return -EPERM;
    }
    if (end > file->size && (file->seals & F_SEAL_GROW)) {
        spin_unlock(&file->lock);
        return -EPERM;
    }
    for (size_t done = 0; done < size;) {
        size_t page    = (offset + done) / PAGE_4K_SIZE;
        size_t in_page = (offset + done) & (PAGE_4K_SIZE - 1);
        size_t chunk   = PAGE_4K_SIZE - in_page;
        if (chunk > size - done) chunk = size - done;
        int ret = memfd_allocate_page(file, page);
        if (ret) {
            spin_unlock(&file->lock);
            return done ? (int64_t)done : ret;
        }
        memcpy((uint8_t *)phys_to_virt(file->pages[page]) + in_page, (const uint8_t *)addr + done, chunk);
        done += chunk;
    }
    if (end > file->size) file->size = end;
    node->size = file->size;
    spin_unlock(&file->lock);
    return (int64_t)size;
}

static int memfd_stat(void *handle, vfs_node_t node)
{
    memfd_file_t *file = handle;
    if (!file) return -EINVAL;
    spin_lock(&file->lock);
    node->type = file_none;
    node->size = file->size;
    spin_unlock(&file->lock);
    return EOK;
}

static int memfd_free(void *handle)
{
    memfd_file_t *file = handle;
    if (!file) return EOK;
    for (size_t i = 0; i < file->page_count; i++) {
        if (file->pages[i]) free_frames(file->pages[i], 1);
    }
    free(file->pages);
    free(file);
    return EOK;
}

static struct vfs_callback memfd_callbacks = {
    .stat       = memfd_stat,
    .free       = memfd_free,
    .file_read  = memfd_file_read,
    .file_write = memfd_file_write,
};

void memfd_init(void)
{
    memfd_fsid = vfs_regist_fs("memfd", &memfd_callbacks);
}

int memfd_is_node(vfs_node_t node)
{
    return node && node->fsid == memfd_fsid;
}

int64_t sys_memfd_create(uint64_t name, uint64_t flags, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    if ((flags & ~(uint64_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING)) || !name) return -EINVAL;
    if (memfd_fsid <= 0) return -ENOSYS;

    char memfd_name[MFD_NAME_MAX + 1];
    int  name_len = strncpy_from_user(memfd_name, (const char *)name, sizeof(memfd_name));
    if (name_len == -ENAMETOOLONG) return -EINVAL;
    if (name_len < 0) return -EFAULT;

    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    memfd_file_t *file = malloc(sizeof(*file));
    if (!file) return -ENOMEM;
    memset(file, 0, sizeof(*file));
    file->seals = (flags & MFD_ALLOW_SEALING) ? 0 : F_SEAL_SEAL;

    char node_name[MFD_NAME_MAX + 8];
    strcpy(node_name, "memfd:");
    memcpy(node_name + 6, memfd_name, (size_t)name_len + 1);
    vfs_node_t node = vfs_node_alloc(NULL, node_name);
    if (!node) {
        free(file);
        return -ENOMEM;
    }
    node->fsid     = memfd_fsid;
    node->handle   = file;
    node->type     = file_none;
    node->mode     = 0600;
    node->owner    = proc->uid;
    node->group    = proc->gid;
    node->refcount = 1; /* The initial open file description owns this reference. */

    int fd = process_fd_install(proc, node, O_RDWR | ((flags & MFD_CLOEXEC) ? O_CLOEXEC : 0));
    if (fd < 0) {
        memfd_free(file);
        vfs_free(node);
    }
    return fd;
}

int memfd_get_seals(vfs_node_t node, uint32_t *seals)
{
    if (!memfd_is_node(node) || !seals) return -EINVAL;
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    *seals = file->seals;
    spin_unlock(&file->lock);
    return EOK;
}

int memfd_add_seals(vfs_node_t node, uint32_t seals)
{
    if (!memfd_is_node(node)) return -EINVAL;
    if (!seals || (seals & ~(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))) return -EINVAL;
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    if (file->seals & F_SEAL_SEAL) {
        spin_unlock(&file->lock);
        return -EPERM;
    }
    if ((seals & F_SEAL_WRITE) && file->writable_mappings) {
        spin_unlock(&file->lock);
        return -EBUSY;
    }
    file->seals |= seals;
    spin_unlock(&file->lock);
    return EOK;
}

int memfd_resize(vfs_node_t node, uint64_t size)
{
    if (!memfd_is_node(node)) return -EINVAL;
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    int ret    = memfd_resize_locked(file, size);
    node->size = file->size;
    spin_unlock(&file->lock);
    return ret;
}

int memfd_fallocate(vfs_node_t node, uint32_t mode, uint64_t offset, uint64_t length)
{
    if (!memfd_is_node(node)) return -EOPNOTSUPP;
    if (!length) return -EINVAL;
    if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE)) return -EOPNOTSUPP;
    if ((mode & FALLOC_FL_PUNCH_HOLE) && (!(mode & FALLOC_FL_KEEP_SIZE) || (mode & FALLOC_FL_ZERO_RANGE))) return -EINVAL;
    if ((mode & FALLOC_FL_ZERO_RANGE) && (mode & FALLOC_FL_PUNCH_HOLE)) return -EINVAL;
    if (offset > UINT64_MAX - length) return -EFBIG;

    memfd_file_t *file = node->handle;
    uint64_t      end  = offset + length;
    spin_lock(&file->lock);
    if (file->seals & F_SEAL_WRITE) {
        spin_unlock(&file->lock);
        return -EPERM;
    }
    if (!(mode & FALLOC_FL_KEEP_SIZE) && end > file->size && (file->seals & F_SEAL_GROW)) {
        spin_unlock(&file->lock);
        return -EPERM;
    }
    for (size_t page = offset / PAGE_4K_SIZE; page <= (end - 1) / PAGE_4K_SIZE; page++) {
        if (mode & FALLOC_FL_PUNCH_HOLE) {
            if (page < file->page_count && file->pages[page]) memset(phys_to_virt(file->pages[page]), 0, PAGE_4K_SIZE);
        } else {
            int ret = memfd_allocate_page(file, page);
            if (ret) {
                spin_unlock(&file->lock);
                return ret;
            }
            if (mode & FALLOC_FL_ZERO_RANGE) memset(phys_to_virt(file->pages[page]), 0, PAGE_4K_SIZE);
        }
    }
    if (!(mode & FALLOC_FL_KEEP_SIZE) && end > file->size) file->size = end;
    node->size = file->size;
    spin_unlock(&file->lock);
    return EOK;
}

int memfd_map(vfs_node_t node, process_t *proc, uintptr_t addr, size_t length, uint64_t offset, vm_flags_t flags)
{
    if (!memfd_is_node(node) || !proc || offset > UINT64_MAX - length) return -EINVAL;
    memfd_file_t *file = node->handle;
    if ((flags & VM_SHARED) && (flags & VM_WRITE)) {
        spin_lock(&file->lock);
        if (file->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) {
            spin_unlock(&file->lock);
            return -EPERM;
        }
        file->writable_mappings++;
        spin_unlock(&file->lock);
    }

    spin_lock(&file->lock);
    /* The VM has no file-page fault path to turn beyond-EOF access into SIGBUS. */
    if (offset + length > file->size) {
        spin_unlock(&file->lock);
        if ((flags & VM_SHARED) && (flags & VM_WRITE)) memfd_vma_release(node, flags);
        return -EINVAL;
    }
    for (size_t done = 0; done < length; done += PAGE_4K_SIZE) {
        int ret = memfd_allocate_page(file, (offset + done) / PAGE_4K_SIZE);
        if (ret) {
            spin_unlock(&file->lock);
            if ((flags & VM_SHARED) && (flags & VM_WRITE)) memfd_vma_release(node, flags);
            return ret;
        }
    }

    uint64_t pte_flags = PTE_USER | PTE_PRESENT;
    if (flags & VM_WRITE) pte_flags |= PTE_WRITEABLE;
    if (flags & VM_SHARED) pte_flags |= PTE_SHARED;
    if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;
    for (size_t done = 0; done < length; done += PAGE_4K_SIZE) {
        size_t page = (offset + done) / PAGE_4K_SIZE;
        frame_retain_range(file->pages[page], 1);
        page_map_to(proc->user_page_dir, addr + done, file->pages[page], pte_flags);
    }
    file->mappings++;
    spin_unlock(&file->lock);
    return EOK;
}

void memfd_vma_retain(vfs_node_t node, vm_flags_t flags)
{
    if (!memfd_is_node(node)) return;
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    file->mappings++;
    if ((flags & (VM_SHARED | VM_WRITE)) == (VM_SHARED | VM_WRITE)) file->writable_mappings++;
    spin_unlock(&file->lock);
}

void memfd_vma_release(vfs_node_t node, vm_flags_t flags)
{
    if (!memfd_is_node(node)) return;
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    if (file->mappings) file->mappings--;
    if ((flags & (VM_SHARED | VM_WRITE)) == (VM_SHARED | VM_WRITE) && file->writable_mappings) file->writable_mappings--;
    spin_unlock(&file->lock);
}

int memfd_vma_protect(vfs_node_t node, vm_flags_t old_flags, vm_flags_t new_flags)
{
    if (!memfd_is_node(node)) return EOK;

    bool old_writable = (old_flags & (VM_SHARED | VM_WRITE)) == (VM_SHARED | VM_WRITE);
    bool new_writable = (new_flags & (VM_SHARED | VM_WRITE)) == (VM_SHARED | VM_WRITE);
    if (old_writable == new_writable) return EOK;

    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    if (new_writable && (file->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))) {
        spin_unlock(&file->lock);
        return -EPERM;
    }
    if (new_writable)
        file->writable_mappings++;
    else if (file->writable_mappings)
        file->writable_mappings--;
    spin_unlock(&file->lock);
    return EOK;
}
