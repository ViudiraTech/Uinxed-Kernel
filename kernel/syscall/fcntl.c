/*
 *
 *      fcntl.c
 *      fcntl syscall implementation
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <process/process.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/fcntl.h>
#include <syscall/memfd.h>
#include <syscall/syscall.h>

int64_t sys_fcntl(int fd, int cmd, uint64_t arg)
{
    process_t *proc = process_current();
    if (!proc) return -ESRCH;

    process_file_t *file = process_fd_get(proc, fd);
    if (!file) return -EBADF;

    int64_t result = -EINVAL;

    /* Todo: Implement fcntl cmds full implementation */
    /* We use raw fd table access for operations that need it */
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

        case F_GETFD : {
            spin_lock(&proc->fd_lock);
            result = proc->fds[fd] ? proc->fd_flags[fd] : -EBADF;
            spin_unlock(&proc->fd_lock);
            break;
        }

        case F_SETFD : {
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
        }

        case F_GETFL : {
            /* Return file access mode and status flags */
            spin_lock(&file->lock);
            result = (int64_t)(file->flags & (O_ACCMODE | O_NONBLOCK | O_APPEND | O_PATH));
            spin_unlock(&file->lock);
            break;
        }

        case F_SETFL : {
            /* Only O_NONBLOCK and O_APPEND can be changed */
            uint64_t settable = O_NONBLOCK | O_APPEND;
            spin_lock(&file->lock);
            uint64_t current = __atomic_load_n(&file->flags, __ATOMIC_RELAXED);
            __atomic_store_n(&file->flags, (current & ~settable) | (arg & settable), __ATOMIC_RELAXED);
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

        case F_GETLK : {
            /* Advisory file locking - not supported, always return success */
            if (arg) {
                /* Write back an unlocked lock struct */
                struct {
                        int16_t l_type;
                        int16_t l_whence;
                        int64_t l_start;
                        int64_t l_len;
                        int32_t l_pid;
                } fl = {F_UNLCK, 0, 0, 0, 0};
                if (copy_to_user((void *)arg, &fl, sizeof(fl))) {
                    result = -EFAULT;
                    break;
                }
            }
            result = 0;
            break;
        }
        case F_SETLK :
        case F_SETLKW :
        case F_SETOWN :
        case F_GETOWN :
        case F_SETOWN_EX :
            /* Non-blocking / blocking lock - no-op (no mandatory locking) */
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
