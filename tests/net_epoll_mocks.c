#include "net_epoll_mocks.h"
#include <fs/vfs.h>
#include <ipc/epoll.h>
#include <kernel/errno.h>
#include <proc/process.h>
#include <proc/uaccess.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <syscall/syscall.h>

#define MOCK_MAX_FS 8
struct mock_target { vfs_node_t node; int fd; uint32_t ready; };
static process_t process;
static uint64_t ticks, deadline;
static unsigned sleeps, timed_sleeps;
static void (*sleep_hook)(void);
static int fs_count, target_fsid;
struct vfs_callback vfs_empty_callback;
vfs_callback_t fs_callbacks[MOCK_MAX_FS];
vfs_node_t rootdir;

void spin_lock(spinlock_t *lock) { (void)lock; }
void spin_unlock(spinlock_t *lock) { (void)lock; }
void wait_queue_init(wait_queue_t *queue) { memset(queue, 0, sizeof(*queue)); }
void wait_queue_prepare(wait_queue_t *queue) { (void)queue; }
void wait_queue_sleep(void) { sleeps++; void (*hook)(void) = sleep_hook; sleep_hook = NULL; if (hook) hook(); }
int wait_queue_wait_timed(wait_queue_t *queue, uint64_t end) { (void)queue; timed_sleeps++; deadline = end; if (sleep_hook) { void (*hook)(void) = sleep_hook; sleep_hook = NULL; hook(); return 0; } ticks = end; return -ETIMEDOUT; }
task_t *wait_queue_wake_one(wait_queue_t *queue) { (void)queue; return NULL; }
uint64_t wait_queue_wake_all(wait_queue_t *queue) { (void)queue; return 0; }
uint64_t sched_ticks(void) { return ticks; }
int copy_from_user(void *dst, const void *src, size_t size) { if (!dst || !src) return 1; memcpy(dst, src, size); return 0; }
int copy_to_user(void *dst, const void *src, size_t size) { if (!dst || !src) return 1; memcpy(dst, src, size); return 0; }
void printk(const char *format, ...) { (void)format; }
void plogk(const char *format, ...) { (void)format; }

void vfs_poll_source_init(vfs_poll_source_t *s) { if (s) memset(s, 0, sizeof(*s)); }
void vfs_poll_source_subscribe(vfs_poll_source_t *s, vfs_poll_subscription_t *sub, uint32_t events, vfs_poll_notify_t notify, void *context)
{
    if (!s || !sub || !notify) return;
    sub->notify = notify;
    sub->context = context;
    sub->events = events;
    sub->next = s->subscribers;
    sub->subscribed = true;
    s->subscribers = sub;
    if (s->closed) notify(sub, UINT32_MAX);
}
void vfs_poll_source_unsubscribe(vfs_poll_source_t *s, vfs_poll_subscription_t *sub)
{
    if (!s || !sub) return;
    vfs_poll_subscription_t **p = &s->subscribers;
    while (*p && *p != sub) p = &(*p)->next;
    if (*p) *p = sub->next;
    sub->next = NULL;
    sub->subscribed = false;
}
void vfs_poll_source_notify(vfs_poll_source_t *s, uint32_t events)
{
    if (!s) return;
    for (vfs_poll_subscription_t *sub = s->subscribers; sub;) {
        vfs_poll_subscription_t *next = sub->next;
        uint32_t matched = events & sub->events;
        if (matched) sub->notify(sub, matched);
        sub = next;
    }
}
void vfs_poll_subscribe(vfs_node_t n, vfs_poll_subscription_t *s, uint32_t e, vfs_poll_notify_t f, void *c) { if (n) vfs_poll_source_subscribe(&n->poll_source, s, e, f, c); }
void vfs_poll_unsubscribe(vfs_node_t n, vfs_poll_subscription_t *s) { if (n) vfs_poll_source_unsubscribe(&n->poll_source, s); }
void vfs_poll_notify(vfs_node_t n, uint32_t e) { if (n) vfs_poll_source_notify(&n->poll_source, e); }
int vfs_regist(vfs_callback_t cb) { if (!cb || fs_count == MOCK_MAX_FS) return -ENOMEM; fs_callbacks[fs_count] = cb; return fs_count++; }
vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name) { (void)name; vfs_node_t n = calloc(1, sizeof(*n)); if (n) { n->parent = parent; n->refcount = 1; vfs_poll_source_init(&n->poll_source); } return n; }
int vfs_close(vfs_node_t n) { if (!n) return -EINVAL; vfs_callback_t cb = fs_callbacks[n->fsid]; if (cb && cb->close) cb->close(n->handle); if (cb && cb->free) cb->free(n->handle); free(n); return 0; }

static int target_poll(void *handle, size_t events) { struct mock_target *t = handle; return (int)(t->ready & events); }
static int target_free(void *handle) { struct mock_target *t = handle; t->node = NULL; free(t); return 0; }
static struct vfs_callback target_cb = {.poll = target_poll, .free = target_free};
process_t *process_current(void) { return &process; }
void process_file_get(process_file_t *f) { if (f) f->refcount++; }
void process_file_put(process_file_t *f) { if (f && !--f->refcount) { vfs_close(f->node); free(f); } }
process_file_t *process_fd_get(process_t *p, int fd) { if (!p || fd < 0 || fd >= PROCESS_MAX_FD || !p->fds[fd]) return NULL; process_file_get(p->fds[fd]); return p->fds[fd]; }
int process_file_poll(process_file_t *f, size_t events) { return fs_callbacks[f->node->fsid]->poll(f->node->handle, events); }
int process_fd_install(process_t *p, vfs_node_t n, uint64_t flags)
{
    int fd; for (fd = 0; fd < PROCESS_MAX_FD && p->fds[fd]; fd++) {} if (fd == PROCESS_MAX_FD) return -EMFILE; process_file_t *f = calloc(1, sizeof(*f)); if (!f) return -ENOMEM; f->node = n; f->flags = flags; f->refcount = f->fd_refcount = 1; vfs_poll_source_init(&f->close_source); p->fds[fd] = f; return fd;
}
int mock_fd_close(int fd)
{
    if (fd < 0 || fd >= PROCESS_MAX_FD || !process.fds[fd]) return -EBADF;
    process_file_t *f = process.fds[fd];
    process.fds[fd] = NULL;
    if (!--f->fd_refcount && !f->descriptors_closed) {
        f->descriptors_closed = true;
        f->close_source.closed = true;
        vfs_poll_source_notify(&f->close_source, UINT32_MAX);
    }
    process_file_put(f);
    return 0;
}
int mock_fd_dup(int fd)
{
    if (fd < 0 || fd >= PROCESS_MAX_FD || !process.fds[fd]) return -EBADF;
    int n;
    for (n = 0; n < PROCESS_MAX_FD && process.fds[n]; n++) {}
    if (n == PROCESS_MAX_FD) return -EMFILE;
    process.fds[n] = process.fds[fd];
    process.fds[n]->refcount++;
    process.fds[n]->fd_refcount++;
    return n;
}
mock_target_t *mock_target_open(uint32_t ready)
{
    mock_target_t *t = calloc(1, sizeof(*t)); if (!t) return NULL; t->ready = ready; t->node = vfs_node_alloc(NULL, "target"); if (!t->node) { free(t); return NULL; } t->node->type = file_stream; t->node->fsid = target_fsid; t->node->handle = t; t->fd = process_fd_install(&process, t->node, O_RDWR); return t;
}
int mock_target_fd(const mock_target_t *t) { return t ? t->fd : -1; }
void mock_target_set_ready(mock_target_t *t, uint32_t events, int ready) { if (!t || !t->node) return; if (ready) t->ready |= events; else t->ready &= ~events; vfs_poll_notify(t->node, events); }
unsigned mock_target_subscribers(const mock_target_t *t) { unsigned n = 0; if (t && t->node) for (vfs_poll_subscription_t *s = t->node->poll_source.subscribers; s; s = s->next) n++; return n; }
void mock_set_sleep_hook(void (*hook)(void)) { sleep_hook = hook; }
unsigned mock_sleep_calls(void) { return sleeps; } unsigned mock_timed_wait_calls(void) { return timed_sleeps; } uint64_t mock_last_deadline(void) { return deadline; }
void mock_set_sched_ticks(uint64_t value) { ticks = value; } uint64_t mock_sched_ticks(void) { return ticks; }
void mock_epoll_reset(void) { for (int i = 0; i < PROCESS_MAX_FD; i++) if (process.fds[i]) mock_fd_close(i); memset(&process, 0, sizeof(process)); ticks = deadline = 0; sleeps = timed_sleeps = 0; sleep_hook = NULL; }
void mock_epoll_init(void) { memset(fs_callbacks, 0, sizeof(fs_callbacks)); fs_count = 0; target_fsid = vfs_regist(&target_cb); epoll_init(); mock_epoll_reset(); }
