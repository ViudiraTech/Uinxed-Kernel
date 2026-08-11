/*
 *
 *      ffsystem.c
 *      User provided OS dependent functions for FatFs.
 *
 *      2026/5/18 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/fatfs/ff.h>
#include <kernel/errno.h>
#include <process/sched.h>
#include <sync/rt_mutex.h>

#if FF_USE_LFN == 3
#    include <mem/alloc.h>

/* Allocate a memory block (returns NULL if not enough core) */
void *ff_memalloc(UINT msize)
{
    return malloc((size_t)msize);
}

/* Free a memory block */
void ff_memfree(void *mblock)
{
    free(mblock);
}

#endif // FF_USE_LFN

#if FF_FS_REENTRANT

/*
 * One real-time mutex per logical volume plus the system mutex (index
 * FF_VOLUMES). The kernel rt_mutex provides priority inheritance.
 */
static rt_mutex_t Mutex[FF_VOLUMES + 1];

/*
 * Create a mutex for the volume. This is called in f_mount. Returns 1 on
 * success, 0 on failure (f_mount fails with FR_INT_ERR).
 */
int ff_mutex_create(int vol)
{
    if (vol < 0 || vol > FF_VOLUMES) return 0;

    rt_mutex_init(&Mutex[vol], NULL);
    return 1;
}

/* Delete the mutex of the volume (created with ff_mutex_create) */
void ff_mutex_delete(int vol)
{
    if (vol < 0 || vol > FF_VOLUMES) return;
    /* The kernel rt_mutex needs no destruction; no waiter should remain here. */
}

/*
 * Lock the volume. This is called on enter file functions. The kernel
 * rt_mutex blocks until the lock is acquired (no timeout), so FF_FS_TIMEOUT
 * is not applied. This is safe because the holder is always a kernel task
 * that completes the file operation and releases the lock.
 */
int ff_mutex_take(int vol)
{
    task_t *self;

    if (vol < 0 || vol > FF_VOLUMES) return 0;

    self = current_task();
    if (!self) return 0;

    return rt_mutex_lock(&Mutex[vol], self) == EOK;
}

/* Unlock the volume (called on leave file functions) */
void ff_mutex_give(int vol)
{
    task_t *self;

    if (vol < 0 || vol > FF_VOLUMES) return;

    self = current_task();
    if (!self) return;

    rt_mutex_unlock(&Mutex[vol], self);
}

#endif // FF_FS_REENTRANT
