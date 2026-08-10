/*
 *
 *      memfd.c
 *      Anonymous page-backed memory files
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <process/uaccess.h>
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

/*
 * Overview
 * memfd implements anonymous memory-backed files (memfd_create).
 * A memfd_file_t holds a growable page array; reads/writes/truncate
 * touch it through the VFS, and mmap attaches physical pages to the
 * caller's page tables. Seals restrict later mutations.
 */

static int memfd_fsid;

static int memfd_expand_page_array(memfd_file_t *file, size_t count)
{
    if (count <= file->page_capacity) return EOK;
    if (count > SIZE_MAX / sizeof(*file->pages)) {
        plogk("memfd: Page array count overflow (%lu)\n", (unsigned long)count);
        return -EFBIG;
    }

    size_t capacity = file->page_capacity ? file->page_capacity : 16;
    while (capacity < count) {
        if (capacity > SIZE_MAX / 2) {
            capacity = count;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*file->pages)) {
        plogk("memfd: Page array capacity overflow (%lu)\n", (unsigned long)capacity);
        return -EFBIG;
    }

    uint64_t *pages = malloc(capacity * sizeof(*pages));
    if (!pages) {
        plogk("memfd: Page array alloc failed (capacity=%lu, file=%p)\n", (unsigned long)capacity, (void *)file);
        return -ENOMEM;
    }
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
        if (!file->pages[index]) {
            plogk("memfd: Frame alloc failed (index=%lu, file=%p)\n", (unsigned long)index, (void *)file);
            return -ENOMEM;
        }
        memset(phys_to_virt(file->pages[index]), 0, PAGE_4K_SIZE);
    }
    if (index >= file->page_count) file->page_count = index + 1;
    return EOK;
}

static int memfd_resize_locked(memfd_file_t *file, uint64_t size)
{
    if (file->seals & F_SEAL_WRITE) {
        plogk("memfd: Resize denied (file=%p, size=%lu, seal=write)\n", (void *)file, (unsigned long)size);
        return -EPERM;
    }
    if (size > file->size && (file->seals & F_SEAL_GROW)) {
        plogk("memfd: Grow denied (file=%p, size=%lu, seal=grow)\n", (void *)file, (unsigned long)size);
        return -EPERM;
    }
    if (size < file->size && (file->seals & F_SEAL_SHRINK)) {
        plogk("memfd: Shrink denied (file=%p, size=%lu, seal=shrink)\n", (void *)file, (unsigned long)size);
        return -EPERM;
    }
    if (size < file->size && file->mappings) {
        plogk("memfd: Shrink denied (file=%p, size=%lu, mappings=%u)\n", (void *)file, (unsigned long)size, file->mappings);
        return -EBUSY;
    }

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
    if (!file) {
        plogk("memfd: Read on node without handle (node=%p)\n", (void *)node);
        return -EINVAL;
    }

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
    if (!file) {
        plogk("memfd: Write on node without handle (node=%p)\n", (void *)node);
        return -EINVAL;
    }
    if (size > SIZE_MAX - offset) {
        plogk("memfd: Write length overflow (offset=%lu, size=%lu)\n", (unsigned long)offset, (unsigned long)size);
        return -EFBIG;
    }

    uint64_t end = offset + size;
    spin_lock(&file->lock);
    if (file->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) {
        spin_unlock(&file->lock);
        plogk("memfd: Write denied (offset=%lu, size=%lu, seal=write)\n", (unsigned long)offset, (unsigned long)size);
        return -EPERM;
    }
    if (end > file->size && (file->seals & F_SEAL_GROW)) {
        spin_unlock(&file->lock);
        plogk("memfd: Write grow denied (offset=%lu, size=%lu, seal=grow)\n", (unsigned long)offset, (unsigned long)size);
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
            if (!done) plogk("memfd: Write page alloc failed (offset=%lu, size=%lu, ret=%d)\n", (unsigned long)offset, (unsigned long)size, ret);
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
    if (!file) {
        plogk("memfd: Stat on node without handle (node=%p)\n", (void *)node);
        return -EINVAL;
    }
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
    for (size_t i = 0; i < file->page_count; i++)
        if (file->pages[i]) free_frames(file->pages[i], 1);
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
    if ((flags & ~(uint64_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING)) || !name) {
        plogk("memfd: Create invalid flags (%lx)\n", (unsigned long)flags);
        return -EINVAL;
    }
    if (memfd_fsid <= 0) {
        plogk("memfd: Create before memfd filesystem registered.\n");
        return -ENOSYS;
    }

    char memfd_name[MFD_NAME_MAX + 1];
    int  name_len = strncpy_from_user(memfd_name, (const char *)name, sizeof(memfd_name));
    if (name_len == -ENAMETOOLONG) {
        plogk("memfd: Create name too long (name=%p)\n", (void *)name);
        return -EINVAL;
    }
    if (name_len < 0) {
        plogk("memfd: Create name copy from user failed (name=%p)\n", (void *)name);
        return -EFAULT;
    }

    process_t *proc = process_current();
    if (!proc) {
        plogk("memfd: Create with no current process.\n");
        return -ESRCH;
    }
    memfd_file_t *file = malloc(sizeof(*file));
    if (!file) {
        plogk("memfd: Create file alloc failed (name_len=%d)\n", name_len);
        return -ENOMEM;
    }
    memset(file, 0, sizeof(*file));
    file->seals = (flags & MFD_ALLOW_SEALING) ? 0 : F_SEAL_SEAL;

    char node_name[MFD_NAME_MAX + 8];
    strcpy(node_name, "memfd:");
    memcpy(node_name + 6, memfd_name, (size_t)name_len + 1);
    vfs_node_t node = vfs_node_alloc(NULL, node_name);
    if (!node) {
        free(file);
        plogk("memfd: Create node alloc failed (name_len=%d)\n", name_len);
        return -ENOMEM;
    }
    node->fsid     = memfd_fsid;
    node->handle   = file;
    node->type     = file_none;
    node->mode     = 0600;
    node->owner    = proc->fsuid;
    node->group    = proc->fsgid;
    node->refcount = 1; // The initial open file description owns this reference.

    int fd = process_fd_install(proc, node, O_RDWR | ((flags & MFD_CLOEXEC) ? O_CLOEXEC : 0));
    if (fd < 0) {
        memfd_free(file);
        vfs_free(node);
        plogk("memfd: Create fd install failed (name_len=%d, ret=%d)\n", name_len, fd);
    }
    return fd;
}

int memfd_get_seals(vfs_node_t node, uint32_t *seals)
{
    if (!memfd_is_node(node) || !seals) {
        plogk("memfd: get_seals on non-memfd node (%p)\n", (void *)node);
        return -EINVAL;
    }
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    *seals = file->seals;
    spin_unlock(&file->lock);
    return EOK;
}

int memfd_add_seals(vfs_node_t node, uint32_t seals)
{
    if (!memfd_is_node(node)) {
        plogk("memfd: add_seals on non-memfd node (%p)\n", (void *)node);
        return -EINVAL;
    }
    if (!seals || (seals & ~(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))) {
        plogk("memfd: add_seals invalid flags (%x)\n", (unsigned)seals);
        return -EINVAL;
    }
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    if (file->seals & F_SEAL_SEAL) {
        spin_unlock(&file->lock);
        plogk("memfd: add_seals denied (file=%p, seal=sealed)\n", (void *)file);
        return -EPERM;
    }
    if ((seals & F_SEAL_WRITE) && file->writable_mappings) {
        spin_unlock(&file->lock);
        plogk("memfd: add_seals F_SEAL_WRITE denied (file=%p, writable_mappings=%u)\n", (void *)file, file->writable_mappings);
        return -EBUSY;
    }
    file->seals |= seals;
    spin_unlock(&file->lock);
    return EOK;
}

int memfd_resize(vfs_node_t node, uint64_t size)
{
    if (!memfd_is_node(node)) {
        plogk("memfd: Resize of non-memfd node (%p)\n", (void *)node);
        return -EINVAL;
    }
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    int ret    = memfd_resize_locked(file, size);
    node->size = file->size;
    spin_unlock(&file->lock);
    return ret;
}

int memfd_fallocate(vfs_node_t node, uint32_t mode, uint64_t offset, uint64_t length)
{
    if (!memfd_is_node(node)) {
        plogk("memfd: Fallocate on non-memfd node (%p)\n", (void *)node);
        return -EOPNOTSUPP;
    }
    if (!length) {
        plogk("memfd: Fallocate with zero length.\n");
        return -EINVAL;
    }
    if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE)) {
        plogk("memfd: Fallocate invalid mode (%x)\n", (unsigned)mode);
        return -EOPNOTSUPP;
    }
    if ((mode & FALLOC_FL_PUNCH_HOLE) && (!(mode & FALLOC_FL_KEEP_SIZE) || (mode & FALLOC_FL_ZERO_RANGE))) {
        plogk("memfd: Fallocate PUNCH_HOLE requires KEEP_SIZE and no ZERO_RANGE (mode=%x)\n", (unsigned)mode);
        return -EINVAL;
    }
    if ((mode & FALLOC_FL_ZERO_RANGE) && (mode & FALLOC_FL_PUNCH_HOLE)) {
        plogk("memfd: Fallocate ZERO_RANGE conflicts with PUNCH_HOLE (mode=%x)\n", (unsigned)mode);
        return -EINVAL;
    }
    if (offset > UINT64_MAX - length) {
        plogk("memfd: Fallocate offset+length overflow (offset=%lu, length=%lu)\n", (unsigned long)offset, (unsigned long)length);
        return -EFBIG;
    }

    memfd_file_t *file = node->handle;
    uint64_t      end  = offset + length;
    spin_lock(&file->lock);
    if (file->seals & F_SEAL_WRITE) {
        spin_unlock(&file->lock);
        plogk("memfd: Fallocate denied (offset=%lu, length=%lu, seal=write)\n", (unsigned long)offset, (unsigned long)length);
        return -EPERM;
    }
    if (!(mode & FALLOC_FL_KEEP_SIZE) && end > file->size && (file->seals & F_SEAL_GROW)) {
        spin_unlock(&file->lock);
        plogk("memfd: Fallocate grow denied (offset=%lu, length=%lu, seal=grow)\n", (unsigned long)offset, (unsigned long)length);
        return -EPERM;
    }
    for (size_t page = offset / PAGE_4K_SIZE; page <= (end - 1) / PAGE_4K_SIZE; page++) {
        if (mode & FALLOC_FL_PUNCH_HOLE) {
            if (page < file->page_count && file->pages[page]) memset(phys_to_virt(file->pages[page]), 0, PAGE_4K_SIZE);
        } else {
            int ret = memfd_allocate_page(file, page);
            if (ret) {
                spin_unlock(&file->lock);
                plogk("memfd: Fallocate page alloc failed (offset=%lu, length=%lu, ret=%d)\n", (unsigned long)offset, (unsigned long)length,
                      ret);
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
    if (!memfd_is_node(node) || !proc || offset > UINT64_MAX - length) {
        plogk("memfd: Map invalid args (node=%p, proc=%p, offset=%lu, length=%lu)\n", (void *)node, (void *)proc, (unsigned long)offset,
              (unsigned long)length);
        return -EINVAL;
    }
    memfd_file_t *file = node->handle;
    spin_lock(&file->lock);
    bool shared_writable = (flags & (VM_SHARED | VM_WRITE)) == (VM_SHARED | VM_WRITE);
    if (shared_writable && (file->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))) {
        spin_unlock(&file->lock);
        plogk("memfd: Map denied (proc=%p, seal=write)\n", (void *)proc);
        return -EPERM;
    }

    /*
     * mmap() rounds its length up to a page.  A mapping may therefore cover
     * the final partial page of a memfd even when the byte length is not
     * page-aligned; Linux exposes the bytes past EOF in that page as zeroes
     * (and callers such as Weston's keymap builder rely on this).  Do not
     * permit mapping a whole page beyond the rounded EOF.
     */
    uint64_t map_limit = ALIGN_UP(file->size, PAGE_4K_SIZE);
    if (offset > map_limit || length > map_limit - offset) {
        spin_unlock(&file->lock);
        plogk("memfd: Map beyond EOF denied (offset=%lu, length=%lu, size=%lu)\n", (unsigned long)offset, (unsigned long)length,
              (unsigned long)file->size);
        return -EINVAL;
    }
    for (size_t done = 0; done < length; done += PAGE_4K_SIZE) {
        int ret = memfd_allocate_page(file, (offset + done) / PAGE_4K_SIZE);
        if (ret) {
            spin_unlock(&file->lock);
            plogk("memfd: Map page alloc failed (offset=%lu, length=%lu, ret=%d)\n", (unsigned long)offset, (unsigned long)length, ret);
            return ret;
        }
    }

    uint64_t pte_flags = PTE_USER | PTE_PRESENT;
    if (flags & VM_WRITE) {
        if (flags & VM_SHARED)
            pte_flags |= PTE_WRITEABLE;
        else
            pte_flags |= PTE_COW;
    }
    if (flags & VM_SHARED) pte_flags |= PTE_SHARED;
    if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;
    size_t mapped = 0;
    for (; mapped < length; mapped += PAGE_4K_SIZE) {
        size_t page = (offset + mapped) / PAGE_4K_SIZE;
        if (frame_retain_range(file->pages[page], 1)) goto rollback;
        if (page_map_new_to(proc->user_page_dir, addr + mapped, file->pages[page], pte_flags)) {
            (void)frame_release_range(file->pages[page], 1);
            goto rollback;
        }
    }
    file->mappings++;
    if (shared_writable) file->writable_mappings++;
    spin_unlock(&file->lock);
    return EOK;

rollback:
    while (mapped) {
        mapped -= PAGE_4K_SIZE;
        (void)page_unmap_release(proc->user_page_dir, addr + mapped);
    }
    spin_unlock(&file->lock);
    plogk("memfd: Map PTE setup failed, rolled back (offset=%lu, length=%lu)\n", (unsigned long)offset, (unsigned long)length);
    return -ENOMEM;
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
        plogk("memfd: vma protect denied (node=%p, seal=write)\n", (void *)node);
        return -EPERM;
    }
    if (new_writable)
        file->writable_mappings++;
    else if (file->writable_mappings)
        file->writable_mappings--;
    spin_unlock(&file->lock);
    return EOK;
}
