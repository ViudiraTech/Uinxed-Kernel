/*
 *
 *      fcntl.c
 *      fcntl syscall implementation
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/filelock.h>
#include <fs/core/vfs.h>
#include <libs/std/errno.h>
#include <process/file_status.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <syscall/fcntl.h>
#include <syscall/memfd.h>
#include <syscall/syscall.h>

/* Read the access mode of an open file */
static uint64_t fcntl_access_mode(process_file_t *file)
{
    spin_lock(&file->lock);
    uint64_t mode = file->flags & O_ACCMODE;
    spin_unlock(&file->lock);
    return mode;
}

/* Translate a struct flock into an inclusive byte range, with the same checks as Linux. */
static int fcntl_flock_range(process_file_t *file, const linux_flock_t *fl, int64_t *start, int64_t *end)
{
    int64_t base;

    spin_lock(&file->lock);
    switch (fl->l_whence) {
        case SEEK_SET :
            base = 0;
            break;
        case SEEK_CUR :
            base = file->offset > (size_t)FILELOCK_OFFSET_MAX ? -1 : (int64_t)file->offset;
            break;
        case SEEK_END :
            base = file->node->size > (uint64_t)FILELOCK_OFFSET_MAX ? -1 : (int64_t)file->node->size;
            break;
        default :
            spin_unlock(&file->lock);
            return -EINVAL;
    }
    spin_unlock(&file->lock);

    if (base < 0 || fl->l_start > FILELOCK_OFFSET_MAX - base) return -EOVERFLOW;
    int64_t first = base + fl->l_start;
    if (first < 0) return -EINVAL;

    if (fl->l_len > 0) {
        if (fl->l_len - 1 > FILELOCK_OFFSET_MAX - first) return -EOVERFLOW;
        *start = first;
        *end   = first + fl->l_len - 1;
    } else if (fl->l_len < 0) {
        if (first + fl->l_len < 0) return -EINVAL;
        *start = first + fl->l_len;
        *end   = first - 1;
    } else {
        *start = first;
        *end   = FILELOCK_OFFSET_MAX;
    }
    return EOK;
}

/* Build the lock request described by a struct flock; POSIX locks belong to the process, OFD locks to the open file description. */
static int fcntl_lock_request(process_t *proc, process_file_t *file, const linux_flock_t *fl, bool ofd, filelock_request_t *request)
{
    int64_t start;
    int64_t end;
    int     ret = fcntl_flock_range(file, fl, &start, &end);

    if (ret != EOK) return ret;
    request->owner = ofd ? (const void *)file : (const void *)proc;
    request->start = start;
    request->end   = end;
    request->pid   = proc->task ? (int32_t)proc->task->tgid : 0;
    request->type  = (uint8_t)fl->l_type;
    request->ofd   = ofd;
    return EOK;
}

/* F_GETLK and F_OFD_GETLK: report the first lock that would block the described lock. */
static int64_t fcntl_getlk(process_t *proc, process_file_t *file, int cmd, uint64_t arg)
{
    linux_flock_t fl;
    bool          ofd = cmd == F_OFD_GETLK;

    if (!arg || copy_from_user(&fl, (const void *)arg, sizeof(fl))) return -EFAULT;
    if (ofd && fl.l_pid != 0) return -EINVAL;
    if (fl.l_type != F_RDLCK && fl.l_type != F_WRLCK) return -EINVAL;
    if (file->flags & O_PATH) return -EBADF;

    filelock_request_t request;
    int                ret = fcntl_lock_request(proc, file, &fl, ofd, &request);
    if (ret != EOK) return ret;

    filelock_key_t      key;
    filelock_conflict_t conflict;
    filelock_key_of(&key, file->node);
    if (filelock_test(&key, &request, &conflict)) {
        fl.l_type   = (int16_t)conflict.type;
        fl.l_whence = SEEK_SET;
        fl.l_start  = conflict.start;
        fl.l_len    = conflict.end == FILELOCK_OFFSET_MAX ? 0 : conflict.end - conflict.start + 1;
        fl.l_pid    = conflict.pid;
    } else {
        fl.l_type = F_UNLCK;
    }
    return copy_to_user((void *)arg, &fl, sizeof(fl)) ? -EFAULT : 0;
}

/* F_SETLK, F_SETLKW, F_OFD_SETLK and F_OFD_SETLKW: take, change or release a byte-range lock. */
static int64_t fcntl_setlk(process_t *proc, process_file_t *file, int fd, int cmd, uint64_t arg)
{
    linux_flock_t fl;
    bool          ofd  = cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW;
    bool          wait = cmd == F_SETLKW || cmd == F_OFD_SETLKW;

    if (!arg || copy_from_user(&fl, (const void *)arg, sizeof(fl))) return -EFAULT;
    if (ofd && fl.l_pid != 0) return -EINVAL;
    if (fl.l_type != F_RDLCK && fl.l_type != F_WRLCK && fl.l_type != F_UNLCK) return -EINVAL;
    if (file->flags & O_PATH) return -EBADF;

    /* A read lock needs a descriptor open for reading and a write lock one open for writing. */
    uint64_t access = fcntl_access_mode(file);
    if (fl.l_type == F_RDLCK && access == O_WRONLY) return -EBADF;
    if (fl.l_type == F_WRLCK && access == O_RDONLY) return -EBADF;

    filelock_request_t request;
    int                ret = fcntl_lock_request(proc, file, &fl, ofd, &request);
    if (ret != EOK) return ret;

    filelock_key_t key;
    filelock_key_of(&key, file->node);
    ret = filelock_set(&key, &request, wait);

    /*
     * A POSIX lock dies with any close of the file by this process.  If another
     * thread closed the descriptor while the request slept, the close missed the
     * lock that was only just granted, so take it back.
     */
    if (ret == EOK && !ofd && fl.l_type != F_UNLCK) {
        spin_lock(&proc->fd_lock);
        bool still_open = fd >= 0 && fd < CONFIG_PROCESS_MAX_FD && proc->fds[fd] == file;
        spin_unlock(&proc->fd_lock);
        if (!still_open) {
            filelock_release(&key, proc);
            ret = -EBADF;
        }
    }
    return ret;
}

/* fcntl syscall: manipulate file descriptor flags, locks, and seals */
int64_t sys_fcntl(int fd, int cmd, uint64_t arg)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    int64_t result = -EINVAL;

    switch (cmd) {
        case F_DUPFD : {
            int      start = (int)arg;
            uint32_t limit = process_fd_limit(proc);
            if (start < 0) {
                result = -EINVAL;
                break;
            }
            if ((uint32_t)start >= limit) {
                result = -EINVAL;
                break;
            }
            spin_lock(&proc->fd_lock);
            int newfd = -1;
            for (uint32_t i = (uint32_t)start; i < limit; i++) {
                if (!proc->fds[i]) {
                    newfd = (int)i;
                    break;
                }
            }
            if (newfd < 0) {
                spin_unlock(&proc->fd_lock);
                result = -EMFILE;
                break;
            }

            /* Reference the file for the new slot */
            spin_lock(&file->lock);
            __atomic_fetch_add(&file->refcount, 1, __ATOMIC_RELAXED);
            file->fd_refcount++;
            spin_unlock(&file->lock);

            proc->fds[newfd]      = file;
            proc->fd_flags[newfd] = 0;
            spin_unlock(&proc->fd_lock);
            result = newfd;
            break;
        }
        case F_DUPFD_CLOEXEC : {
            int      start = (int)arg;
            uint32_t limit = process_fd_limit(proc);
            if (start < 0) {
                result = -EINVAL;
                break;
            }
            if ((uint32_t)start >= limit) {
                result = -EINVAL;
                break;
            }
            spin_lock(&proc->fd_lock);
            int newfd = -1;
            for (uint32_t i = (uint32_t)start; i < limit; i++) {
                if (!proc->fds[i]) {
                    newfd = (int)i;
                    break;
                }
            }
            if (newfd < 0) {
                spin_unlock(&proc->fd_lock);
                result = -EMFILE;
                break;
            }

            spin_lock(&file->lock);
            __atomic_fetch_add(&file->refcount, 1, __ATOMIC_RELAXED);
            file->fd_refcount++;
            spin_unlock(&file->lock);

            proc->fds[newfd]      = file;
            proc->fd_flags[newfd] = FD_CLOEXEC;
            spin_unlock(&proc->fd_lock);
            result = newfd;
            break;
        }
        case F_GETFD :
            spin_lock(&proc->fd_lock);
            result = proc->fds[fd] ? proc->fd_flags[fd] : -EBADF;
            spin_unlock(&proc->fd_lock);
            break;
        case F_SETFD :
            if (arg & ~(uint64_t)FD_CLOEXEC) {
                result = -EINVAL;
                break;
            }
            spin_lock(&proc->fd_lock);
            if (proc->fds[fd]) {
                proc->fd_flags[fd] = (uint8_t)(arg & FD_CLOEXEC);
                result             = 0;
            } else {
                result = -EBADF;
            }
            spin_unlock(&proc->fd_lock);
            break;
        case F_GETFL :
            /* Return file access mode and status flags */
            spin_lock(&file->lock);
            result = (int64_t)(file->flags & (O_ACCMODE | O_NONBLOCK | O_APPEND | O_PATH));
            spin_unlock(&file->lock);
            break;
        case F_SETFL : {
            /* Only O_NONBLOCK and O_APPEND can be changed */
            uint64_t settable = O_NONBLOCK | O_APPEND;
            spin_lock(&file->lock);
            file->flags = process_file_status_flags_merge(file->flags, settable, arg);
            spin_unlock(&file->lock);
            result = 0;
            break;
        }
        case F_ADD_SEALS :
            result = memfd_add_seals(file->node, (uint32_t)arg);
            break;

        case F_GET_SEALS : {
            uint32_t seals;
            result = memfd_get_seals(file->node, &seals);
            if (result == EOK) result = seals;
            break;
        }
        case F_GETLK :
        case F_OFD_GETLK :
            result = fcntl_getlk(proc, file, cmd, arg);
            break;
        case F_SETLK :
        case F_SETLKW :
        case F_OFD_SETLK :
        case F_OFD_SETLKW :
            result = fcntl_setlk(proc, file, fd, cmd, arg);
            break;
        case F_SETOWN :
        case F_GETOWN :
        case F_SETOWN_EX :
            result = 0;
            break;
        case F_GETOWN_EX : {
            /* Get owner (struct f_owner_ex) - return empty */
            if (arg) {
                struct {
                        int32_t type;
                        int32_t pid;
                } owner = {F_OWNER_PID, 0};

                if (copy_to_user((void *)arg, &owner, sizeof(owner))) {
                    result = -EFAULT;
                    break;
                }
            }
            result = 0;
            break;
        }
        case F_SETSIG :
        case F_GETSIG :
            result = 0;
            break;
        default :
            result = -EINVAL;
            break;
    }
    process_file_put(file);
    return result;
}
