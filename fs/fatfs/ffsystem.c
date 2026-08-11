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

#if FF_USE_LFN == 3 // Use dynamic memory allocation

/*------------------------------------------------------------------------*/
/* Allocate/Free a Memory Block                                           */
/*------------------------------------------------------------------------*/

#    include <mem/alloc.h>

void *ff_memalloc( // Returns pointer to the allocated memory block (null if not enough core)
    UINT msize     // Number of bytes to allocate
)
{
    return malloc((size_t)msize); // Allocate a new memory block
}

void ff_memfree(void *mblock // Pointer to the memory block to free (no effect if null)
)
{
    free(mblock); // Free the memory block
}

#endif

#if FF_FS_REENTRANT // Mutual exclusion

/*------------------------------------------------------------------------*/
/* Definitions of Mutex                                                   */
/*------------------------------------------------------------------------*/

/*
 * One real-time mutex per logical volume plus the system mutex (index
 * FF_VOLUMES). The kernel rt_mutex provides priority inheritance.
 */

static rt_mutex_t Mutex[FF_VOLUMES + 1]; // Table of mutex

/*------------------------------------------------------------------------*/
/* Create a Mutex                                                         */
/*------------------------------------------------------------------------*/
/*
 * This function is called in f_mount function to create a new mutex
 * or semaphore for the volume. When a 0 is returned, the f_mount function
 * fails with FR_INT_ERR.
 */

int ff_mutex_create( // Returns 1:Function succeeded or 0:Could not create the mutex
    int vol          // Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES)
)
{
    if (vol < 0 || vol > FF_VOLUMES) return 0;

    rt_mutex_init(&Mutex[vol], NULL);
    return 1;
}

/*------------------------------------------------------------------------*/
/* Delete a Mutex                                                         */
/*------------------------------------------------------------------------*/
/*
 * This function is called in f_mount function to delete a mutex or
 * semaphore of the volume created with ff_mutex_create function.
 */

void ff_mutex_delete( // Returns 1:Function succeeded or 0:Could not delete due to an error
    int vol           // Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES)
)
{
    if (vol < 0 || vol > FF_VOLUMES) return;

    /* The kernel rt_mutex needs no destruction; no waiter should remain here. */
}

/*------------------------------------------------------------------------*/
/* Request a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/*
 * This function is called on enter file functions to lock the volume.
 * When a 0 is returned, the file function fails with FR_TIMEOUT.
 *
 * The kernel rt_mutex blocks until the lock is acquired (no timeout), so
 * FF_FS_TIMEOUT is not applied. This is safe because the holder is always
 * a kernel task that completes the file operation and releases the lock.
 */

int ff_mutex_take( // Returns 1:Succeeded or 0:Timeout
    int vol        // Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES)
)
{
    task_t *self;

    if (vol < 0 || vol > FF_VOLUMES) return 0;

    self = current_task();
    if (!self) return 0;

    return rt_mutex_lock(&Mutex[vol], self) == EOK;
}

/*------------------------------------------------------------------------*/
/* Release a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/*
 * This function is called on leave file functions to unlock the volume.
 */

void ff_mutex_give(int vol // Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES)
)
{
    task_t *self;

    if (vol < 0 || vol > FF_VOLUMES) return;

    self = current_task();
    if (!self) return;

    rt_mutex_unlock(&Mutex[vol], self);
}

#endif // FF_FS_REENTRANT
