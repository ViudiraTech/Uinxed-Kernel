/*
 *
 *      timer.c
 *      ULSA ALSA-compatible timer subsystem (/dev/snd/timer and PCM slave timers)
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <kernel/debug/printk.h>
#include <kernel/debug/vsprintf.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <process/kthread.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/uaccess.h>
#include <syscall/fcntl.h>
#include <syscall/poll.h>

#if CONFIG_AUDIO

#    define TIMER_DEFAULT_QUEUE_SIZE 128U
#    define TIMER_MIN_QUEUE_SIZE     32U
#    define TIMER_MAX_QUEUE_SIZE     1024U
#    define TIMER_MIN_RESOLUTION_NS  1000000UL

/* Compatibility ioctl variants issued by older userspace or alsa-lib < 2.0.4. */
#    define TIMER_IOCTL_STATUS_OLD   _IOW('T', 0x14, struct snd_timer_status)
#    define TIMER_IOCTL_START_OLD    _IO('T', 0x20)
#    define TIMER_IOCTL_STOP_OLD     _IO('T', 0x21)
#    define TIMER_IOCTL_CONTINUE_OLD _IO('T', 0x22)
#    define TIMER_IOCTL_PAUSE_OLD    _IO('T', 0x23)

#    define TIMER_EVENT_VALID_MASK                                                                                                                                                     \
        ((1U << SNDRV_TIMER_EVENT_RESOLUTION) | (1U << SNDRV_TIMER_EVENT_TICK) | (1U << SNDRV_TIMER_EVENT_START) | (1U << SNDRV_TIMER_EVENT_STOP) | (1U << SNDRV_TIMER_EVENT_CONTINUE) \
         | (1U << SNDRV_TIMER_EVENT_PAUSE) | (1U << SNDRV_TIMER_EVENT_SUSPEND) | (1U << SNDRV_TIMER_EVENT_RESUME) | (1U << SNDRV_TIMER_EVENT_MSTART) | (1U << SNDRV_TIMER_EVENT_MSTOP) \
         | (1U << SNDRV_TIMER_EVENT_MCONTINUE) | (1U << SNDRV_TIMER_EVENT_MPAUSE) | (1U << SNDRV_TIMER_EVENT_MSUSPEND) | (1U << SNDRV_TIMER_EVENT_MRESUME))

#    define TIMER_IFLG_AUTO        (1U << 0)
#    define TIMER_IFLG_EXCLUSIVE   (1U << 1)
#    define TIMER_IFLG_EARLY_EVENT (1U << 2)
#    define TIMER_IFLG_PAUSED      (1U << 3)

typedef struct ulsa_timer      ulsa_timer_t;
typedef struct ulsa_timer_user ulsa_timer_user_t;

struct ulsa_timer_user {
        ulsa_timer_t      *timer;
        ulsa_timer_user_t *next;

        mutex_t           ioctl_lock;
        spinlock_t        qlock;
        wait_queue_t      wait;
        vfs_poll_source_t poll_source;

        bool tread;
        bool running;
        bool disconnected;

        unsigned int  iflags;
        unsigned int  ticks;
        unsigned int  filter;
        unsigned int  lost;
        unsigned int  overrun;
        unsigned long last_resolution;

        uint64_t next_tick;
        uint64_t rem_ticks;

        struct snd_timespec     tstamp;
        struct snd_timer_read  *queue;
        struct snd_timer_tread *tqueue;
        unsigned int            queue_size;
        unsigned int            qhead;
        unsigned int            qtail;
        unsigned int            qused;
};

struct ulsa_timer {
        struct snd_timer_id id;
        char                str_id[64];
        char                name[80];
        bool                slave;
        unsigned long       resolution;
        unsigned long       resolution_min;
        unsigned long       resolution_max;

        ulsa_card_t          *card;
        ulsa_pcm_substream_t *substream;

        spinlock_t         lock; // lock hierarchy: timer_mutex / substream->lock -> timer->lock -> tu->qlock
        int                refcount;
        unsigned int       clients;
        unsigned int       running;
        bool               registered;
        ulsa_timer_user_t *users;
        ulsa_timer_t      *next;
};

static mutex_t       timer_mutex;
static ulsa_timer_t *timer_list;
static ulsa_timer_t *sys_timer;
static ulsa_minor_t *timer_minor;
static wait_queue_t  sys_wait;

/* Compute the greatest common divisor of two positive integers. */
static unsigned long timer_gcd(unsigned long a, unsigned long b)
{
    while (b != 0) {
        unsigned long t = a % b;
        a               = b;
        b               = t;
    }
    return a;
}

/* Fill an id struct with the sentinel values representing "no timer". */
static void timer_zero_id(struct snd_timer_id *id)
{
    id->dev_class  = SNDRV_TIMER_CLASS_NONE;
    id->dev_sclass = SNDRV_TIMER_SCLASS_NONE;
    id->card       = -1;
    id->device     = -1;
    id->subdevice  = -1;
}

/* Compare two timer identifiers in Linux's canonical enumeration order. */
static int timer_id_cmp(const struct snd_timer_id *a, const struct snd_timer_id *b)
{
    if (a->dev_class != b->dev_class) return a->dev_class < b->dev_class ? -1 : 1;
    if (a->card != b->card) return a->card < b->card ? -1 : 1;
    if (a->device != b->device) return a->device < b->device ? -1 : 1;
    if (a->subdevice != b->subdevice) return a->subdevice < b->subdevice ? -1 : 1;
    return 0;
}

/* Capture the current monotonic timestamp as a snd_timespec. */
static void timer_now_ts(struct snd_timespec *ts)
{
    uint64_t ns = timer_monotonic_ns();

    ts->tv_sec  = (int64_t)(ns / 1000000000ULL);
    ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
}

/* Retain a reference on a timer instance; timer_mutex or timer->lock held. */
static void timer_get(ulsa_timer_t *t)
{
    if (t) __atomic_add_fetch(&t->refcount, 1, __ATOMIC_RELAXED);
}

/* Drop a reference on a timer instance and free it once the last holder leaves. */
static void timer_put(ulsa_timer_t *t)
{
    if (!t) return;
    if (__atomic_sub_fetch(&t->refcount, 1, __ATOMIC_ACQ_REL) == 0) free(t);
}

/* Look up a registered timer by id (matching Linux's class/sclass rules); timer_mutex held. */
static ulsa_timer_t *timer_find_locked(const struct snd_timer_id *id)
{
    for (ulsa_timer_t *t = timer_list; t; t = t->next) {
        if (!t->registered) continue;
        if (t->id.dev_class != id->dev_class) continue;
        if (t->id.card != id->card) continue;
        if (t->id.device != id->device) continue;
        if (t->id.subdevice != id->subdevice) continue;
        return t;
    }
    return NULL;
}

/* Insert a timer into the sorted global list; timer_mutex held. */
static void timer_insert_locked(ulsa_timer_t *t)
{
    ulsa_timer_t **pp = &timer_list;

    while (*pp && timer_id_cmp(&(*pp)->id, &t->id) < 0) pp = &(*pp)->next;
    t->next       = *pp;
    t->registered = true;
    *pp           = t;
}

/* Allocate or resize the read/tread ring buffer of a user timer handle. */
static int timer_realloc_queue(ulsa_timer_user_t *tu, unsigned int size)
{
    struct snd_timer_read  *q  = NULL;
    struct snd_timer_tread *tq = NULL;
    uint64_t                flags;

    if (tu->tread) {
        tq = calloc(size, sizeof(*tq));
        if (!tq) return -ENOMEM;
    } else {
        q = calloc(size, sizeof(*q));
        if (!q) return -ENOMEM;
    }

    flags = spin_lock_irqsave(&tu->qlock);
    free(tu->queue);
    free(tu->tqueue);
    tu->queue      = q;
    tu->tqueue     = tq;
    tu->queue_size = size;
    tu->qhead      = 0;
    tu->qtail      = 0;
    tu->qused      = 0;
    spin_unlock_irqrestore(&tu->qlock, flags);
    return EOK;
}

/* Append one tread record into the user handle ring; tu->qlock held. */
static void timer_push_tread_locked(ulsa_timer_user_t *tu, int event, const struct snd_timespec *ts, unsigned int val)
{
    struct snd_timer_tread *slot;

    if (!tu->tqueue || !tu->queue_size) return;
    if (tu->qused >= tu->queue_size) {
        tu->overrun++;
        return;
    }
    slot = &tu->tqueue[tu->qtail++];
    tu->qtail %= tu->queue_size;
    tu->qused++;
    memset(slot, 0, sizeof(*slot));
    slot->event  = event;
    slot->tstamp = *ts;
    slot->val    = val;
}

/* Enqueue tick/resolution notification for one user handle and wake readers/pollers. */
static void timer_user_deliver_tick(ulsa_timer_user_t *tu, unsigned long resolution, unsigned int ticks, const struct snd_timespec *ts)
{
    bool     wake = false;
    uint64_t flags;

    flags = spin_lock_irqsave(&tu->qlock);
    if (!tu->tread) {
        if (tu->qused > 0 && tu->queue && tu->queue_size) {
            unsigned int           prev = tu->qtail == 0 ? tu->queue_size - 1 : tu->qtail - 1;
            struct snd_timer_read *r    = &tu->queue[prev];
            if (r->resolution == (unsigned int)resolution) {
                r->ticks += ticks;
                wake = true;
                goto done;
            }
        }
        if (!tu->queue || !tu->queue_size || tu->qused >= tu->queue_size) {
            tu->overrun++;
        } else {
            struct snd_timer_read *r = &tu->queue[tu->qtail++];
            tu->qtail %= tu->queue_size;
            r->resolution = (unsigned int)resolution;
            r->ticks      = ticks;
            tu->qused++;
            wake = true;
        }
    } else {
        const unsigned int want_res  = tu->filter & (1U << SNDRV_TIMER_EVENT_RESOLUTION);
        const unsigned int want_tick = tu->filter & (1U << SNDRV_TIMER_EVENT_TICK);

        if (want_res && tu->last_resolution != resolution) {
            timer_push_tread_locked(tu, SNDRV_TIMER_EVENT_RESOLUTION, ts, (unsigned int)resolution);
            tu->last_resolution = resolution;
            wake                = true;
        }
        if (want_tick && ticks > 0) {
            if (tu->qused > 0 && tu->tqueue && tu->queue_size) {
                unsigned int            prev = tu->qtail == 0 ? tu->queue_size - 1 : tu->qtail - 1;
                struct snd_timer_tread *r    = &tu->tqueue[prev];
                if (r->event == SNDRV_TIMER_EVENT_TICK) {
                    r->tstamp = *ts;
                    r->val += ticks;
                    wake = true;
                    goto done;
                }
            }
            timer_push_tread_locked(tu, SNDRV_TIMER_EVENT_TICK, ts, ticks);
            wake = true;
        }
    }
done:
    spin_unlock_irqrestore(&tu->qlock, flags);
    if (wake) {
        wait_queue_wake_all(&tu->wait);
        vfs_poll_source_notify(&tu->poll_source, POLLIN | POLLRDNORM);
    }
}

/* Enqueue a lifecycle control event (START/STOP/CONTINUE/PAUSE/M*) for one user handle. */
static void timer_user_deliver_event(ulsa_timer_user_t *tu, int event, const struct snd_timespec *ts, unsigned long val)
{
    uint64_t flags;
    bool     wake = false;

    if (event >= SNDRV_TIMER_EVENT_START && event <= SNDRV_TIMER_EVENT_PAUSE) tu->tstamp = *ts;
    if (!tu->tread || !(tu->filter & (1U << event))) return;

    flags = spin_lock_irqsave(&tu->qlock);
    timer_push_tread_locked(tu, event, ts, (unsigned int)val);
    wake = tu->qused > 0;
    spin_unlock_irqrestore(&tu->qlock, flags);
    if (wake) {
        wait_queue_wake_all(&tu->wait);
        vfs_poll_source_notify(&tu->poll_source, POLLIN | POLLRDNORM);
    }
}

/* Stop one user timer handle; caller holds tu->ioctl_lock (and optionally timer_mutex). */
static int timer_stop_user(ulsa_timer_user_t *tu, bool pause)
{
    ulsa_timer_t       *t = tu->timer;
    struct snd_timespec ts;
    uint64_t            flags;

    if (!t) return -EBADFD;
    timer_now_ts(&ts);

    flags = spin_lock_irqsave(&t->lock);
    if (!tu->running) {
        spin_unlock_irqrestore(&t->lock, flags);
        return EOK;
    }
    if (pause && !t->slave) {
        uint64_t now  = sched_ticks();
        tu->rem_ticks = tu->next_tick > now ? tu->next_tick - now : 1;
        tu->iflags |= TIMER_IFLG_PAUSED;
    } else {
        tu->rem_ticks = 0;
        tu->iflags &= ~TIMER_IFLG_PAUSED;
    }
    tu->running = false;
    if (t->running > 0) t->running--;
    if (t->slave && t->running == 0 && t->substream) __atomic_store_n(&t->substream->timer_running, false, __ATOMIC_RELEASE);
    timer_user_deliver_event(tu, pause ? SNDRV_TIMER_EVENT_PAUSE : SNDRV_TIMER_EVENT_STOP, &ts, 0);
    spin_unlock_irqrestore(&t->lock, flags);

    if (!t->slave) (void)wait_queue_wake_all(&sys_wait);
    return EOK;
}

/* Start or resume one user timer handle; caller holds tu->ioctl_lock. */
static int timer_start_user(ulsa_timer_user_t *tu, bool resume)
{
    ulsa_timer_t       *t = tu->timer;
    struct snd_timespec ts;
    uint64_t            flags;
    unsigned long       res;

    if (!t) return -EBADFD;
    timer_now_ts(&ts);

    flags = spin_lock_irqsave(&t->lock);
    if (t->id.dev_class == SNDRV_TIMER_CLASS_NONE || !t->registered) {
        spin_unlock_irqrestore(&t->lock, flags);
        return -ENODEV;
    }
    if (tu->running) {
        spin_unlock_irqrestore(&t->lock, flags);
        return -EBUSY;
    }
    if (!resume) {
        for (ulsa_timer_user_t *other = t->users; other; other = other->next) {
            if (other != tu && other->running && ((tu->iflags & TIMER_IFLG_EXCLUSIVE) || (other->iflags & TIMER_IFLG_EXCLUSIVE))) {
                spin_unlock_irqrestore(&t->lock, flags);
                return -EBUSY;
            }
        }
        tu->lost            = 0;
        tu->last_resolution = 0;
        tu->iflags &= ~TIMER_IFLG_PAUSED;
        tu->next_tick = sched_ticks() + (tu->ticks ? tu->ticks : 1);
    } else {
        uint64_t rem = tu->rem_ticks;
        if (!rem) rem = tu->ticks ? tu->ticks : 1;
        tu->iflags &= ~TIMER_IFLG_PAUSED;
        tu->next_tick = sched_ticks() + rem;
    }
    tu->running = true;
    t->running++;
    if (t->slave && t->substream) __atomic_store_n(&t->substream->timer_running, true, __ATOMIC_RELEASE);
    res = t->resolution;
    timer_user_deliver_event(tu, resume ? SNDRV_TIMER_EVENT_CONTINUE : SNDRV_TIMER_EVENT_START, &ts, res);
    spin_unlock_irqrestore(&t->lock, flags);

    if (!t->slave) (void)wait_queue_wake_all(&sys_wait);
    return EOK;
}

/* Detach the currently selected timer from a user handle; caller holds tu->ioctl_lock. */
static void timer_detach_user(ulsa_timer_user_t *tu)
{
    ulsa_timer_t *t = tu->timer;
    uint64_t      flags;

    if (!t) return;
    (void)timer_stop_user(tu, false);

    flags = spin_lock_irqsave(&t->lock);
    for (ulsa_timer_user_t **pp = &t->users; *pp; pp = &(*pp)->next) {
        if (*pp == tu) {
            *pp = tu->next;
            break;
        }
    }
    tu->next = NULL;
    if (t->clients > 0) t->clients--;
    spin_unlock_irqrestore(&t->lock, flags);

    tu->timer = NULL;
    timer_put(t);
}

/* Background thread driving system global timers using wait_queue_wait_timed. */
static int timer_sys_worker(void *arg)
{
    (void)arg;
    while (!kthread_should_stop()) {
        ulsa_timer_t *t = sys_timer;
        uint64_t      flags;
        uint64_t      earliest = UINT64_MAX;
        uint64_t      now      = sched_ticks();

        if (!t) {
            usleep(10000);
            continue;
        }

        flags = spin_lock_irqsave(&t->lock);
        for (ulsa_timer_user_t *tu = t->users; tu; tu = tu->next) {
            if (!tu->running) continue;
            if (now >= tu->next_tick) {
                const uint64_t      step   = tu->ticks ? tu->ticks : 1;
                uint64_t            missed = (now - tu->next_tick) / step;
                struct snd_timespec ts;

                timer_now_ts(&ts);
                if (tu->iflags & TIMER_IFLG_AUTO) {
                    uint64_t total_ticks = (missed + 1ULL) * step;
                    tu->next_tick += total_ticks;
                    timer_user_deliver_tick(tu, t->resolution, (unsigned int)total_ticks, &ts);
                } else {
                    tu->running = false;
                    if (t->running > 0) t->running--;
                    if (missed) tu->lost += (unsigned int)missed;
                    timer_user_deliver_tick(tu, t->resolution, (unsigned int)step, &ts);
                    continue;
                }
            }
            if (tu->running && tu->next_tick < earliest) earliest = tu->next_tick;
        }

        wait_queue_prepare(&sys_wait);
        if (kthread_should_stop() || (earliest != UINT64_MAX && earliest <= sched_ticks())) {
            wait_queue_cancel(&sys_wait);
            spin_unlock_irqrestore(&t->lock, flags);
            continue;
        }
        spin_unlock_irqrestore(&t->lock, flags);

        if (earliest == UINT64_MAX) {
            wait_queue_sleep();
        } else {
            (void)wait_queue_wait_timed(&sys_wait, earliest);
        }
    }
    return EOK;
}

/* Find the next registered timer after `*id` in canonical order. */
static void timer_next_device_query(struct snd_timer_id *id)
{
    struct snd_timer_id key = *id;
    ulsa_timer_t       *hit = NULL;

    mutex_lock(&timer_mutex);
    if (key.dev_class < 0) {
        for (ulsa_timer_t *t = timer_list; t; t = t->next) {
            if (t->registered) {
                hit = t;
                break;
            }
        }
    } else {
        if (key.dev_class == SNDRV_TIMER_CLASS_GLOBAL) {
            key.device = key.device < 0 ? 0 : key.device + 1;
        } else {
            if (key.card < 0) {
                key.card = 0;
            } else if (key.device < 0) {
                key.device = 0;
            } else if (key.subdevice < 0) {
                key.subdevice = 0;
            } else if (key.subdevice < 0x7fffffff) {
                key.subdevice++;
            }
        }
        for (ulsa_timer_t *t = timer_list; t; t = t->next) {
            if (!t->registered) continue;
            if (t->id.dev_class > key.dev_class || (t->id.dev_class == key.dev_class && timer_id_cmp(&t->id, &key) >= 0)) {
                hit = t;
                break;
            }
        }
    }
    if (hit) {
        *id            = hit->id;
        id->dev_sclass = SNDRV_TIMER_SCLASS_NONE;
    } else {
        timer_zero_id(id);
    }
    mutex_unlock(&timer_mutex);
}

/* Fill snd_timer_ginfo for the requested timer id. */
static int timer_ioctl_ginfo(void *arg)
{
    struct snd_timer_ginfo gi;
    struct snd_timer_id    tid;
    ulsa_timer_t          *t;
    uint64_t               flags;

    if (copy_from_user(&gi, arg, sizeof(gi))) return -EFAULT;
    tid = gi.tid;
    memset(&gi, 0, sizeof(gi));
    gi.tid = tid;

    mutex_lock(&timer_mutex);
    t = timer_find_locked(&tid);
    if (!t) {
        mutex_unlock(&timer_mutex);
        return -ENODEV;
    }
    gi.card = t->card ? t->card->number : -1;
    if (t->slave) gi.flags |= SNDRV_TIMER_FLG_SLAVE;
    strncpy((char *)gi.id, t->str_id, sizeof(gi.id) - 1);
    strncpy((char *)gi.name, t->name, sizeof(gi.name) - 1);
    flags         = spin_lock_irqsave(&t->lock);
    gi.resolution = t->resolution;
    gi.clients    = t->clients;
    spin_unlock_irqrestore(&t->lock, flags);
    gi.resolution_min = t->resolution_min;
    gi.resolution_max = t->resolution_max;
    mutex_unlock(&timer_mutex);

    return copy_to_user(arg, &gi, sizeof(gi)) ? -EFAULT : EOK;
}

/* Fill snd_timer_gstatus for the requested timer id. */
static int timer_ioctl_gstatus(void *arg)
{
    struct snd_timer_gstatus gs;
    struct snd_timer_id      tid;
    ulsa_timer_t            *t;
    uint64_t                 flags;

    if (copy_from_user(&gs, arg, sizeof(gs))) return -EFAULT;
    tid = gs.tid;
    memset(&gs, 0, sizeof(gs));
    gs.tid = tid;

    mutex_lock(&timer_mutex);
    t = timer_find_locked(&tid);
    if (!t) {
        mutex_unlock(&timer_mutex);
        return -ENODEV;
    }
    flags             = spin_lock_irqsave(&t->lock);
    gs.resolution     = t->resolution;
    gs.resolution_num = gs.resolution;
    gs.resolution_den = 1000000000UL;
    spin_unlock_irqrestore(&t->lock, flags);
    mutex_unlock(&timer_mutex);

    return copy_to_user(arg, &gs, sizeof(gs)) ? -EFAULT : EOK;
}

/* Bind a user timer handle to a specific timer id (SNDRV_TIMER_IOCTL_SELECT). */
static int timer_ioctl_select(ulsa_timer_user_t *tu, void *arg)
{
    struct snd_timer_select sel;
    ulsa_timer_t           *t;
    uint64_t                flags;

    if (copy_from_user(&sel, arg, sizeof(sel))) return -EFAULT;

    timer_detach_user(tu);

    mutex_lock(&timer_mutex);
    t = timer_find_locked(&sel.id);
    if (!t) {
        mutex_unlock(&timer_mutex);
        return -ENODEV;
    }
    timer_get(t);
    flags    = spin_lock_irqsave(&t->lock);
    tu->next = t->users;
    t->users = tu;
    t->clients++;
    tu->timer   = t;
    tu->running = false;
    spin_unlock_irqrestore(&t->lock, flags);
    mutex_unlock(&timer_mutex);

    flags            = spin_lock_irqsave(&tu->qlock);
    tu->disconnected = false;
    tu->qhead        = 0;
    tu->qtail        = 0;
    tu->qused        = 0;
    spin_unlock_irqrestore(&tu->qlock, flags);
    return EOK;
}

/* Fill snd_timer_info for the user handle's selected timer. */
static int timer_ioctl_info(ulsa_timer_user_t *tu, void *arg)
{
    struct snd_timer_info info;
    ulsa_timer_t         *t = tu->timer;
    uint64_t              flags;

    if (!t) return -EBADFD;
    memset(&info, 0, sizeof(info));
    info.card = t->card ? t->card->number : -1;
    if (t->slave) info.flags |= SNDRV_TIMER_FLG_SLAVE;
    strncpy((char *)info.id, t->str_id, sizeof(info.id) - 1);
    strncpy((char *)info.name, t->name, sizeof(info.name) - 1);
    flags           = spin_lock_irqsave(&t->lock);
    info.resolution = t->resolution;
    spin_unlock_irqrestore(&t->lock, flags);

    return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : EOK;
}

/* Apply timer tick period, filter, flags and queue size (SNDRV_TIMER_IOCTL_PARAMS). */
static int timer_ioctl_params(ulsa_timer_user_t *tu, void *arg)
{
    struct snd_timer_params params;
    ulsa_timer_t           *t = tu->timer;
    uint64_t                flags;

    if (!t) return -EBADFD;
    if (copy_from_user(&params, arg, sizeof(params))) return -EFAULT;

    if (!t->slave) {
        if (params.ticks < 1) return -EINVAL;
        flags              = spin_lock_irqsave(&t->lock);
        uint64_t period_ns = (uint64_t)t->resolution * params.ticks;
        spin_unlock_irqrestore(&t->lock, flags);
        if (period_ns < TIMER_MIN_RESOLUTION_NS) return -EINVAL;
    }
    if (params.queue_size > 0 && (params.queue_size < TIMER_MIN_QUEUE_SIZE || params.queue_size > TIMER_MAX_QUEUE_SIZE)) return -EINVAL;
    if (params.filter & ~TIMER_EVENT_VALID_MASK) return -EINVAL;

    (void)timer_stop_user(tu, false);

    if (params.queue_size > 0 && params.queue_size != tu->queue_size) {
        int err = timer_realloc_queue(tu, params.queue_size);
        if (err < 0) return err;
    }

    flags = spin_lock_irqsave(&t->lock);
    tu->iflags &= ~(TIMER_IFLG_AUTO | TIMER_IFLG_EXCLUSIVE | TIMER_IFLG_EARLY_EVENT);
    if (params.flags & SNDRV_TIMER_PSFLG_AUTO) tu->iflags |= TIMER_IFLG_AUTO;
    if (params.flags & SNDRV_TIMER_PSFLG_EXCLUSIVE) tu->iflags |= TIMER_IFLG_EXCLUSIVE;
    if (params.flags & SNDRV_TIMER_PSFLG_EARLY_EVENT) tu->iflags |= TIMER_IFLG_EARLY_EVENT;
    spin_unlock_irqrestore(&t->lock, flags);

    flags     = spin_lock_irqsave(&tu->qlock);
    tu->qhead = 0;
    tu->qtail = 0;
    tu->qused = 0;
    if (tu->iflags & TIMER_IFLG_EARLY_EVENT) {
        if (tu->tread) {
            struct snd_timespec zero_ts = {0, 0};
            timer_push_tread_locked(tu, SNDRV_TIMER_EVENT_EARLY, &zero_ts, 0);
        } else if (tu->queue && tu->queue_size) {
            tu->queue[0].resolution = 0;
            tu->queue[0].ticks      = 0;
            tu->qtail               = 1 % tu->queue_size;
            tu->qused               = 1;
        }
    }
    tu->ticks  = params.ticks;
    tu->filter = params.filter;
    spin_unlock_irqrestore(&tu->qlock, flags);

    return copy_to_user(arg, &params, sizeof(params)) ? -EFAULT : EOK;
}

/* Fill snd_timer_status for the user handle's selected timer. */
static int timer_ioctl_status(ulsa_timer_user_t *tu, void *arg)
{
    struct snd_timer_status status;
    ulsa_timer_t           *t = tu->timer;
    uint64_t                flags;

    if (!t) return -EBADFD;
    memset(&status, 0, sizeof(status));
    flags             = spin_lock_irqsave(&t->lock);
    status.resolution = (unsigned int)t->resolution;
    spin_unlock_irqrestore(&t->lock, flags);

    status.tstamp  = tu->tstamp;
    status.lost    = tu->lost;
    status.overrun = tu->overrun;

    flags        = spin_lock_irqsave(&tu->qlock);
    status.queue = tu->qused;
    spin_unlock_irqrestore(&tu->qlock, flags);

    return copy_to_user(arg, &status, sizeof(status)) ? -EFAULT : EOK;
}

/* Toggle 64-bit tread event format on an unselected user timer handle. */
static int timer_ioctl_tread(ulsa_timer_user_t *tu, void *arg)
{
    int  enable = 0;
    bool next_tread;

    if (tu->timer) return -EBUSY;
    if (copy_from_user(&enable, arg, sizeof(enable))) return -EFAULT;
    next_tread = enable != 0;
    if (next_tread == tu->tread) return EOK;

    tu->tread = next_tread;
    if (timer_realloc_queue(tu, tu->queue_size) < 0) {
        tu->tread = !next_tread;
        return -ENOMEM;
    }
    return EOK;
}

/* tmpfs open callback for /dev/snd/timer. */
static int timer_dev_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    ulsa_timer_user_t *tu;

    (void)node;
    (void)flags;
    if (!private_data) return -EINVAL;
    tu = calloc(1, sizeof(*tu));
    if (!tu) return -ENOMEM;
    mutex_init(&tu->ioctl_lock);
    wait_queue_init(&tu->wait);
    vfs_poll_source_init(&tu->poll_source);
    tu->ticks = 1;
    if (timer_realloc_queue(tu, TIMER_DEFAULT_QUEUE_SIZE) < 0) {
        free(tu);
        return -ENOMEM;
    }
    *private_data = tu;
    return EOK;
}

/* tmpfs release callback for /dev/snd/timer. */
static void timer_dev_release(vfs_node_t node, void *private_data)
{
    ulsa_timer_user_t *tu = private_data;

    (void)node;
    if (!tu) return;
    mutex_lock(&tu->ioctl_lock);
    timer_detach_user(tu);
    mutex_unlock(&tu->ioctl_lock);
    vfs_poll_source_close(&tu->poll_source, POLLHUP);
    free(tu->queue);
    free(tu->tqueue);
    free(tu);
}

/* Kernel-buffer reads are unsupported on /dev/snd/timer. */
static int64_t timer_dev_read_kernel(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)addr;
    (void)offset;
    (void)size;
    return -EINVAL;
}

/* read(2): drain queued snd_timer_read or snd_timer_tread records. */
static int64_t timer_dev_read_user(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, struct process *proc)
{
    ulsa_timer_user_t *tu = private_data;
    size_t             unit;
    int64_t            result = 0;
    uint64_t           lflags;

    (void)ctx;
    (void)offset;
    (void)proc;
    if (!tu) return -ENXIO;

    mutex_lock(&tu->ioctl_lock);
    unit = tu->tread ? sizeof(struct snd_timer_tread) : sizeof(struct snd_timer_read);
    if (size < unit) {
        mutex_unlock(&tu->ioctl_lock);
        return -EINVAL;
    }

    lflags = spin_lock_irqsave(&tu->qlock);
    while (size - (size_t)result >= unit) {
        while (!tu->qused) {
            if (tu->disconnected) {
                spin_unlock_irqrestore(&tu->qlock, lflags);
                mutex_unlock(&tu->ioctl_lock);
                return result > 0 ? result : -ENODEV;
            }
            if ((flags & O_NONBLOCK) || result > 0) {
                spin_unlock_irqrestore(&tu->qlock, lflags);
                mutex_unlock(&tu->ioctl_lock);
                return result > 0 ? result : -EAGAIN;
            }
            if (signal_has_pending_current()) {
                spin_unlock_irqrestore(&tu->qlock, lflags);
                mutex_unlock(&tu->ioctl_lock);
                return -ERESTARTSYS;
            }
            wait_queue_prepare(&tu->wait);
            spin_unlock_irqrestore(&tu->qlock, lflags);
            mutex_unlock(&tu->ioctl_lock);
            wait_queue_sleep();
            mutex_lock(&tu->ioctl_lock);
            lflags = spin_lock_irqsave(&tu->qlock);
        }

        unsigned int idx = tu->qhead;
        tu->qhead        = (tu->qhead + 1) % tu->queue_size;
        tu->qused--;

        if (tu->tread) {
            struct snd_timer_tread rec = tu->tqueue[idx];
            spin_unlock_irqrestore(&tu->qlock, lflags);
            if (copy_to_user((uint8_t *)addr + result, &rec, sizeof(rec))) {
                mutex_unlock(&tu->ioctl_lock);
                return result > 0 ? result : -EFAULT;
            }
        } else {
            struct snd_timer_read rec = tu->queue[idx];
            spin_unlock_irqrestore(&tu->qlock, lflags);
            if (copy_to_user((uint8_t *)addr + result, &rec, sizeof(rec))) {
                mutex_unlock(&tu->ioctl_lock);
                return result > 0 ? result : -EFAULT;
            }
        }

        result += (int64_t)unit;
        lflags = spin_lock_irqsave(&tu->qlock);
    }
    spin_unlock_irqrestore(&tu->qlock, lflags);
    mutex_unlock(&tu->ioctl_lock);
    return result;
}

/* poll(2): report POLLIN when records wait in the queue or POLLERR when detached. */
static int timer_dev_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    ulsa_timer_user_t *tu = private_data;
    uint64_t           lflags;
    size_t             mask = 0;

    (void)ctx;
    (void)flags;
    if (!tu) return 0;

    lflags = spin_lock_irqsave(&tu->qlock);
    if (tu->qused) mask |= POLLIN | POLLRDNORM;
    if (tu->disconnected) mask |= POLLERR;
    spin_unlock_irqrestore(&tu->qlock, lflags);
    return (int)(mask & (events | POLLERR));
}

/* Expose the poll/epoll source of a /dev/snd/timer file descriptor. */
static vfs_poll_source_t *timer_dev_poll_source(void *ctx, void *private_data)
{
    ulsa_timer_user_t *tu = private_data;

    (void)ctx;
    return tu ? &tu->poll_source : NULL;
}

/* ioctl(2) dispatcher for /dev/snd/timer. */
static int timer_dev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    ulsa_timer_user_t *tu  = private_data;
    int                err = EOK;

    (void)ctx;
    (void)flags;
    if (!tu) return -ENXIO;

    switch (req) {
        case SNDRV_TIMER_IOCTL_PVERSION : {
            int ver = SNDRV_TIMER_VERSION;
            return copy_to_user(arg, &ver, sizeof(ver)) ? -EFAULT : EOK;
        }
        case SNDRV_TIMER_IOCTL_NEXT_DEVICE : {
            struct snd_timer_id id;
            if (copy_from_user(&id, arg, sizeof(id))) return -EFAULT;
            timer_next_device_query(&id);
            return copy_to_user(arg, &id, sizeof(id)) ? -EFAULT : EOK;
        }
        case SNDRV_TIMER_IOCTL_GINFO :
            return timer_ioctl_ginfo(arg);
        case SNDRV_TIMER_IOCTL_GPARAMS : {
            struct snd_timer_gparams gp;
            if (copy_from_user(&gp, arg, sizeof(gp))) return -EFAULT;
            mutex_lock(&timer_mutex);
            ulsa_timer_t *t = timer_find_locked(&gp.tid);
            if (!t) {
                mutex_unlock(&timer_mutex);
                return -ENODEV;
            }
            bool busy = t->clients > 0;
            mutex_unlock(&timer_mutex);
            return busy ? -EBUSY : -ENOSYS;
        }
        case SNDRV_TIMER_IOCTL_GSTATUS :
            return timer_ioctl_gstatus(arg);
        default :
            break;
    }

    mutex_lock(&tu->ioctl_lock);
    switch (req) {
        case SNDRV_TIMER_IOCTL_TREAD_OLD :
        case SNDRV_TIMER_IOCTL_TREAD64 :
            err = timer_ioctl_tread(tu, arg);
            break;
        case SNDRV_TIMER_IOCTL_SELECT :
            err = timer_ioctl_select(tu, arg);
            break;
        case SNDRV_TIMER_IOCTL_INFO :
            err = timer_ioctl_info(tu, arg);
            break;
        case SNDRV_TIMER_IOCTL_PARAMS :
            err = timer_ioctl_params(tu, arg);
            break;
        case SNDRV_TIMER_IOCTL_STATUS :
        case TIMER_IOCTL_STATUS_OLD :
            err = timer_ioctl_status(tu, arg);
            break;
        case SNDRV_TIMER_IOCTL_START :
        case TIMER_IOCTL_START_OLD :
            err = timer_start_user(tu, false);
            break;
        case SNDRV_TIMER_IOCTL_STOP :
        case TIMER_IOCTL_STOP_OLD :
            err = timer_stop_user(tu, false);
            break;
        case SNDRV_TIMER_IOCTL_CONTINUE :
        case TIMER_IOCTL_CONTINUE_OLD :
            if (!tu->timer) {
                err = -EBADFD;
            } else if (!(tu->iflags & TIMER_IFLG_PAUSED)) {
                err = timer_start_user(tu, false);
            } else {
                err = timer_start_user(tu, true);
            }
            break;
        case SNDRV_TIMER_IOCTL_PAUSE :
        case TIMER_IOCTL_PAUSE_OLD :
            err = timer_stop_user(tu, true);
            break;
        default :
            err = -ENOTTY;
            break;
    }
    mutex_unlock(&tu->ioctl_lock);
    return err;
}

static const tmpfs_device_ops_t timer_device_ops = {
    .open             = timer_dev_open,
    .release          = timer_dev_release,
    .file_read        = timer_dev_read_kernel,
    .file_read_user   = timer_dev_read_user,
    .file_poll        = timer_dev_poll,
    .file_poll_source = timer_dev_poll_source,
    .file_ioctl       = timer_dev_ioctl,
};

/* Initialize /dev/snd/timer, register the global system timer, and queue its worker. */
int ulsa_timer_init(void)
{
    ulsa_timer_t *t;
    int           err;

    mutex_init(&timer_mutex);
    wait_queue_init(&sys_wait);

    err = ulsa_register_device(ULSA_DEVICE_TIMER, NULL, NULL, 0, 0, "timer", &timer_device_ops, &timer_minor);
    if (err != EOK) return err;

    t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    t->id.dev_class  = SNDRV_TIMER_CLASS_GLOBAL;
    t->id.dev_sclass = SNDRV_TIMER_SCLASS_NONE;
    t->id.card       = -1;
    t->id.device     = SNDRV_TIMER_GLOBAL_SYSTEM;
    t->id.subdevice  = 0;
    strncpy(t->str_id, "system", sizeof(t->str_id) - 1);
    strncpy(t->name, "system timer", sizeof(t->name) - 1);
    t->resolution = 1000000000UL / CONFIG_TIMER_HZ;
    t->refcount   = 1;

    mutex_lock(&timer_mutex);
    sys_timer = t;
    timer_insert_locked(t);
    mutex_unlock(&timer_mutex);

    err = kernel_worker_register("ulsa-timer", timer_sys_worker, NULL, NULL);
    if (err != EOK) plogk("ulsa: timer worker registration failed: %d\n", err);
    return EOK;
}

/* Attach a slave PCM timer instance to an open substream. */
int ulsa_timer_pcm_new(ulsa_pcm_substream_t *substream)
{
    ulsa_timer_t *t;

    if (!substream || !substream->pcm || !substream->pcm->card) return -EINVAL;
    if (substream->timer) return EOK;

    t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    t->id.dev_class  = SNDRV_TIMER_CLASS_PCM;
    t->id.dev_sclass = SNDRV_TIMER_SCLASS_NONE;
    t->id.card       = substream->pcm->card->number;
    t->id.device     = substream->pcm->device;
    t->id.subdevice  = (substream->number << 1) | (substream->stream & 1);
    t->slave         = true;
    t->card          = substream->pcm->card;
    t->substream     = substream;
    t->refcount      = 1;
    strncpy(t->str_id, "PCM", sizeof(t->str_id) - 1);
    (void)snprintf(t->name, sizeof(t->name), "PCM %s %d-%d-%d", substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture", t->id.card, t->id.device, t->id.subdevice);

    mutex_lock(&timer_mutex);
    timer_insert_locked(t);
    substream->timer = t;
    mutex_unlock(&timer_mutex);
    return EOK;
}

/* Detach the slave PCM timer instance when a substream closes. */
void ulsa_timer_pcm_free(ulsa_pcm_substream_t *substream)
{
    ulsa_timer_t *t;
    uint64_t      flags;

    if (!substream || !substream->timer) return;
    t                = substream->timer;
    substream->timer = NULL;
    __atomic_store_n(&substream->timer_running, false, __ATOMIC_RELEASE);

    mutex_lock(&timer_mutex);
    for (ulsa_timer_t **pp = &timer_list; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    t->next       = NULL;
    t->registered = false;
    mutex_unlock(&timer_mutex);

    flags        = spin_lock_irqsave(&t->lock);
    t->substream = NULL;
    t->running   = 0;
    for (ulsa_timer_user_t *tu = t->users; tu; tu = tu->next) {
        tu->running = false;
        spin_lock(&tu->qlock);
        tu->disconnected = true;
        spin_unlock(&tu->qlock);
        wait_queue_wake_all(&tu->wait);
        vfs_poll_source_notify(&tu->poll_source, POLLERR);
    }
    spin_unlock_irqrestore(&t->lock, flags);

    timer_put(t);
}

/* Notify running slave PCM timer listeners of stream start/stop/pause/continue; stream lock held. */
void ulsa_timer_pcm_notify(ulsa_pcm_substream_t *substream, int event)
{
    ulsa_timer_t       *t;
    struct snd_timespec ts;
    unsigned long       res = 0;
    uint64_t            flags;

    if (!substream || !substream->timer) return;
    t = substream->timer;
    timer_now_ts(&ts);

    flags = spin_lock_irqsave(&t->lock);
    if (event == SNDRV_TIMER_EVENT_MSTART || event == SNDRV_TIMER_EVENT_MCONTINUE || event == SNDRV_TIMER_EVENT_MRESUME) res = t->resolution;
    for (ulsa_timer_user_t *tu = t->users; tu; tu = tu->next) {
        if (tu->running) timer_user_deliver_event(tu, event, &ts, res);
    }
    spin_unlock_irqrestore(&t->lock, flags);
}

/* Advance slave PCM timer listeners at each period interrupt; stream lock held. */
void ulsa_timer_pcm_interrupt(ulsa_pcm_substream_t *substream)
{
    ulsa_timer_t       *t;
    struct snd_timespec ts;
    uint64_t            flags;

    if (!substream || !substream->timer) return;
    t = substream->timer;
    timer_now_ts(&ts);

    flags = spin_lock_irqsave(&t->lock);
    for (ulsa_timer_user_t *tu = t->users; tu; tu = tu->next) {
        if (!tu->running) continue;
        timer_user_deliver_tick(tu, t->resolution, 1U, &ts);
    }
    spin_unlock_irqrestore(&t->lock, flags);
}

/* Recompute the slave PCM timer period duration in nanoseconds after hw_params. */
void ulsa_timer_pcm_resolution_change(ulsa_pcm_substream_t *substream)
{
    ulsa_timer_t       *t;
    ulsa_pcm_runtime_t *rt;
    unsigned long       mult = 1000000000UL;
    unsigned long       rate;
    unsigned long       fsize;
    unsigned long       g;
    unsigned long       post = 1;
    unsigned long       res  = 0;
    uint64_t            flags;

    if (!substream || !substream->timer || !substream->runtime) return;
    t     = substream->timer;
    rt    = substream->runtime;
    rate  = rt->rate;
    fsize = rt->period_size;
    if (!rate || !fsize) return;

    g = timer_gcd(mult, rate);
    mult /= g;
    rate /= g;
    g = timer_gcd(rate, fsize);
    rate /= g;
    fsize /= g;
    while ((mult * fsize) / fsize != mult) {
        mult /= 2;
        post *= 2;
    }
    if (rate != 0) res = (mult * fsize / rate) * post;

    flags         = spin_lock_irqsave(&t->lock);
    t->resolution = res;
    spin_unlock_irqrestore(&t->lock, flags);
}

#endif
