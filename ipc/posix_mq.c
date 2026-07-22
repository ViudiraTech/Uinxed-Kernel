/*
 *
 *      posix_mq.c
 *      POSIX Message Queue implementation
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <alloc.h>
#include <errno.h>
#include <posix_mq.h>
#include <printk.h>
#include <process.h>
#include <sched.h>
#include <signal.h>
#include <syscall.h>
#include <spin_lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <task.h>
#include <uaccess.h>
#include <vfs.h>

/* ------------------------------------------------------------------ */
/*  Local constants (O_EXCL / O_CLOEXEC not defined in syscall.h)       */
/* ------------------------------------------------------------------ */

#define O_EXCL    0x0080
#define O_CLOEXEC 0x80000

/* ------------------------------------------------------------------ */
/*  Internal structures                                                 */
/* ------------------------------------------------------------------ */

#define MQ_MAX_QUEUES 64

typedef struct mq_message {
        struct mq_message *next;
        uint32_t           prio;
        size_t             size;
        char               data[];
} mq_message_t;

typedef struct mq_queue mq_queue_t;

typedef struct mq_des {
        mq_queue_t *queue;
        int         flags;
        uint32_t    refcount;
        spinlock_t  lock;
} mq_des_t;

struct mq_queue {
        char         name[MQ_NAME_MAX];
        mq_attr_t    attr;
        mq_message_t *head;
        mq_message_t *tail;
        uint32_t     msg_count;
        uint32_t     refcount;
        int          unlinked;
        wait_queue_t send_wq;
        wait_queue_t recv_wq;
        spinlock_t   lock;
        /* mq_notify */
        sigevent_t   notify;
        task_t      *notify_task;
        int          notify_pending;
};

/* ------------------------------------------------------------------ */
/*  Global queue registry                                               */
/* ------------------------------------------------------------------ */

static mq_queue_t *mq_registry[MQ_MAX_QUEUES];
static spinlock_t  mq_registry_lock;
static int         mq_fsid = -1;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */

static mq_queue_t *mq_queue_lookup(const char *name);
static mq_queue_t *mq_queue_create(const char *name, const mq_attr_t *attr);
static void        mq_queue_destroy(mq_queue_t *queue);
static void        mq_notify_signal(mq_queue_t *queue);

/* ------------------------------------------------------------------ */
/*  Registry helpers                                                    */
/* ------------------------------------------------------------------ */

static mq_queue_t *mq_queue_lookup(const char *name)
{
        for (int i = 0; i < MQ_MAX_QUEUES; i++) {
                if (mq_registry[i] && strcmp(mq_registry[i]->name, name) == 0) {
                        return mq_registry[i];
                }
        }
        return NULL;
}

static mq_queue_t *mq_queue_create(const char *name, const mq_attr_t *attr)
{
        mq_queue_t *queue;
        int         slot = -1;

        /* Find a free slot */
        for (int i = 0; i < MQ_MAX_QUEUES; i++) {
                if (!mq_registry[i]) {
                        slot = i;
                        break;
                }
        }
        if (slot < 0) return NULL;

        queue = malloc(sizeof(mq_queue_t));
        if (!queue) return NULL;
        memset(queue, 0, sizeof(mq_queue_t));

        strncpy(queue->name, name, MQ_NAME_MAX - 1);
        queue->name[MQ_NAME_MAX - 1] = '\0';

        /* Set default or user-provided attributes */
        if (attr) {
                queue->attr.mq_maxmsg  = attr->mq_maxmsg;
                queue->attr.mq_msgsize = attr->mq_msgsize;
                queue->attr.mq_flags   = 0;
        } else {
                queue->attr.mq_maxmsg  = MQ_MAXMSG_DEFAULT;
                queue->attr.mq_msgsize = MQ_MSGSIZE_DEFAULT;
                queue->attr.mq_flags   = 0;
        }

        /* Clamp to valid ranges */
        if (queue->attr.mq_maxmsg <= 0 || queue->attr.mq_maxmsg > MQ_MAXMSG_MAX) {
                queue->attr.mq_maxmsg = MQ_MAXMSG_DEFAULT;
        }
        if (queue->attr.mq_msgsize <= 0 || queue->attr.mq_msgsize > MQ_MSGSIZE_MAX) {
                queue->attr.mq_msgsize = MQ_MSGSIZE_DEFAULT;
        }

        queue->attr.mq_curmsgs = 0;
        queue->msg_count       = 0;
        queue->refcount        = 0;
        queue->unlinked        = 0;
        queue->head            = NULL;
        queue->tail            = NULL;
        queue->notify_task     = NULL;
        queue->notify_pending  = 0;

        wait_queue_init(&queue->send_wq);
        wait_queue_init(&queue->recv_wq);

        mq_registry[slot] = queue;

        return queue;
}

static void mq_queue_destroy(mq_queue_t *queue)
{
        if (!queue) return;

        /* Free all messages still in the queue */
        mq_message_t *msg = queue->head;
        while (msg) {
                mq_message_t *next = msg->next;
                free(msg);
                msg = next;
        }
        queue->head      = NULL;
        queue->tail      = NULL;
        queue->msg_count = 0;

        /* Wake any waiters */
        wait_queue_wake_all(&queue->send_wq);
        wait_queue_wake_all(&queue->recv_wq);

        /* Remove from registry */
        for (int i = 0; i < MQ_MAX_QUEUES; i++) {
                if (mq_registry[i] == queue) {
                        mq_registry[i] = NULL;
                        break;
                }
        }

        free(queue);
}

/* ------------------------------------------------------------------ */
/*  Message priority insertion (highest priority first)                  */
/* ------------------------------------------------------------------ */

static void mq_enqueue(mq_queue_t *queue, mq_message_t *msg)
{
        if (!queue->head) {
                /* Empty queue */
                queue->head = msg;
                queue->tail = msg;
                msg->next   = NULL;
        } else if (msg->prio > queue->head->prio) {
                /* Insert at head (highest priority) */
                msg->next   = queue->head;
                queue->head = msg;
        } else {
                /* Find insertion point: first message with prio <= msg->prio */
                mq_message_t *prev = queue->head;
                mq_message_t *cur  = queue->head->next;

                while (cur && cur->prio > msg->prio) {
                        prev = cur;
                        cur  = cur->next;
                }

                prev->next = msg;
                msg->next  = cur;

                if (!cur) {
                        queue->tail = msg;
                }
        }

        queue->msg_count++;
        queue->attr.mq_curmsgs = queue->msg_count;
}

/* Dequeue the highest priority message (always at head) */
static mq_message_t *mq_dequeue(mq_queue_t *queue)
{
        if (!queue->head) return NULL;

        mq_message_t *msg = queue->head;
        queue->head = msg->next;
        if (!queue->head) {
                queue->tail = NULL;
        }
        msg->next = NULL;

        queue->msg_count--;
        queue->attr.mq_curmsgs = queue->msg_count;

        return msg;
}

/* ------------------------------------------------------------------ */
/*  mq_notify signal delivery                                           */
/* ------------------------------------------------------------------ */

static void mq_notify_signal(mq_queue_t *queue)
{
        task_t *task;

        spin_lock(&queue->lock);

        if (!queue->notify_task || queue->notify_pending) {
                spin_unlock(&queue->lock);
                return;
        }

        task = queue->notify_task;

        if (queue->notify.sigev_notify == SIGEV_SIGNAL) {
                queue->notify_pending = 1;
                spin_unlock(&queue->lock);

                /* Send signal to the registered task's process */
                if (task->process) {
                        siginfo_t info;
                        memset(&info, 0, sizeof(info));
                        info.si_signo = queue->notify.sigev_signo;
                        info.si_value = queue->notify.sigev_value;
                        signal_send(task->process, queue->notify.sigev_signo, &info);
                }
        } else {
                /* SIGEV_NONE or unsupported: just mark pending */
                queue->notify_pending = 1;
                spin_unlock(&queue->lock);
        }
}

/* ------------------------------------------------------------------ */
/*  VFS callback: close                                                 */
/* ------------------------------------------------------------------ */

static void mq_vfs_close(void *current)
{
        mq_des_t *des = (mq_des_t *)current;
        if (!des) return;

        spin_lock(&des->lock);
        des->refcount--;
        if (des->refcount > 0) {
                spin_unlock(&des->lock);
                return;
        }
        spin_unlock(&des->lock);

        /* Last reference to this descriptor */
        mq_queue_t *queue = des->queue;
        if (queue) {
                spin_lock(&mq_registry_lock);
                spin_lock(&queue->lock);

                queue->refcount--;
                if (queue->refcount == 0 && queue->unlinked) {
                        spin_unlock(&queue->lock);
                        mq_queue_destroy(queue);
                        spin_unlock(&mq_registry_lock);
                } else {
                        spin_unlock(&queue->lock);
                        spin_unlock(&mq_registry_lock);
                }
        }

        free(des);
}

/* ------------------------------------------------------------------ */
/*  VFS callback: read (delegate to mq_timedreceive)                     */
/* ------------------------------------------------------------------ */

static size_t mq_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
        (void)offset;
        mq_des_t *des = (mq_des_t *)file;
        if (!des || !des->queue) return (size_t)-1;

        /* VFS read is always blocking; no timeout, no prio pointer */
        mq_queue_t *queue = des->queue;

        spin_lock(&queue->lock);

        while (queue->msg_count == 0) {
                if (queue->unlinked) {
                        spin_unlock(&queue->lock);
                        return 0;
                }
                spin_unlock(&queue->lock);
                wait_queue_wait(&queue->recv_wq);
                spin_lock(&queue->lock);
        }

        mq_message_t *msg = mq_dequeue(queue);
        spin_unlock(&queue->lock);

        if (!msg) return 0;

        size_t copy_size = (size < msg->size) ? size : msg->size;
        memcpy(addr, msg->data, copy_size);

        free(msg);

        wait_queue_wake_all(&queue->send_wq);

        return copy_size;
}

/* ------------------------------------------------------------------ */
/*  VFS callback: write (delegate to mq_timedsend)                       */
/* ------------------------------------------------------------------ */

static size_t mq_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
        (void)offset;
        mq_des_t *des = (mq_des_t *)file;
        if (!des || !des->queue) return (size_t)-1;

        mq_queue_t *queue = des->queue;

        /* Validate message size */
        if (size > (size_t)queue->attr.mq_msgsize) {
                return (size_t)-1;
        }

        spin_lock(&queue->lock);

        while (queue->msg_count >= (uint32_t)queue->attr.mq_maxmsg) {
                if (queue->unlinked) {
                        spin_unlock(&queue->lock);
                        return (size_t)-1;
                }
                spin_unlock(&queue->lock);
                wait_queue_wait(&queue->send_wq);
                spin_lock(&queue->lock);
        }

        mq_message_t *msg = malloc(sizeof(mq_message_t) + size);
        if (!msg) {
                spin_unlock(&queue->lock);
                return (size_t)-1;
        }

        msg->prio = 0;
        msg->size = size;
        memcpy(msg->data, addr, size);

        mq_enqueue(queue, msg);

        spin_unlock(&queue->lock);

        wait_queue_wake_all(&queue->recv_wq);

        /* Notify if registered */
        mq_notify_signal(queue);

        return size;
}

/* ------------------------------------------------------------------ */
/*  VFS callback: free (release handle)                                  */
/* ------------------------------------------------------------------ */

static int mq_vfs_free(void *handle)
{
        /* Free is handled by mq_vfs_close */
        (void)handle;
        return EOK;
}

/* ------------------------------------------------------------------ */
/*  VFS callback stubs                                                  */
/* ------------------------------------------------------------------ */

static int mq_stub_mount(const char *s, vfs_node_t n)
{
        (void)s;
        (void)n;
        return -ENOSYS;
}

static void mq_stub_unmount(void *root)
{
        (void)root;
}

static void mq_stub_open(void *parent, const char *name, vfs_node_t node)
{
        (void)parent;
        (void)name;
        (void)node;
}

static size_t mq_stub_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
        (void)node;
        (void)addr;
        (void)offset;
        (void)size;
        return (size_t)-1;
}

static int mq_stub_mk(void *parent, const char *name, vfs_node_t node)
{
        (void)parent;
        (void)name;
        (void)node;
        return -ENOSYS;
}

static int mq_stub_stat(void *file, vfs_node_t node)
{
        (void)file;
        (void)node;
        return -ENOSYS;
}

static int mq_stub_ioctl(void *file, size_t req, void *arg)
{
        (void)file;
        (void)req;
        (void)arg;
        return -ENOSYS;
}

static vfs_node_t mq_stub_dup(vfs_node_t node)
{
        (void)node;
        return NULL;
}

static int mq_stub_poll(void *file, size_t events)
{
        (void)file;
        (void)events;
        return 0;
}

static int mq_stub_del(void *parent, vfs_node_t node)
{
        (void)parent;
        (void)node;
        return -ENOSYS;
}

static int mq_stub_rename(void *current, const char *new_name)
{
        (void)current;
        (void)new_name;
        return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/*  VFS node creation for mq descriptor                                  */
/* ------------------------------------------------------------------ */

static int mq_des_install(mq_des_t *des)
{
        process_t *proc = process_current();
        if (!proc) return -ESRCH;

        if (mq_fsid < 0) return -ENOSYS;

        vfs_node_t node = vfs_node_alloc(NULL, "[posix_mq]");
        if (!node) return -ENOMEM;

        node->type   = file_socket;
        node->handle = des;
        node->fsid   = mq_fsid;
        node->mode   = 0644;
        node->size   = 0;

        int fd = process_fd_install(proc, node, (uint64_t)des->flags);
        if (fd < 0) {
                vfs_close(node);
                vfs_delete(node);
                return fd;
        }

        return fd;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mq_open                                                    */
/* ------------------------------------------------------------------ */

int64_t sys_mq_open(const char *name, int oflag, uint32_t mode, mq_attr_t *attr)
{
        char name_buf[MQ_NAME_MAX];
        int  ret;

        (void)mode;

        if (!name) return -EFAULT;

        /* Copy name from user space */
        if (strncpy_from_user(name_buf, name, sizeof(name_buf)) < 0) {
                return -EFAULT;
        }

        /* Validate name: must start with '/' */
        if (name_buf[0] != '/') return -EINVAL;
        if (strlen(name_buf) >= MQ_NAME_MAX) return -ENAMETOOLONG;

        mq_attr_t kernel_attr;
        int       creating = (oflag & O_CREAT);

        if (creating && attr) {
                if (copy_from_user(&kernel_attr, attr, sizeof(mq_attr_t))) {
                        return -EFAULT;
                }
        }

        spin_lock(&mq_registry_lock);

        mq_queue_t *queue = mq_queue_lookup(name_buf);

        if (queue) {
                /* Queue already exists */
                if (creating && (oflag & O_EXCL)) {
                        spin_unlock(&mq_registry_lock);
                        return -EEXIST;
                }
                spin_lock(&queue->lock);
                queue->refcount++;
                spin_unlock(&queue->lock);
        } else if (creating) {
                /* Create new queue */
                queue = mq_queue_create(name_buf, attr ? &kernel_attr : NULL);
                if (!queue) {
                        spin_unlock(&mq_registry_lock);
                        return -ENOSPC;
                }
                spin_lock(&queue->lock);
                queue->refcount = 1;
                spin_unlock(&queue->lock);
        } else {
                spin_unlock(&mq_registry_lock);
                return -ENOENT;
        }

        spin_unlock(&mq_registry_lock);

        /* Allocate descriptor */
        mq_des_t *des = malloc(sizeof(mq_des_t));
        if (!des) {
                spin_lock(&mq_registry_lock);
                spin_lock(&queue->lock);
                queue->refcount--;
                if (queue->refcount == 0 && queue->unlinked) {
                        spin_unlock(&queue->lock);
                        mq_queue_destroy(queue);
                        spin_unlock(&mq_registry_lock);
                } else {
                        spin_unlock(&queue->lock);
                        spin_unlock(&mq_registry_lock);
                }
                return -ENOMEM;
        }
        memset(des, 0, sizeof(mq_des_t));

        des->queue    = queue;
        des->flags    = oflag;
        des->refcount = 1;

        ret = mq_des_install(des);
        if (ret < 0) {
                free(des);
                spin_lock(&mq_registry_lock);
                spin_lock(&queue->lock);
                queue->refcount--;
                if (queue->refcount == 0 && queue->unlinked) {
                        spin_unlock(&queue->lock);
                        mq_queue_destroy(queue);
                        spin_unlock(&mq_registry_lock);
                } else {
                        spin_unlock(&queue->lock);
                        spin_unlock(&mq_registry_lock);
                }
                return ret;
        }

        return ret;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mq_unlink                                                  */
/* ------------------------------------------------------------------ */

int64_t sys_mq_unlink(const char *name)
{
        char name_buf[MQ_NAME_MAX];

        if (!name) return -EFAULT;

        if (strncpy_from_user(name_buf, name, sizeof(name_buf)) < 0) {
                return -EFAULT;
        }

        if (name_buf[0] != '/') return -EINVAL;

        spin_lock(&mq_registry_lock);

        mq_queue_t *queue = mq_queue_lookup(name_buf);
        if (!queue) {
                spin_unlock(&mq_registry_lock);
                return -ENOENT;
        }

        spin_lock(&queue->lock);
        queue->unlinked = 1;

        if (queue->refcount == 0) {
                spin_unlock(&queue->lock);
                mq_queue_destroy(queue);
                spin_unlock(&mq_registry_lock);
        } else {
                spin_unlock(&queue->lock);
                spin_unlock(&mq_registry_lock);
        }

        return EOK;
}

/* ------------------------------------------------------------------ */
/*  Look up mq_des from fd                                              */
/* ------------------------------------------------------------------ */

static mq_des_t *mq_fd_lookup(int mqdes, int *err)
{
        process_t *proc = process_current();
        if (!proc) {
                *err = -ESRCH;
                return NULL;
        }

        if (mqdes < 0 || mqdes >= PROCESS_MAX_FD) {
                *err = -EBADF;
                return NULL;
        }

        spin_lock(&proc->fd_lock);
        process_file_t *pfile = proc->fds[mqdes];
        if (!pfile || !pfile->node) {
                spin_unlock(&proc->fd_lock);
                *err = -EBADF;
                return NULL;
        }

        vfs_node_t node = pfile->node;
        if (node->fsid != mq_fsid || !node->handle) {
                spin_unlock(&proc->fd_lock);
                *err = -EBADF;
                return NULL;
        }

        mq_des_t *des = (mq_des_t *)node->handle;
        spin_unlock(&proc->fd_lock);

        *err = EOK;
        return des;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mq_timedsend                                               */
/* ------------------------------------------------------------------ */

int64_t sys_mq_timedsend(int mqdes, const char *msg_ptr, size_t msg_len,
                         uint32_t msg_prio, const void *abs_timeout)
{
        int      err;
        uint64_t deadline = 0;
        int      has_timeout = 0;

        if (!msg_ptr) return -EFAULT;

        mq_des_t *des = mq_fd_lookup(mqdes, &err);
        if (!des) return err;

        /* Validate priority */
        if (msg_prio > (uint32_t)MQ_PRIO_MAX) return -EINVAL;

        mq_queue_t *queue = des->queue;
        if (!queue) return -EBADF;

        /* Validate message size */
        if (msg_len > (size_t)queue->attr.mq_msgsize) return -EMSGSIZE;

        /* Parse timeout */
        if (abs_timeout) {
                uint64_t timeout_ticks;
                if (copy_from_user(&timeout_ticks, abs_timeout, sizeof(uint64_t))) {
                        return -EFAULT;
                }
                deadline    = timeout_ticks;
                has_timeout = 1;
        }

        spin_lock(&queue->lock);

        /* Check if unlinked */
        if (queue->unlinked) {
                spin_unlock(&queue->lock);
                return -EBADF;
        }

        /* Block until space available or timeout */
        while (queue->msg_count >= (uint32_t)queue->attr.mq_maxmsg) {
                if (des->flags & O_NONBLOCK) {
                        spin_unlock(&queue->lock);
                        return -EAGAIN;
                }
                if (has_timeout && sched_ticks() >= deadline) {
                        spin_unlock(&queue->lock);
                        return -ETIMEDOUT;
                }
                if (queue->unlinked) {
                        spin_unlock(&queue->lock);
                        return -EBADF;
                }

                spin_unlock(&queue->lock);
                wait_queue_wait(&queue->send_wq);

                /* Re-check timeout after wakeup */
                if (has_timeout && sched_ticks() >= deadline) {
                        return -ETIMEDOUT;
                }

                spin_lock(&queue->lock);
        }

        /* Allocate and populate message */
        mq_message_t *msg = malloc(sizeof(mq_message_t) + msg_len);
        if (!msg) {
                spin_unlock(&queue->lock);
                return -ENOMEM;
        }

        msg->prio = msg_prio;
        msg->size = msg_len;
        if (copy_from_user(msg->data, msg_ptr, msg_len)) {
                spin_unlock(&queue->lock);
                free(msg);
                return -EFAULT;
        }

        mq_enqueue(queue, msg);

        spin_unlock(&queue->lock);

        /* Wake up a blocked receiver */
        wait_queue_wake_one(&queue->recv_wq);

        /* Deliver notification if registered */
        mq_notify_signal(queue);

        return EOK;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mq_timedreceive                                            */
/* ------------------------------------------------------------------ */

int64_t sys_mq_timedreceive(int mqdes, char *msg_ptr, size_t msg_len,
                            uint32_t *msg_prio, const void *abs_timeout)
{
        int      err;
        uint64_t deadline = 0;
        int      has_timeout = 0;

        if (!msg_ptr) return -EFAULT;

        mq_des_t *des = mq_fd_lookup(mqdes, &err);
        if (!des) return err;

        mq_queue_t *queue = des->queue;
        if (!queue) return -EBADF;

        /* Validate buffer size */
        if (msg_len < (size_t)queue->attr.mq_msgsize) return -EMSGSIZE;

        /* Parse timeout */
        if (abs_timeout) {
                uint64_t timeout_ticks;
                if (copy_from_user(&timeout_ticks, abs_timeout, sizeof(uint64_t))) {
                        return -EFAULT;
                }
                deadline    = timeout_ticks;
                has_timeout = 1;
        }

        spin_lock(&queue->lock);

        /* Block until message available or timeout */
        while (queue->msg_count == 0) {
                if (des->flags & O_NONBLOCK) {
                        spin_unlock(&queue->lock);
                        return -EAGAIN;
                }
                if (has_timeout && sched_ticks() >= deadline) {
                        spin_unlock(&queue->lock);
                        return -ETIMEDOUT;
                }
                if (queue->unlinked) {
                        spin_unlock(&queue->lock);
                        return -EBADF;
                }

                spin_unlock(&queue->lock);
                wait_queue_wait(&queue->recv_wq);

                /* Re-check timeout after wakeup */
                if (has_timeout && sched_ticks() >= deadline) {
                        return -ETIMEDOUT;
                }

                spin_lock(&queue->lock);
        }

        /* Dequeue highest priority message */
        mq_message_t *msg = mq_dequeue(queue);

        /* Clear notification pending flag — a successful receive
         * means the queue is no longer non-empty, so the notification
         * condition is cleared. */
        queue->notify_pending = 0;

        spin_unlock(&queue->lock);

        if (!msg) return -EAGAIN;

        /* Copy message data to user */
        size_t copy_size = (msg_len < msg->size) ? msg_len : msg->size;
        if (copy_to_user(msg_ptr, msg->data, copy_size)) {
                free(msg);
                return -EFAULT;
        }

        /* Copy priority if requested */
        if (msg_prio) {
                uint32_t prio = msg->prio;
                if (copy_to_user(msg_prio, &prio, sizeof(uint32_t))) {
                        free(msg);
                        return -EFAULT;
                }
        }

        int64_t ret = (int64_t)msg->size;
        free(msg);

        /* Wake a blocked sender */
        wait_queue_wake_one(&queue->send_wq);

        return ret;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mq_notify                                                  */
/* ------------------------------------------------------------------ */

int64_t sys_mq_notify(int mqdes, const sigevent_t *notification)
{
        int err;

        mq_des_t *des = mq_fd_lookup(mqdes, &err);
        if (!des) return err;

        mq_queue_t *queue = des->queue;
        if (!queue) return -EBADF;

        spin_lock(&queue->lock);

        if (!notification) {
                /* Cancel existing notification */
                queue->notify_task    = NULL;
                queue->notify_pending = 0;
                memset(&queue->notify, 0, sizeof(sigevent_t));
                spin_unlock(&queue->lock);
                return EOK;
        }

        /* Only one registration at a time */
        if (queue->notify_task) {
                spin_unlock(&queue->lock);
                return -EBUSY;
        }

        sigevent_t sev;
        if (copy_from_user(&sev, notification, sizeof(sigevent_t))) {
                spin_unlock(&queue->lock);
                return -EFAULT;
        }

        /* Validate notification type */
        if (sev.sigev_notify != SIGEV_NONE && sev.sigev_notify != SIGEV_SIGNAL) {
                spin_unlock(&queue->lock);
                return -EINVAL;
        }

        /* Validate signal number for SIGEV_SIGNAL */
        if (sev.sigev_notify == SIGEV_SIGNAL) {
                if (sev.sigev_signo <= 0 || sev.sigev_signo >= NSIG) {
                        spin_unlock(&queue->lock);
                        return -EINVAL;
                }
        }

        queue->notify         = sev;
        queue->notify_task    = current_task();
        queue->notify_pending = 0;

        /* If queue already has messages, deliver notification immediately */
        if (queue->msg_count > 0) {
                spin_unlock(&queue->lock);
                mq_notify_signal(queue);
        } else {
                spin_unlock(&queue->lock);
        }

        return EOK;
}

/* ------------------------------------------------------------------ */
/*  Syscall: mq_getsetattr                                              */
/* ------------------------------------------------------------------ */

int64_t sys_mq_getsetattr(int mqdes, const mq_attr_t *newattr, mq_attr_t *oldattr)
{
        int err;

        mq_des_t *des = mq_fd_lookup(mqdes, &err);
        if (!des) return err;

        mq_queue_t *queue = des->queue;
        if (!queue) return -EBADF;

        spin_lock(&queue->lock);

        /* Return old attributes if requested */
        if (oldattr) {
                mq_attr_t attr;
                attr.mq_flags   = (des->flags & O_NONBLOCK) ? O_NONBLOCK : 0;
                attr.mq_maxmsg  = queue->attr.mq_maxmsg;
                attr.mq_msgsize = queue->attr.mq_msgsize;
                attr.mq_curmsgs = queue->attr.mq_curmsgs;

                spin_unlock(&queue->lock);

                if (copy_to_user(oldattr, &attr, sizeof(mq_attr_t))) {
                        return -EFAULT;
                }
                return EOK;
        }

        /* Set new attributes if requested */
        if (newattr) {
                mq_attr_t nattr;
                spin_unlock(&queue->lock);

                if (copy_from_user(&nattr, newattr, sizeof(mq_attr_t))) {
                        return -EFAULT;
                }

                spin_lock(&des->lock);
                /* Only mq_flags (O_NONBLOCK) can be changed */
                if (nattr.mq_flags & O_NONBLOCK) {
                        des->flags |= O_NONBLOCK;
                } else {
                        des->flags &= ~O_NONBLOCK;
                }
                spin_unlock(&des->lock);
        }

        return EOK;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                      */
/* ------------------------------------------------------------------ */

void posix_mq_init(void)
{
        /* Initialize registry */
        memset(mq_registry, 0, sizeof(mq_registry));

        /* Register VFS callback */
        vfs_callback_t cb = malloc(sizeof(struct vfs_callback));
        if (!cb) {
                plogk("posix_mq: Failed to allocate VFS callback structure.\n");
                return;
        }
        memset(cb, 0, sizeof(struct vfs_callback));

        cb->mount    = mq_stub_mount;
        cb->unmount  = mq_stub_unmount;
        cb->open     = mq_stub_open;
        cb->close    = mq_vfs_close;
        cb->read     = mq_vfs_read;
        cb->write    = mq_vfs_write;
        cb->readlink = mq_stub_readlink;
        cb->mkdir    = mq_stub_mk;
        cb->mkfile   = mq_stub_mk;
        cb->link     = mq_stub_mk;
        cb->symlink  = mq_stub_mk;
        cb->stat     = mq_stub_stat;
        cb->ioctl    = mq_stub_ioctl;
        cb->dup      = mq_stub_dup;
        cb->poll     = mq_stub_poll;
        cb->free     = mq_vfs_free;
        cb->delete   = mq_stub_del;
        cb->rename   = mq_stub_rename;

        mq_fsid = vfs_regist(cb);
        if (mq_fsid < 0) {
                plogk("posix_mq: Failed to register VFS callback (err=%d).\n", mq_fsid);
                free(cb);
                return;
        }

        plogk("posix_mq: Subsystem initialized (fsid=%d, max_queues=%d)\n",
              mq_fsid, MQ_MAX_QUEUES);
}