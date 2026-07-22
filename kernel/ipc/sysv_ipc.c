/*
 *
 *      sysv_ipc.c
 *      System V IPC (semaphores, shared memory, message queues) implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <alloc.h>
#include <errno.h>
#include <frame.h>
#include <hhdm.h>
#include <page.h>
#include <printk.h>
#include <process.h>
#include <sched.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sysv_ipc.h>
#include <task.h>
#include <uaccess.h>

/* ------------------------------------------------------------------ */
/*  Internal constants                                                  */
/* ------------------------------------------------------------------ */

#define SEM_MAX_NSEMS   250
#define SEM_MAX_SETS    128

#define SHM_MAX_SEGS    128
#define SHM_MMAP_BASE   0x00007f0000000000ULL
#define SHM_MMAP_STEP   0x0000000010000000ULL
#define SHMLBA          PAGE_4K_SIZE

#define MSG_MAX_QUEUES  128

#define IPC_ID_MASK     0x0000FFFF
#define IPC_SEQ_SHIFT   16
#define IPC_SEQ_MASK    0x0000FFFF

/* ------------------------------------------------------------------ */
/*  Internal structures                                                 */
/* ------------------------------------------------------------------ */

typedef struct sem_array {
        ipc_perm_t   perm;
        uint64_t     ctime;
        uint64_t     otime;
        uint32_t     nsems;
        uint16_t    *values;
        uint32_t    *sempid;
        uint32_t    *semcnt;
        uint32_t    *semzcnt;
        wait_queue_t *waitq;
        spinlock_t   lock;
        int          deleted;
} sem_array_t;

typedef struct sem_undo {
        struct sem_undo *next;
        int              semid;
        uint32_t         nsems;
        int16_t         *adj;
        process_t       *proc;
} sem_undo_t;

typedef struct shm_seg {
        ipc_perm_t   perm;
        size_t       size;
        uint64_t     atime;
        uint64_t     dtime;
        uint64_t     ctime;
        uint32_t     cpid;
        uint32_t     lpid;
        uint32_t     nattch;
        uintptr_t    phys_addr;
        uint32_t     npages;
        spinlock_t   lock;
        int          deleted;
        int          locked;
} shm_seg_t;

typedef struct msg_msg {
        struct msg_msg *next;
        int64_t          type;
        size_t           size;
        char             data[];
} msg_msg_t;

typedef struct msg_queue {
        ipc_perm_t   perm;
        uint64_t     stime;
        uint64_t     rtime;
        uint64_t     ctime;
        uint64_t     cbytes;
        uint32_t     qnum;
        uint32_t     qbytes;
        uint32_t     lspid;
        uint32_t     lrpid;
        msg_msg_t   *head;
        msg_msg_t   *tail;
        wait_queue_t recv_wq;
        wait_queue_t send_wq;
        spinlock_t   lock;
        int          deleted;
} msg_queue_t;

/* ------------------------------------------------------------------ */
/*  Global IPC namespace                                                */
/* ------------------------------------------------------------------ */

static sem_array_t  *sem_sets[SEM_MAX_SETS];
static uint16_t      sem_seq[SEM_MAX_SETS];
static spinlock_t    sem_global_lock;

static shm_seg_t    *shm_segs[SHM_MAX_SEGS];
static uint16_t      shm_seq[SHM_MAX_SEGS];
static spinlock_t    shm_global_lock;

static msg_queue_t  *msg_queues[MSG_MAX_QUEUES];
static uint16_t      msg_seq[MSG_MAX_QUEUES];
static spinlock_t    msg_global_lock;

static sem_undo_t   *sem_undo_list;
static spinlock_t    sem_undo_lock;

/* ------------------------------------------------------------------ */
/*  Common IPC helpers                                                  */
/* ------------------------------------------------------------------ */

static int ipc_perm_check(const ipc_perm_t *perm, int mode)
{
        process_t *proc = process_current();
        if (!proc) return -ESRCH;

        /* Superuser bypass */
        if (proc->uid == 0) return 0;

        if (proc->uid == perm->uid) {
                mode >>= 6;
        } else if (proc->gid == perm->gid) {
                mode >>= 3;
        }

        if ((perm->mode & 0777 & mode) == (mode & 0777)) return 0;
        return -EACCES;
}

/* ---------- ID allocation helpers ---------- */

static int ipc_id_alloc(void **table, uint16_t *seq_table, int max,
                         spinlock_t *lock, void *obj)
{
        spin_lock(lock);
        for (int i = 0; i < max; i++) {
                if (table[i] == NULL) {
                        table[i]     = obj;
                        seq_table[i] = (uint16_t)(seq_table[i] + 1);
                        if (seq_table[i] == 0) seq_table[i] = 1;
                        int id = ((int)seq_table[i] << IPC_SEQ_SHIFT) | i;
                        spin_unlock(lock);
                        return id;
                }
        }
        spin_unlock(lock);
        return -ENOSPC;
}

static void *ipc_id_lookup(void **table, uint16_t *seq_table, int max,
                            spinlock_t *lock, int id)
{
        int idx = id & IPC_ID_MASK;
        if (idx < 0 || idx >= max) return NULL;
        uint16_t seq = (uint16_t)((id >> IPC_SEQ_SHIFT) & IPC_SEQ_MASK);

        spin_lock(lock);
        void *obj = table[idx];
        if (obj == NULL || seq_table[idx] != seq) {
                spin_unlock(lock);
                return NULL;
        }
        spin_unlock(lock);
        return obj;
}

static int ipc_id_remove(void **table, uint16_t *seq_table, int max,
                          spinlock_t *lock, int id)
{
        int idx = id & IPC_ID_MASK;
        if (idx < 0 || idx >= max) return -EINVAL;
        uint16_t seq = (uint16_t)((id >> IPC_SEQ_SHIFT) & IPC_SEQ_MASK);

        spin_lock(lock);
        if (table[idx] == NULL || seq_table[idx] != seq) {
                spin_unlock(lock);
                return -EINVAL;
        }
        table[idx] = NULL;
        spin_unlock(lock);
        return 0;
}

/* ------------------------------------------------------------------ */
/*  Semaphore undo helpers                                              */
/* ------------------------------------------------------------------ */

static sem_undo_t *sem_undo_find(process_t *proc, int semid)
{
        sem_undo_t *u;
        for (u = sem_undo_list; u != NULL; u = u->next) {
                if (u->proc == proc && u->semid == semid) return u;
        }
        return NULL;
}

static void sem_undo_apply(sem_undo_t *u, sem_array_t *sem)
{
        spin_lock(&sem->lock);
        uint32_t n = u->nsems;
        if (n > sem->nsems) n = sem->nsems;
        for (uint32_t i = 0; i < n; i++) {
                sem->values[i] = (uint16_t)((int32_t)sem->values[i] + u->adj[i]);
        }
        spin_unlock(&sem->lock);
}

__attribute__((unused)) static void sem_undo_release_process(process_t *proc)
{
        spin_lock(&sem_undo_lock);
        sem_undo_t **prev = &sem_undo_list;
        while (*prev != NULL) {
                sem_undo_t *u = *prev;
                if (u->proc == proc) {
                        *prev = u->next;
                        sem_array_t *sem = (sem_array_t *)ipc_id_lookup(
                                (void **)sem_sets, sem_seq, SEM_MAX_SETS,
                                &sem_global_lock, u->semid);
                        if (sem != NULL) {
                                sem_undo_apply(u, sem);
                                wait_queue_wake_all(&sem->waitq[0]);
                        }
                        free(u->adj);
                        free(u);
                        continue;
                }
                prev = &u->next;
        }
        spin_unlock(&sem_undo_lock);
}

/* ------------------------------------------------------------------ */
/*  Semaphore subsystem                                                 */
/* ------------------------------------------------------------------ */

/*
 *  sys_semget - get or create a semaphore set
 */
int64_t sys_semget(key_t key, int nsems, int semflg)
{
        if (nsems < 0 || nsems > SEM_MAX_NSEMS) return -EINVAL;
        if (nsems == 0 && (semflg & IPC_CREAT)) return -EINVAL;

        /* Search for existing set by key (non-zero key only) */
        if (key != IPC_PRIVATE) {
                spin_lock(&sem_global_lock);
                for (int i = 0; i < SEM_MAX_SETS; i++) {
                        if (sem_sets[i] != NULL && sem_sets[i]->perm.key == key) {
                                sem_array_t *sem = sem_sets[i];
                                spin_unlock(&sem_global_lock);

                                if (semflg & (IPC_CREAT | IPC_EXCL)) {
                                        if ((semflg & (IPC_CREAT | IPC_EXCL)) ==
                                            (IPC_CREAT | IPC_EXCL))
                                                return -EEXIST;
                                }

                                int ret = ipc_perm_check(&sem->perm,
                                                         semflg & 0777);
                                if (ret < 0) return ret;

                                if (nsems > 0 && (uint32_t)nsems > sem->nsems)
                                        return -EINVAL;

                                return ((int)sem->perm.seq << IPC_SEQ_SHIFT) | i;
                        }
                }
                spin_unlock(&sem_global_lock);
        }

        if (!(semflg & IPC_CREAT)) return -ENOENT;

        /* Allocate new semaphore set */
        sem_array_t *sem = malloc(sizeof(sem_array_t));
        if (sem == NULL) return -ENOMEM;
        memset(sem, 0, sizeof(sem_array_t));

        sem->values = malloc(sizeof(uint16_t) * (uint32_t)nsems);
        if (sem->values == NULL) { free(sem); return -ENOMEM; }
        memset(sem->values, 0, sizeof(uint16_t) * (uint32_t)nsems);

        sem->sempid = malloc(sizeof(uint32_t) * (uint32_t)nsems);
        if (sem->sempid == NULL) { free(sem->values); free(sem); return -ENOMEM; }
        memset(sem->sempid, 0, sizeof(uint32_t) * (uint32_t)nsems);

        sem->semcnt = malloc(sizeof(uint32_t) * (uint32_t)nsems);
        if (sem->semcnt == NULL) {
                free(sem->sempid); free(sem->values); free(sem); return -ENOMEM;
        }
        memset(sem->semcnt, 0, sizeof(uint32_t) * (uint32_t)nsems);

        sem->semzcnt = malloc(sizeof(uint32_t) * (uint32_t)nsems);
        if (sem->semzcnt == NULL) {
                free(sem->semcnt); free(sem->sempid);
                free(sem->values); free(sem); return -ENOMEM;
        }
        memset(sem->semzcnt, 0, sizeof(uint32_t) * (uint32_t)nsems);

        sem->waitq = malloc(sizeof(wait_queue_t) * (uint32_t)nsems);
        if (sem->waitq == NULL) {
                free(sem->semzcnt); free(sem->semcnt); free(sem->sempid);
                free(sem->values); free(sem); return -ENOMEM;
        }
        for (uint32_t i = 0; i < (uint32_t)nsems; i++) {
                wait_queue_init(&sem->waitq[i]);
        }

        sem->nsems = (uint32_t)nsems;
        sem->ctime = sched_ticks();
        sem->otime = 0;

        process_t *proc = process_current();
        sem->perm.uid  = proc ? proc->uid : 0;
        sem->perm.gid  = proc ? proc->gid : 0;
        sem->perm.cuid = sem->perm.uid;
        sem->perm.cgid = sem->perm.gid;
        sem->perm.mode = (uint32_t)(semflg & 0777);
        sem->perm.key  = key;

        int id = ipc_id_alloc((void **)sem_sets, sem_seq, SEM_MAX_SETS,
                              &sem_global_lock, sem);
        if (id < 0) {
                free(sem->waitq); free(sem->semzcnt); free(sem->semcnt);
                free(sem->sempid); free(sem->values); free(sem);
                return id;
        }

        sem->perm.seq = (uint32_t)((id >> IPC_SEQ_SHIFT) & IPC_SEQ_MASK);
        return id;
}

/*
 *  sys_semop - perform semaphore operations atomically
 */
int64_t sys_semop(int semid, sembuf_t *sops, size_t nsops)
{
        return sys_semtimedop(semid, sops, nsops, NULL);
}

/*
 *  sys_semtimedop - timed semaphore operations
 */
int64_t sys_semtimedop(int semid, sembuf_t *sops, size_t nsops,
                        const void *timeout)
{
        if (nsops == 0 || nsops > SEMOPM) return -EINVAL;
        if (sops == NULL) return -EFAULT;

        sem_array_t *sem = (sem_array_t *)ipc_id_lookup(
                (void **)sem_sets, sem_seq, SEM_MAX_SETS, &sem_global_lock, semid);
        if (sem == NULL) return -EINVAL;

        /* Copy user sembuf array into kernel memory */
        size_t sop_size = nsops * sizeof(sembuf_t);
        sembuf_t *ksops = malloc(sop_size);
        if (ksops == NULL) return -ENOMEM;

        if (copy_from_user(ksops, sops, sop_size) != 0) {
                free(ksops);
                return -EFAULT;
        }

        /* Validate all sem_num values */
        for (size_t i = 0; i < nsops; i++) {
                if (ksops[i].sem_num >= sem->nsems) {
                        free(ksops);
                        return -EFBIG;
                }
        }

        int ret = ipc_perm_check(&sem->perm,
                                  (nsops > 0 && ksops[0].sem_op >= 0) ? 0222 : 0222);
        if (ret < 0) { free(ksops); return ret; }

        /* Check read permission for sem_op == 0 (wait for zero) */
        for (size_t i = 0; i < nsops; i++) {
                if (ksops[i].sem_op == 0) {
                        ret = ipc_perm_check(&sem->perm, 0444);
                        if (ret < 0) { free(ksops); return ret; }
                        break;
                }
        }

        /* Build undo adjustments if needed */
        int16_t *undo_adj = NULL;
        int has_undo = 0;
        for (size_t i = 0; i < nsops; i++) {
                if (ksops[i].sem_flg & SEM_UNDO) {
                        has_undo = 1;
                        break;
                }
        }
        if (has_undo) {
                undo_adj = malloc(sizeof(int16_t) * sem->nsems);
                if (undo_adj == NULL) { free(ksops); return -ENOMEM; }
                memset(undo_adj, 0, sizeof(int16_t) * sem->nsems);
        }

        process_t *proc = process_current();
        uint64_t   deadline = 0;
        int        has_timeout = 0;

        if (timeout != NULL) {
                /* Simple timeout: read a uint64_t * ticks value */
                uint64_t user_deadline;
                if (copy_from_user(&user_deadline, timeout,
                                   sizeof(uint64_t)) != 0) {
                        free(undo_adj); free(ksops);
                        return -EFAULT;
                }
                deadline    = user_deadline;
                has_timeout = 1;
        }

        for (;;) {
                spin_lock(&sem->lock);

                if (sem->deleted) {
                        spin_unlock(&sem->lock);
                        free(undo_adj); free(ksops);
                        return -EIDRM;
                }

                /* Check if all operations can proceed */
                int would_block = 0;
                for (size_t i = 0; i < nsops; i++) {
                        uint16_t snum = ksops[i].sem_num;
                        if (ksops[i].sem_op < 0) {
                                if ((int16_t)sem->values[snum] <
                                    (int16_t)(-ksops[i].sem_op)) {
                                        would_block = 1;
                                        break;
                                }
                        } else if (ksops[i].sem_op == 0) {
                                if (sem->values[snum] != 0) {
                                        would_block = 1;
                                        break;
                                }
                        }
                }

                if (would_block) {
                        if (ksops[0].sem_flg & IPC_NOWAIT) {
                                spin_unlock(&sem->lock);
                                free(undo_adj); free(ksops);
                                return -EAGAIN;
                        }

                        /* Check timeout */
                        if (has_timeout && sched_ticks() >= deadline) {
                                spin_unlock(&sem->lock);
                                free(undo_adj); free(ksops);
                                return -EAGAIN;
                        }

                        /* Block on the first semaphore that would block */
                        for (size_t i = 0; i < nsops; i++) {
                                uint16_t snum = ksops[i].sem_num;
                                if (ksops[i].sem_op < 0) {
                                        sem->semcnt[snum]++;
                                } else if (ksops[i].sem_op == 0) {
                                        sem->semzcnt[snum]++;
                                }
                        }
                        spin_unlock(&sem->lock);

                        /* Wait on the first semaphore's wait queue */
                        wait_queue_wait(&sem->waitq[ksops[0].sem_num]);

                        /* After waking, decrement wait counts */
                        spin_lock(&sem->lock);
                        for (size_t i = 0; i < nsops; i++) {
                                uint16_t snum = ksops[i].sem_num;
                                if (ksops[i].sem_op < 0 && sem->semcnt[snum] > 0) {
                                        sem->semcnt[snum]--;
                                } else if (ksops[i].sem_op == 0 &&
                                           sem->semzcnt[snum] > 0) {
                                        sem->semzcnt[snum]--;
                                }
                        }
                        spin_unlock(&sem->lock);

                        /* Retry */
                        continue;
                }

                /* Apply all operations atomically */
                for (size_t i = 0; i < nsops; i++) {
                        uint16_t snum = ksops[i].sem_num;
                        if (ksops[i].sem_op > 0) {
                                sem->values[snum] =
                                        (uint16_t)((int32_t)sem->values[snum] +
                                                    ksops[i].sem_op);
                        } else if (ksops[i].sem_op < 0) {
                                sem->values[snum] =
                                        (uint16_t)((int32_t)sem->values[snum] +
                                                    ksops[i].sem_op);
                        }
                        /* sem_op == 0: no change to value */

                        if (ksops[i].sem_op != 0) {
                                sem->sempid[snum] = proc ? (uint32_t)proc->task->pid
                                                         : 0;
                        }

                        if (ksops[i].sem_flg & SEM_UNDO) {
                                undo_adj[snum] = (int16_t)(undo_adj[snum] -
                                                            ksops[i].sem_op);
                        }
                }

                sem->otime = sched_ticks();
                spin_unlock(&sem->lock);

                /* Record undo adjustments */
                if (has_undo && proc != NULL) {
                        spin_lock(&sem_undo_lock);
                        sem_undo_t *u = sem_undo_find(proc, semid);
                        if (u == NULL) {
                                u = malloc(sizeof(sem_undo_t));
                                if (u != NULL) {
                                        memset(u, 0, sizeof(sem_undo_t));
                                        u->semid = semid;
                                        u->nsems = sem->nsems;
                                        u->proc  = proc;
                                        u->adj   = malloc(sizeof(int16_t) *
                                                          sem->nsems);
                                        if (u->adj != NULL) {
                                                memset(u->adj, 0,
                                                       sizeof(int16_t) *
                                                               sem->nsems);
                                        }
                                        u->next = sem_undo_list;
                                        sem_undo_list = u;
                                }
                        }
                        if (u != NULL && u->adj != NULL) {
                                for (uint32_t i = 0; i < sem->nsems; i++) {
                                        u->adj[i] = (int16_t)(u->adj[i] +
                                                               undo_adj[i]);
                                }
                        }
                        spin_unlock(&sem_undo_lock);
                }

                /* Wake up any waiters */
                for (uint32_t i = 0; i < sem->nsems; i++) {
                        if (sem->values[i] > 0 || sem->semzcnt[i] > 0) {
                                wait_queue_wake_all(&sem->waitq[i]);
                        }
                }

                free(undo_adj);
                free(ksops);
                return 0;
        }
}

/*
 *  sys_semctl - semaphore control operations
 */
int64_t sys_semctl(int semid, int semnum, int cmd, uint64_t arg)
{
        sem_array_t *sem = (sem_array_t *)ipc_id_lookup(
                (void **)sem_sets, sem_seq, SEM_MAX_SETS, &sem_global_lock, semid);
        if (sem == NULL) return -EINVAL;

        switch (cmd) {
        case IPC_RMID: {
                int ret = ipc_perm_check(&sem->perm, 0);
                if (ret < 0) return ret;

                int rem = ipc_id_remove((void **)sem_sets, sem_seq,
                                        SEM_MAX_SETS, &sem_global_lock, semid);
                if (rem < 0) return rem;

                spin_lock(&sem->lock);
                sem->deleted = 1;
                spin_unlock(&sem->lock);

                /* Wake all waiters */
                for (uint32_t i = 0; i < sem->nsems; i++) {
                        wait_queue_wake_all(&sem->waitq[i]);
                }

                free(sem->waitq);
                free(sem->semzcnt);
                free(sem->semcnt);
                free(sem->sempid);
                free(sem->values);
                free(sem);
                return 0;
        }

        case IPC_SET: {
                int ret = ipc_perm_check(&sem->perm, 0);
                if (ret < 0) return ret;
                if (arg == 0) return -EFAULT;

                semid_ds_t ds;
                if (copy_from_user(&ds, (void *)arg, sizeof(semid_ds_t)) != 0)
                        return -EFAULT;

                spin_lock(&sem->lock);
                sem->perm.uid  = ds.sem_perm.uid;
                sem->perm.gid  = ds.sem_perm.gid;
                sem->perm.mode = (ds.sem_perm.mode & 0777) |
                                 (sem->perm.mode & ~0777U);
                sem->ctime     = sched_ticks();
                spin_unlock(&sem->lock);
                return 0;
        }

        case IPC_STAT: {
                int ret = ipc_perm_check(&sem->perm, 0444);
                if (ret < 0) return ret;
                if (arg == 0) return -EFAULT;

                semid_ds_t ds;
                memset(&ds, 0, sizeof(ds));
                spin_lock(&sem->lock);
                ds.sem_perm  = sem->perm;
                ds.sem_otime = sem->otime;
                ds.sem_ctime = sem->ctime;
                ds.sem_nsems = sem->nsems;
                spin_unlock(&sem->lock);

                if (copy_to_user((void *)arg, &ds, sizeof(semid_ds_t)) != 0)
                        return -EFAULT;
                return 0;
        }

        case GETVAL: {
                int ret = ipc_perm_check(&sem->perm, 0444);
                if (ret < 0) return ret;
                if (semnum < 0 || (uint32_t)semnum >= sem->nsems)
                        return -EINVAL;

                spin_lock(&sem->lock);
                int16_t val = (int16_t)sem->values[semnum];
                spin_unlock(&sem->lock);
                return val;
        }

        case SETVAL: {
                int ret = ipc_perm_check(&sem->perm, 0222);
                if (ret < 0) return ret;
                if (semnum < 0 || (uint32_t)semnum >= sem->nsems)
                        return -EINVAL;

                int16_t val = (int16_t)(arg & 0xFFFF);
                spin_lock(&sem->lock);
                sem->values[semnum] = (uint16_t)val;
                sem->sempid[semnum] = process_current()
                                              ? (uint32_t)process_current()
                                                        ->task->pid
                                              : 0;
                sem->ctime = sched_ticks();
                spin_unlock(&sem->lock);

                wait_queue_wake_all(&sem->waitq[semnum]);
                return 0;
        }

        case GETALL: {
                int ret = ipc_perm_check(&sem->perm, 0444);
                if (ret < 0) return ret;
                if (arg == 0) return -EFAULT;

                uint16_t *vals = malloc(sizeof(uint16_t) * sem->nsems);
                if (vals == NULL) return -ENOMEM;

                spin_lock(&sem->lock);
                memcpy(vals, sem->values, sizeof(uint16_t) * sem->nsems);
                spin_unlock(&sem->lock);

                if (copy_to_user((void *)arg, vals,
                                 sizeof(uint16_t) * sem->nsems) != 0) {
                        free(vals);
                        return -EFAULT;
                }
                free(vals);
                return 0;
        }

        case SETALL: {
                int ret = ipc_perm_check(&sem->perm, 0222);
                if (ret < 0) return ret;
                if (arg == 0) return -EFAULT;

                uint16_t *vals = malloc(sizeof(uint16_t) * sem->nsems);
                if (vals == NULL) return -ENOMEM;

                if (copy_from_user(vals, (void *)arg,
                                   sizeof(uint16_t) * sem->nsems) != 0) {
                        free(vals);
                        return -EFAULT;
                }

                spin_lock(&sem->lock);
                memcpy(sem->values, vals, sizeof(uint16_t) * sem->nsems);
                sem->ctime = sched_ticks();
                spin_unlock(&sem->lock);

                for (uint32_t i = 0; i < sem->nsems; i++) {
                        wait_queue_wake_all(&sem->waitq[i]);
                }
                free(vals);
                return 0;
        }

        case GETPID: {
                int ret = ipc_perm_check(&sem->perm, 0444);
                if (ret < 0) return ret;
                if (semnum < 0 || (uint32_t)semnum >= sem->nsems)
                        return -EINVAL;

                spin_lock(&sem->lock);
                uint32_t pid = sem->sempid[semnum];
                spin_unlock(&sem->lock);
                return (int64_t)pid;
        }

        case GETNCNT: {
                int ret = ipc_perm_check(&sem->perm, 0444);
                if (ret < 0) return ret;
                if (semnum < 0 || (uint32_t)semnum >= sem->nsems)
                        return -EINVAL;

                spin_lock(&sem->lock);
                uint32_t cnt = sem->semcnt[semnum];
                spin_unlock(&sem->lock);
                return (int64_t)cnt;
        }

        case GETZCNT: {
                int ret = ipc_perm_check(&sem->perm, 0444);
                if (ret < 0) return ret;
                if (semnum < 0 || (uint32_t)semnum >= sem->nsems)
                        return -EINVAL;

                spin_lock(&sem->lock);
                uint32_t cnt = sem->semzcnt[semnum];
                spin_unlock(&sem->lock);
                return (int64_t)cnt;
        }

        case IPC_INFO:
        case SEM_INFO: {
                if (arg == 0) return -EFAULT;
                seminfo_t info;
                memset(&info, 0, sizeof(info));
                info.semmni = SEM_MAX_SETS;
                info.semmns = SEM_MAX_SETS * SEM_MAX_NSEMS;
                info.semmsl = SEM_MAX_NSEMS;
                info.semopm = SEMOPM;
                info.semvmx = 32767;
                info.semusz = (int32_t)sizeof(sem_array_t);
                if (copy_to_user((void *)arg, &info, sizeof(seminfo_t)) != 0)
                        return -EFAULT;

                if (cmd == SEM_INFO) {
                        spin_lock(&sem_global_lock);
                        int used = 0;
                        for (int i = 0; i < SEM_MAX_SETS; i++) {
                                if (sem_sets[i] != NULL) used++;
                        }
                        spin_unlock(&sem_global_lock);
                        return used;
                }
                return SEM_MAX_SETS;
        }

        case SEM_STAT: {
                if (arg == 0) return -EFAULT;
                int idx = semid & IPC_ID_MASK;
                if (idx < 0 || idx >= SEM_MAX_SETS) return -EINVAL;

                spin_lock(&sem_global_lock);
                sem_array_t *s = sem_sets[idx];
                if (s == NULL) { spin_unlock(&sem_global_lock); return -EINVAL; }
                semid_ds_t ds;
                memset(&ds, 0, sizeof(ds));
                ds.sem_perm  = s->perm;
                ds.sem_otime = s->otime;
                ds.sem_ctime = s->ctime;
                ds.sem_nsems = s->nsems;
                spin_unlock(&sem_global_lock);

                if (copy_to_user((void *)arg, &ds, sizeof(semid_ds_t)) != 0)
                        return -EFAULT;
                return ((int)s->perm.seq << IPC_SEQ_SHIFT) | idx;
        }

        default:
                return -EINVAL;
        }
}

/* ------------------------------------------------------------------ */
/*  Shared memory subsystem                                             */
/* ------------------------------------------------------------------ */

/*
 *  sys_shmget - get or create a shared memory segment
 */
int64_t sys_shmget(key_t key, size_t size, int shmflg)
{
        if (size > SHM_SIZE_MAX) return -EINVAL;

        /* Search for existing segment by key */
        if (key != IPC_PRIVATE) {
                spin_lock(&shm_global_lock);
                for (int i = 0; i < SHM_MAX_SEGS; i++) {
                        if (shm_segs[i] != NULL &&
                            shm_segs[i]->perm.key == key) {
                                shm_seg_t *seg = shm_segs[i];
                                spin_unlock(&shm_global_lock);

                                if (shmflg & (IPC_CREAT | IPC_EXCL)) {
                                        if ((shmflg & (IPC_CREAT | IPC_EXCL)) ==
                                            (IPC_CREAT | IPC_EXCL))
                                                return -EEXIST;
                                }

                                int ret = ipc_perm_check(&seg->perm,
                                                         shmflg & 0777);
                                if (ret < 0) return ret;

                                if (size > 0 && size > seg->size)
                                        return -EINVAL;

                                return ((int)seg->perm.seq << IPC_SEQ_SHIFT) |
                                       i;
                        }
                }
                spin_unlock(&shm_global_lock);
        }

        if (!(shmflg & IPC_CREAT)) return -ENOENT;

        if (size == 0) return -EINVAL;
        size_t pages = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
        if (pages == 0) pages = 1;

        /* Allocate physical frames */
        uint64_t phys = alloc_frames(pages);
        if (phys == 0) return -ENOMEM;

        /* Zero the physical memory */
        void *virt = phys_to_virt(phys);
        memset(virt, 0, pages * PAGE_4K_SIZE);

        /* Allocate segment descriptor */
        shm_seg_t *seg = malloc(sizeof(shm_seg_t));
        if (seg == NULL) {
                free_frames(phys, pages);
                return -ENOMEM;
        }
        memset(seg, 0, sizeof(shm_seg_t));

        seg->size      = pages * PAGE_4K_SIZE;
        seg->phys_addr = (uintptr_t)phys;
        seg->npages    = (uint32_t)pages;
        seg->ctime     = sched_ticks();
        seg->atime     = 0;
        seg->dtime     = 0;
        seg->nattch    = 0;

        process_t *proc = process_current();
        seg->cpid      = proc ? (uint32_t)proc->task->pid : 0;
        seg->lpid      = 0;
        seg->perm.uid  = proc ? proc->uid : 0;
        seg->perm.gid  = proc ? proc->gid : 0;
        seg->perm.cuid = seg->perm.uid;
        seg->perm.cgid = seg->perm.gid;
        seg->perm.mode = (uint32_t)(shmflg & 0777);
        seg->perm.key  = key;

        int id = ipc_id_alloc((void **)shm_segs, shm_seq, SHM_MAX_SEGS,
                              &shm_global_lock, seg);
        if (id < 0) {
                free_frames(phys, pages);
                free(seg);
                return id;
        }

        seg->perm.seq = (uint32_t)((id >> IPC_SEQ_SHIFT) & IPC_SEQ_MASK);
        return id;
}

/*
 *  sys_shmat - attach shared memory segment
 */
int64_t sys_shmat(int shmid, const void *shmaddr, int shmflg)
{
        shm_seg_t *seg = (shm_seg_t *)ipc_id_lookup(
                (void **)shm_segs, shm_seq, SHM_MAX_SEGS, &shm_global_lock, shmid);
        if (seg == NULL) return -EINVAL;

        int ret = ipc_perm_check(&seg->perm,
                                 (shmflg & SHM_RDONLY) ? 0444 : 0666);
        if (ret < 0) return ret;

        process_t *proc = process_current();
        if (proc == NULL) return -ESRCH;

        /* Determine the virtual address for the mapping */
        uintptr_t vaddr;
        if (shmaddr != NULL && (shmflg & SHM_REMAP)) {
                vaddr = (uintptr_t)shmaddr;
        } else if (shmaddr != NULL) {
                vaddr = (uintptr_t)shmaddr;
                if (shmflg & SHM_RND) {
                        vaddr &= ~(SHMLBA - 1);
                }
        } else {
                /* Find a free address: use a simple incrementing allocator */
                static uintptr_t next_shm_addr = SHM_MMAP_BASE;
                spin_lock(&shm_global_lock);
                vaddr          = next_shm_addr;
                next_shm_addr += SHM_MMAP_STEP;
                if (next_shm_addr < SHM_MMAP_BASE) {
                        next_shm_addr = SHM_MMAP_BASE;
                }
                spin_unlock(&shm_global_lock);
        }

        /* Map the physical pages into the process */
        vm_flags_t flags = VM_SHARED;
        if (!(shmflg & SHM_RDONLY)) flags |= VM_WRITE;
        if (shmflg & SHM_EXEC) flags |= VM_EXEC;
        flags |= VM_READ;

        spin_lock(&seg->lock);
        for (uint32_t i = 0; i < seg->npages; i++) {
                uint64_t frame = seg->phys_addr + i * PAGE_4K_SIZE;
                uint64_t pte_flags = PTE_USER | PTE_PRESENT;
                if (flags & VM_WRITE) pte_flags |= PTE_WRITEABLE;
                if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;

                page_map_to(proc->user_page_dir,
                            vaddr + i * PAGE_4K_SIZE, frame, pte_flags);
        }

        seg->nattch++;
        seg->atime = sched_ticks();
        seg->lpid  = (uint32_t)proc->task->pid;
        spin_unlock(&seg->lock);

        /* Register a VMA for tracking */
        vm_area_t *vma = malloc(sizeof(vm_area_t));
        if (vma != NULL) {
                memset(vma, 0, sizeof(vm_area_t));
                vma->start = vaddr;
                vma->end   = vaddr + seg->size;
                vma->flags = flags;
                vma->type  = VM_REGION_MMAP;
                vma->next  = NULL;

                spin_lock(&proc->mmap_lock);
                vm_area_t **prev = &proc->mmap_list;
                while (*prev != NULL) prev = &(*prev)->next;
                *prev = vma;
                spin_unlock(&proc->mmap_lock);
        }

        return (int64_t)vaddr;
}

/*
 *  sys_shmdt - detach shared memory segment
 */
int64_t sys_shmdt(const void *shmaddr)
{
        if (shmaddr == NULL) return -EINVAL;

        process_t *proc = process_current();
        if (proc == NULL) return -ESRCH;

        uintptr_t vaddr = (uintptr_t)shmaddr;

        /* Find and remove the VMA */
        spin_lock(&proc->mmap_lock);
        vm_area_t **prev = &proc->mmap_list;
        vm_area_t  *found = NULL;
        while (*prev != NULL) {
                if ((*prev)->start == vaddr) {
                        found = *prev;
                        *prev = found->next;
                        break;
                }
                prev = &(*prev)->next;
        }
        spin_unlock(&proc->mmap_lock);

        if (found == NULL) return -EINVAL;

        size_t   length = found->end - found->start;
        uint32_t npages = (uint32_t)((length + PAGE_4K_SIZE - 1) /
                                     PAGE_4K_SIZE);
        free(found);

        /* Find the matching shm segment by size */
        spin_lock(&shm_global_lock);
        for (int i = 0; i < SHM_MAX_SEGS; i++) {
                shm_seg_t *seg = shm_segs[i];
                if (seg == NULL) continue;

                spin_lock(&seg->lock);
                if (seg->npages == npages && seg->nattch > 0) {
                        seg->nattch--;
                        seg->dtime = sched_ticks();
                        seg->lpid  = (uint32_t)proc->task->pid;

                        /* Clean up if segment was marked for deletion */
                        if (seg->deleted && seg->nattch == 0) {
                                spin_unlock(&seg->lock);
                                spin_unlock(&shm_global_lock);
                                free_frames(seg->phys_addr, seg->npages);
                                free(seg);
                                return 0;
                        }
                        spin_unlock(&seg->lock);
                        spin_unlock(&shm_global_lock);
                        return 0;
                }
                spin_unlock(&seg->lock);
        }
        spin_unlock(&shm_global_lock);

        return 0;
}

/*
 *  sys_shmctl - shared memory control operations
 */
int64_t sys_shmctl(int shmid, int cmd, void *buf)
{
        shm_seg_t *seg = (shm_seg_t *)ipc_id_lookup(
                (void **)shm_segs, shm_seq, SHM_MAX_SEGS, &shm_global_lock, shmid);
        if (seg == NULL) return -EINVAL;

        switch (cmd) {
        case IPC_RMID: {
                int ret = ipc_perm_check(&seg->perm, 0);
                if (ret < 0) return ret;

                int rem = ipc_id_remove((void **)shm_segs, shm_seq,
                                        SHM_MAX_SEGS, &shm_global_lock, shmid);
                if (rem < 0) return rem;

                spin_lock(&seg->lock);
                seg->deleted = 1;
                int nattch    = (int)seg->nattch;
                spin_unlock(&seg->lock);

                if (nattch == 0) {
                        free_frames(seg->phys_addr, seg->npages);
                        free(seg);
                }
                return 0;
        }

        case IPC_SET: {
                int ret = ipc_perm_check(&seg->perm, 0);
                if (ret < 0) return ret;
                if (buf == NULL) return -EFAULT;

                shmid_ds_t ds;
                if (copy_from_user(&ds, buf, sizeof(shmid_ds_t)) != 0)
                        return -EFAULT;

                spin_lock(&seg->lock);
                seg->perm.uid  = ds.shm_perm.uid;
                seg->perm.gid  = ds.shm_perm.gid;
                seg->perm.mode = (ds.shm_perm.mode & 0777) |
                                 (seg->perm.mode & ~0777U);
                seg->ctime     = sched_ticks();
                spin_unlock(&seg->lock);
                return 0;
        }

        case IPC_STAT: {
                int ret = ipc_perm_check(&seg->perm, 0444);
                if (ret < 0) return ret;
                if (buf == NULL) return -EFAULT;

                shmid_ds_t ds;
                memset(&ds, 0, sizeof(ds));
                spin_lock(&seg->lock);
                ds.shm_perm  = seg->perm;
                ds.shm_segsz = seg->size;
                ds.shm_atime = seg->atime;
                ds.shm_dtime = seg->dtime;
                ds.shm_ctime = seg->ctime;
                ds.shm_cpid  = seg->cpid;
                ds.shm_lpid  = seg->lpid;
                ds.shm_nattch = seg->nattch;
                spin_unlock(&seg->lock);

                if (copy_to_user(buf, &ds, sizeof(shmid_ds_t)) != 0)
                        return -EFAULT;
                return 0;
        }

        case SHM_LOCK: {
                int ret = ipc_perm_check(&seg->perm, 0);
                if (ret < 0) return ret;
                spin_lock(&seg->lock);
                seg->locked = 1;
                spin_unlock(&seg->lock);
                return 0;
        }

        case SHM_UNLOCK: {
                int ret = ipc_perm_check(&seg->perm, 0);
                if (ret < 0) return ret;
                spin_lock(&seg->lock);
                seg->locked = 0;
                spin_unlock(&seg->lock);
                return 0;
        }

        case IPC_INFO:
        case SHM_INFO: {
                if (buf == NULL) return -EFAULT;
                shminfo_t info;
                memset(&info, 0, sizeof(info));
                info.shmmax = SHM_SIZE_MAX;
                info.shmmin = 1;
                info.shmmni = SHM_MAX_SEGS;
                info.shmseg = SHM_MAX_SEGS;
                info.shmall = SHM_MAX_SEGS * 256;
                if (copy_to_user(buf, &info, sizeof(shminfo_t)) != 0)
                        return -EFAULT;

                if (cmd == SHM_INFO) {
                        spin_lock(&shm_global_lock);
                        int used = 0;
                        for (int i = 0; i < SHM_MAX_SEGS; i++) {
                                if (shm_segs[i] != NULL) used++;
                        }
                        spin_unlock(&shm_global_lock);
                        return used;
                }
                return SHM_MAX_SEGS;
        }

        case SHM_STAT: {
                if (buf == NULL) return -EFAULT;
                int idx = shmid & IPC_ID_MASK;
                if (idx < 0 || idx >= SHM_MAX_SEGS) return -EINVAL;

                spin_lock(&shm_global_lock);
                shm_seg_t *s = shm_segs[idx];
                if (s == NULL) {
                        spin_unlock(&shm_global_lock);
                        return -EINVAL;
                }
                shmid_ds_t ds;
                memset(&ds, 0, sizeof(ds));
                ds.shm_perm   = s->perm;
                ds.shm_segsz  = s->size;
                ds.shm_atime  = s->atime;
                ds.shm_dtime  = s->dtime;
                ds.shm_ctime  = s->ctime;
                ds.shm_cpid   = s->cpid;
                ds.shm_lpid   = s->lpid;
                ds.shm_nattch = s->nattch;
                spin_unlock(&shm_global_lock);

                if (copy_to_user(buf, &ds, sizeof(shmid_ds_t)) != 0)
                        return -EFAULT;
                return ((int)s->perm.seq << IPC_SEQ_SHIFT) | idx;
        }

        default:
                return -EINVAL;
        }
}

/* ------------------------------------------------------------------ */
/*  Message queue subsystem                                             */
/* ------------------------------------------------------------------ */

/*
 *  sys_msgget - get or create a message queue
 */
int64_t sys_msgget(key_t key, int msgflg)
{
        /* Search for existing queue by key */
        if (key != IPC_PRIVATE) {
                spin_lock(&msg_global_lock);
                for (int i = 0; i < MSG_MAX_QUEUES; i++) {
                        if (msg_queues[i] != NULL &&
                            msg_queues[i]->perm.key == key) {
                                msg_queue_t *q = msg_queues[i];
                                spin_unlock(&msg_global_lock);

                                if (msgflg & (IPC_CREAT | IPC_EXCL)) {
                                        if ((msgflg & (IPC_CREAT | IPC_EXCL)) ==
                                            (IPC_CREAT | IPC_EXCL))
                                                return -EEXIST;
                                }

                                int ret = ipc_perm_check(&q->perm,
                                                         msgflg & 0777);
                                if (ret < 0) return ret;

                                return ((int)q->perm.seq << IPC_SEQ_SHIFT) | i;
                        }
                }
                spin_unlock(&msg_global_lock);
        }

        if (!(msgflg & IPC_CREAT)) return -ENOENT;

        /* Allocate new message queue */
        msg_queue_t *q = malloc(sizeof(msg_queue_t));
        if (q == NULL) return -ENOMEM;
        memset(q, 0, sizeof(msg_queue_t));

        q->qbytes = MSGMNB;
        q->ctime  = sched_ticks();
        wait_queue_init(&q->recv_wq);
        wait_queue_init(&q->send_wq);

        process_t *proc = process_current();
        q->perm.uid  = proc ? proc->uid : 0;
        q->perm.gid  = proc ? proc->gid : 0;
        q->perm.cuid = q->perm.uid;
        q->perm.cgid = q->perm.gid;
        q->perm.mode = (uint32_t)(msgflg & 0777);
        q->perm.key  = key;

        int id = ipc_id_alloc((void **)msg_queues, msg_seq, MSG_MAX_QUEUES,
                              &msg_global_lock, q);
        if (id < 0) {
                free(q);
                return id;
        }

        q->perm.seq = (uint32_t)((id >> IPC_SEQ_SHIFT) & IPC_SEQ_MASK);
        return id;
}

/*
 *  sys_msgsnd - send a message to a queue
 */
int64_t sys_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg)
{
        if (msgsz > MSGMAX) return -EINVAL;
        if (msgp == NULL) return -EFAULT;

        msg_queue_t *q = (msg_queue_t *)ipc_id_lookup(
                (void **)msg_queues, msg_seq, MSG_MAX_QUEUES, &msg_global_lock,
                msqid);
        if (q == NULL) return -EINVAL;

        int ret = ipc_perm_check(&q->perm, 0222);
        if (ret < 0) return ret;

        /* Read the message type from user space */
        int64_t mtype;
        if (copy_from_user(&mtype, msgp, sizeof(int64_t)) != 0)
                return -EFAULT;
        if (mtype <= 0) return -EINVAL;

        /* Allocate kernel message buffer */
        size_t   msg_total = sizeof(msg_msg_t) + msgsz;
        msg_msg_t *msg     = malloc(msg_total);
        if (msg == NULL) return -ENOMEM;
        memset(msg, 0, msg_total);

        msg->type = mtype;
        msg->size = msgsz;
        msg->next = NULL;

        /* Copy message data from user space */
        if (copy_from_user(msg->data, (const char *)msgp + sizeof(int64_t),
                           msgsz) != 0) {
                free(msg);
                return -EFAULT;
        }

        spin_lock(&q->lock);

        if (q->deleted) {
                spin_unlock(&q->lock);
                free(msg);
                return -EIDRM;
        }

        /* Check if queue is full */
        while (q->cbytes + msgsz > q->qbytes || q->qnum >= MSGMNB) {
                if (msgflg & IPC_NOWAIT) {
                        spin_unlock(&q->lock);
                        free(msg);
                        return -EAGAIN;
                }
                spin_unlock(&q->lock);
                wait_queue_wait(&q->send_wq);
                spin_lock(&q->lock);

                if (q->deleted) {
                        spin_unlock(&q->lock);
                        free(msg);
                        return -EIDRM;
                }
        }

        /* Insert into queue (at tail) */
        if (q->tail == NULL) {
                q->head = msg;
                q->tail = msg;
        } else {
                q->tail->next = msg;
                q->tail       = msg;
        }

        q->qnum++;
        q->cbytes += msgsz;
        q->stime = sched_ticks();
        q->lspid = process_current() ? (uint32_t)process_current()->task->pid
                                     : 0;

        spin_unlock(&q->lock);

        /* Wake up a receiver */
        wait_queue_wake_all(&q->recv_wq);
        return 0;
}

/*
 *  sys_msgrcv - receive a message from a queue
 */
int64_t sys_msgrcv(int msqid, void *msgp, size_t msgsz, int64_t msgtyp,
                    int msgflg)
{
        if (msgp == NULL) return -EFAULT;
        if (msgsz == 0) return -EINVAL;

        msg_queue_t *q = (msg_queue_t *)ipc_id_lookup(
                (void **)msg_queues, msg_seq, MSG_MAX_QUEUES, &msg_global_lock,
                msqid);
        if (q == NULL) return -EINVAL;

        int ret = ipc_perm_check(&q->perm, 0444);
        if (ret < 0) return ret;

        spin_lock(&q->lock);

        if (q->deleted) {
                spin_unlock(&q->lock);
                return -EIDRM;
        }

        msg_msg_t  *msg       = NULL;
        msg_msg_t **prev_link = NULL;
        msg_msg_t  *prev_node = NULL;

        for (;;) {
                /* Search for matching message */
                msg       = NULL;
                prev_link = &q->head;
                prev_node = NULL;

                if (msgtyp == 0) {
                        /* First message in queue */
                        msg = q->head;
                } else if (msgtyp > 0) {
                        /* First message of exact type */
                        msg_msg_t *cur = q->head;
                        while (cur != NULL) {
                                if (cur->type == msgtyp) {
                                        msg = cur;
                                        break;
                                }
                                prev_link = &cur->next;
                                prev_node = cur;
                                cur       = cur->next;
                        }
                } else {
                        /* First message of type <= |msgtyp| */
                        int64_t    limit = -msgtyp;
                        msg_msg_t *cur   = q->head;
                        while (cur != NULL) {
                                int match = (cur->type <= limit);
                                if (msgflg & MSG_EXCEPT) {
                                        match = !match;
                                }
                                if (match) {
                                        msg = cur;
                                        break;
                                }
                                prev_link = &cur->next;
                                prev_node = cur;
                                cur       = cur->next;
                        }
                }

                if (msg != NULL) break;

                /* No message found */
                if (msgflg & IPC_NOWAIT) {
                        spin_unlock(&q->lock);
                        return -ENOMSG;
                }

                spin_unlock(&q->lock);
                wait_queue_wait(&q->recv_wq);
                spin_lock(&q->lock);

                if (q->deleted) {
                        spin_unlock(&q->lock);
                        return -EIDRM;
                }
        }

        /* Remove message from queue */
        *prev_link = msg->next;
        if (msg == q->tail) {
                q->tail = prev_node;
        }
        q->qnum--;
        q->cbytes -= msg->size;
        q->rtime = sched_ticks();
        q->lrpid = process_current() ? (uint32_t)process_current()->task->pid
                                     : 0;

        spin_unlock(&q->lock);

        /* Determine how much data to copy */
        size_t copy_size = msg->size;
        if (copy_size > msgsz) {
                if (msgflg & MSG_NOERROR) {
                        copy_size = msgsz;
                } else {
                        free(msg);
                        return -E2BIG;
                }
        }

        /* Copy message type to user */
        if (copy_to_user(msgp, &msg->type, sizeof(int64_t)) != 0) {
                free(msg);
                return -EFAULT;
        }

        /* Copy message data to user */
        if (copy_to_user((char *)msgp + sizeof(int64_t), msg->data,
                         copy_size) != 0) {
                free(msg);
                return -EFAULT;
        }

        free(msg);

        /* Wake up a sender */
        wait_queue_wake_all(&q->send_wq);
        return (int64_t)copy_size;
}

/*
 *  sys_msgctl - message queue control operations
 */
int64_t sys_msgctl(int msqid, int cmd, void *buf)
{
        msg_queue_t *q = (msg_queue_t *)ipc_id_lookup(
                (void **)msg_queues, msg_seq, MSG_MAX_QUEUES, &msg_global_lock,
                msqid);
        if (q == NULL) return -EINVAL;

        switch (cmd) {
        case IPC_RMID: {
                int ret = ipc_perm_check(&q->perm, 0);
                if (ret < 0) return ret;

                int rem = ipc_id_remove((void **)msg_queues, msg_seq,
                                        MSG_MAX_QUEUES, &msg_global_lock, msqid);
                if (rem < 0) return rem;

                spin_lock(&q->lock);
                q->deleted = 1;
                spin_unlock(&q->lock);

                /* Wake all waiters */
                wait_queue_wake_all(&q->recv_wq);
                wait_queue_wake_all(&q->send_wq);

                /* Free all messages in the queue */
                msg_msg_t *m = q->head;
                while (m != NULL) {
                        msg_msg_t *next = m->next;
                        free(m);
                        m = next;
                }
                free(q);
                return 0;
        }

        case IPC_SET: {
                int ret = ipc_perm_check(&q->perm, 0);
                if (ret < 0) return ret;
                if (buf == NULL) return -EFAULT;

                msqid_ds_t ds;
                if (copy_from_user(&ds, buf, sizeof(msqid_ds_t)) != 0)
                        return -EFAULT;

                spin_lock(&q->lock);
                q->perm.uid  = ds.msg_perm.uid;
                q->perm.gid  = ds.msg_perm.gid;
                q->perm.mode = (ds.msg_perm.mode & 0777) |
                               (q->perm.mode & ~0777U);
                q->qbytes    = ds.msg_qbytes;
                q->ctime     = sched_ticks();
                spin_unlock(&q->lock);

                /* Wake senders if queue capacity increased */
                wait_queue_wake_all(&q->send_wq);
                return 0;
        }

        case IPC_STAT: {
                int ret = ipc_perm_check(&q->perm, 0444);
                if (ret < 0) return ret;
                if (buf == NULL) return -EFAULT;

                msqid_ds_t ds;
                memset(&ds, 0, sizeof(ds));
                spin_lock(&q->lock);
                ds.msg_perm   = q->perm;
                ds.msg_stime  = q->stime;
                ds.msg_rtime  = q->rtime;
                ds.msg_ctime  = q->ctime;
                ds.msg_cbytes = q->cbytes;
                ds.msg_qnum   = q->qnum;
                ds.msg_qbytes = q->qbytes;
                ds.msg_lspid  = q->lspid;
                ds.msg_lrpid  = q->lrpid;
                spin_unlock(&q->lock);

                if (copy_to_user(buf, &ds, sizeof(msqid_ds_t)) != 0)
                        return -EFAULT;
                return 0;
        }

        case IPC_INFO:
        case MSG_INFO: {
                if (buf == NULL) return -EFAULT;
                msginfo_t info;
                memset(&info, 0, sizeof(info));
                info.msgmax = MSGMAX;
                info.msgmnb = MSGMNB;
                info.msgmni = MSG_MAX_QUEUES;
                info.msgtql = MSG_MAX_QUEUES * 16;
                info.msgseg = MSG_MAX_QUEUES * 64;
                if (copy_to_user(buf, &info, sizeof(msginfo_t)) != 0)
                        return -EFAULT;

                if (cmd == MSG_INFO) {
                        spin_lock(&msg_global_lock);
                        int used = 0;
                        for (int i = 0; i < MSG_MAX_QUEUES; i++) {
                                if (msg_queues[i] != NULL) used++;
                        }
                        spin_unlock(&msg_global_lock);
                        return used;
                }
                return MSG_MAX_QUEUES;
        }

        case MSG_STAT: {
                if (buf == NULL) return -EFAULT;
                int idx = msqid & IPC_ID_MASK;
                if (idx < 0 || idx >= MSG_MAX_QUEUES) return -EINVAL;

                spin_lock(&msg_global_lock);
                msg_queue_t *mq = msg_queues[idx];
                if (mq == NULL) {
                        spin_unlock(&msg_global_lock);
                        return -EINVAL;
                }
                msqid_ds_t ds;
                memset(&ds, 0, sizeof(ds));
                ds.msg_perm   = mq->perm;
                ds.msg_stime  = mq->stime;
                ds.msg_rtime  = mq->rtime;
                ds.msg_ctime  = mq->ctime;
                ds.msg_cbytes = mq->cbytes;
                ds.msg_qnum   = mq->qnum;
                ds.msg_qbytes = mq->qbytes;
                ds.msg_lspid  = mq->lspid;
                ds.msg_lrpid  = mq->lrpid;
                spin_unlock(&msg_global_lock);

                if (copy_to_user(buf, &ds, sizeof(msqid_ds_t)) != 0)
                        return -EFAULT;
                return ((int)mq->perm.seq << IPC_SEQ_SHIFT) | idx;
        }

        default:
                return -EINVAL;
        }
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                      */
/* ------------------------------------------------------------------ */

/*
 *  sysv_ipc_init - initialize all System V IPC subsystems
 */
void sysv_ipc_init(void)
{
        memset(sem_sets, 0, sizeof(sem_sets));
        memset(sem_seq, 0, sizeof(sem_seq));
        memset(shm_segs, 0, sizeof(shm_segs));
        memset(shm_seq, 0, sizeof(shm_seq));
        memset(msg_queues, 0, sizeof(msg_queues));
        memset(msg_seq, 0, sizeof(msg_seq));

        sem_undo_list = NULL;

        plogk("sysv_ipc: Subsystem initialized "
              "(sem=%d, shm=%d, msg=%d)\n",
              SEM_MAX_SETS, SHM_MAX_SEGS, MSG_MAX_QUEUES);
}