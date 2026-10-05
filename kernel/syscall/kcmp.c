/*
 *
 *      kcmp.c
 *      Compare the kernel resources two processes share
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <process/process.h>
#include <process/sched.h>
#include <security/capability.h>
#include <syscall/syscall.h>

/* Linux x86-64 kcmp resource selectors. */
#define KCMP_FILE      0
#define KCMP_VM        1
#define KCMP_FILES     2
#define KCMP_FS        3
#define KCMP_SIGHAND   4
#define KCMP_IO        5
#define KCMP_SYSVSEM   6
#define KCMP_EPOLL_TFD 7

/*
 * kcmp(2) reports the ordering of the two compared kernel objects, which is
 * what makes it usable as a sort key.  Each selector resolves to the address of
 * the object that models the resource in this kernel.
 */
static int kcmp_order(uintptr_t first, uintptr_t second)
{
    if (first == second) return 0;
    return first < second ? 1 : 2;
}

int64_t sys_kcmp(uint64_t pid1, uint64_t pid2, uint64_t type, uint64_t idx1, uint64_t idx2, uint64_t arg5)
{
    (void)arg5;
    process_t *caller = process_current();
    if (!caller) return -ESRCH;

    process_t *first = process_find_get((pid_t)pid1);
    if (!first) return -ESRCH;
    process_t *second = pid1 == pid2 ? first : process_find_get((pid_t)pid2);
    if (!second) {
        process_put(first);
        return -ESRCH;
    }

    /*
     * Only a tracer or a same-UID caller may compare another process's
     * resources; the check mirrors the one guarding get_robust_list(2).
     */
    bool same_uid = first->uid == caller->uid && second->uid == caller->uid;
    if (!same_uid && !capability_has(current_task(), CAP_SYS_PTRACE)) {
        if (second != first) process_put(second);
        process_put(first);
        return -EPERM;
    }

    int64_t result;
    switch ((int)type) {
        case KCMP_FILE : {
            process_file_t *file1 = process_fd_get(first, (int)idx1);
            process_file_t *file2 = process_fd_get(second, (int)idx2);
            result                = (file1 && file2) ? kcmp_order((uintptr_t)file1, (uintptr_t)file2) : -EBADF;
            if (file1) process_file_put(file1);
            if (file2) process_file_put(file2);
            break;
        }
        case KCMP_VM :
            result = kcmp_order((uintptr_t)first->user_page_dir, (uintptr_t)second->user_page_dir);
            break;
        case KCMP_FILES :
            result = kcmp_order((uintptr_t)first->fds, (uintptr_t)second->fds);
            break;
        case KCMP_FS :
            result = kcmp_order((uintptr_t)first->root, (uintptr_t)second->root);
            break;
        case KCMP_SIGHAND :
            result = kcmp_order((uintptr_t)&first->signal, (uintptr_t)&second->signal);
            break;
        case KCMP_SYSVSEM :
            result = kcmp_order((uintptr_t)first, (uintptr_t)second);
            break;
        default :
            /* KCMP_IO, KCMP_EPOLL_TFD and unknown selectors are not supported. */
            result = -EOPNOTSUPP;
            break;
    }

    if (second != first) process_put(second);
    process_put(first);
    return result;
}
