/*
 *
 *      sysv_ipc.h
 *      System V IPC (semaphores, shared memory, message queues) header file
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSV_IPC_H_
#define INCLUDE_SYSV_IPC_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* ------------------------------------------------------------------ */
/*  IPC common                                                         */
/* ------------------------------------------------------------------ */

#define IPC_CREAT   0x0200
#define IPC_EXCL    0x0400
#define IPC_NOWAIT  0x0800
#define IPC_PRIVATE ((key_t)0)

#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
#define IPC_INFO 3

#define IPC_64 0x0100

typedef int32_t key_t;

typedef struct ipc_perm {
        uint32_t uid;
        uint32_t gid;
        uint32_t cuid;
        uint32_t cgid;
        uint32_t mode;
        uint32_t seq;
        key_t    key;
} ipc_perm_t;

/* ------------------------------------------------------------------ */
/*  Semaphores                                                         */
/* ------------------------------------------------------------------ */

#define SEM_UNDO 0x1000

#define GETNCNT  14
#define GETPID   11
#define GETVAL   12
#define GETALL   13
#define GETZCNT  15
#define SETVAL   16
#define SETALL   17
#define SEM_STAT 18
#define SEM_INFO 19

#define SEMOPM 500

typedef struct sembuf {
        uint16_t sem_num;
        int16_t  sem_op;
        int16_t  sem_flg;
} sembuf_t;

typedef struct semid_ds {
        ipc_perm_t sem_perm;
        uint64_t   sem_otime;
        uint64_t   sem_ctime;
        uint32_t   sem_nsems;
        uint32_t   __pad;
} semid_ds_t;

typedef struct seminfo {
        int32_t semmap;
        int32_t semmni;
        int32_t semmns;
        int32_t semmnu;
        int32_t semmsl;
        int32_t semopm;
        int32_t semume;
        int32_t semusz;
        int32_t semvmx;
        int32_t semaem;
} seminfo_t;

/* ------------------------------------------------------------------ */
/*  Shared memory                                                      */
/* ------------------------------------------------------------------ */

#define SHM_RDONLY 0x1000
#define SHM_RND    0x2000
#define SHM_REMAP  0x4000
#define SHM_EXEC   0x100000

#define SHM_LOCK    11
#define SHM_UNLOCK  12
#define SHM_STAT    13
#define SHM_INFO    14
#define SHM_DEST    0x1000
#define SHM_LOCKED  0x2000
#define SHM_HUGETLB 0x4000

#define SHM_SIZE_MAX 0x100000000ULL

typedef struct shmid_ds {
        ipc_perm_t shm_perm;
        size_t     shm_segsz;
        uint64_t   shm_atime;
        uint64_t   shm_dtime;
        uint64_t   shm_ctime;
        uint32_t   shm_cpid;
        uint32_t   shm_lpid;
        uint32_t   shm_nattch;
        uint32_t   __pad;
} shmid_ds_t;

typedef struct shminfo {
        uint64_t shmmax;
        uint64_t shmmin;
        uint64_t shmmni;
        uint64_t shmseg;
        uint64_t shmall;
} shminfo_t;

/* ------------------------------------------------------------------ */
/*  Message queues                                                     */
/* ------------------------------------------------------------------ */

#define MSG_NOERROR 0x0100
#define MSG_EXCEPT  0x0200
#define MSG_COPY    0x0400

#define MSG_STAT 11
#define MSG_INFO 12
#define MSG_MNGR 13

#define MSGMAX 8192
#define MSGMNB 16384
#define MSGMNI 32000

typedef uint32_t msgqnum_t;
typedef uint32_t msglen_t;

typedef struct msgbuf {
        int64_t mtype;
        char    mtext[1];
} msgbuf_t;

typedef struct msqid_ds {
        ipc_perm_t msg_perm;
        uint64_t   msg_stime;
        uint64_t   msg_rtime;
        uint64_t   msg_ctime;
        uint64_t   msg_cbytes;
        msgqnum_t  msg_qnum;
        msglen_t   msg_qbytes;
        uint32_t   msg_lspid;
        uint32_t   msg_lrpid;
        uint32_t   __pad;
} msqid_ds_t;

typedef struct msginfo {
        int32_t msgpool;
        int32_t msgmap;
        int32_t msgmax;
        int32_t msgmnb;
        int32_t msgmni;
        int32_t msgssz;
        int32_t msgtql;
        int32_t msgseg;
} msginfo_t;

/* ------------------------------------------------------------------ */
/*  System V IPC syscall interface                                     */
/* ------------------------------------------------------------------ */

int64_t sys_semget(key_t key, int nsems, int semflg);
int64_t sys_semop(int semid, sembuf_t *sops, size_t nsops);
int64_t sys_semtimedop(int semid, sembuf_t *sops, size_t nsops, const void *timeout);
int64_t sys_semctl(int semid, int semnum, int cmd, uint64_t arg);

int64_t sys_shmget(key_t key, size_t size, int shmflg);
int64_t sys_shmat(int shmid, const void *shmaddr, int shmflg);
int64_t sys_shmdt(const void *shmaddr);
int64_t sys_shmctl(int shmid, int cmd, void *buf);

int64_t sys_msgget(key_t key, int msgflg);
int64_t sys_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
int64_t sys_msgrcv(int msqid, void *msgp, size_t msgsz, int64_t msgtyp, int msgflg);
int64_t sys_msgctl(int msqid, int cmd, void *buf);

void sysv_ipc_init(void);

#endif /* INCLUDE_SYSV_IPC_H_ */