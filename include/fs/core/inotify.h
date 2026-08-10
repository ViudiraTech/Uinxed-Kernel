/*
 *
 *      inotify.h
 *      Linux-compatible filesystem event notification
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_inotify_H_
#define INCLUDE_inotify_H_

#include <fs/core/vfs.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#define IN_ACCESS        0x00000001U
#define IN_MODIFY        0x00000002U
#define IN_ATTRIB        0x00000004U
#define IN_CLOSE_WRITE   0x00000008U
#define IN_CLOSE_NOWRITE 0x00000010U
#define IN_OPEN          0x00000020U
#define IN_MOVED_FROM    0x00000040U
#define IN_MOVED_TO      0x00000080U
#define IN_CREATE        0x00000100U
#define IN_DELETE        0x00000200U
#define IN_DELETE_SELF   0x00000400U
#define IN_MOVE_SELF     0x00000800U

#define IN_UNMOUNT    0x00002000U
#define IN_Q_OVERFLOW 0x00004000U
#define IN_IGNORED    0x00008000U

#define IN_ONLYDIR     0x01000000U
#define IN_DONT_FOLLOW 0x02000000U
#define IN_EXCL_UNLINK 0x04000000U
#define IN_MASK_CREATE 0x10000000U
#define IN_MASK_ADD    0x20000000U
#define IN_ISDIR       0x40000000U
#define IN_ONESHOT     0x80000000U

#define IN_CLOSE (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_MOVE  (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_ALL_EVENTS                                                                                                                      \
    (IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_OPEN | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE \
     | IN_DELETE_SELF | IN_MOVE_SELF)

#define IN_CLOEXEC  0x00080000
#define IN_NONBLOCK 0x00000800

#ifndef INOTIFY_MAX_QUEUED_EVENTS
#    define INOTIFY_MAX_QUEUED_EVENTS 16384U
#endif
#ifndef INOTIFY_MAX_USER_INSTANCES
#    define INOTIFY_MAX_USER_INSTANCES 128U
#endif
#ifndef INOTIFY_MAX_USER_WATCHES
#    define INOTIFY_MAX_USER_WATCHES 8192U
#endif

struct inotify_event {
        int32_t  wd;
        uint32_t mask;
        uint32_t cookie;
        uint32_t len;
        char     name[];
};

_Static_assert(sizeof(struct inotify_event) == 16, "Linux x86_64 inotify_event must be 16 bytes");
_Static_assert(__builtin_offsetof(struct inotify_event, len) == 12, "inotify_event.len must start at byte 12");

typedef struct inotify_queue_event {
        struct inotify_queue_event *next;
        size_t                      size;
        struct inotify_event        event;
} inotify_queue_event_t;

typedef struct inotify_watch inotify_watch_t;

typedef struct inotify_context {
        spinlock_t              lock;
        wait_queue_t            wait_queue;
        inotify_queue_event_t  *head;
        inotify_queue_event_t  *tail;
        inotify_watch_t        *watches;
        struct inotify_context *next;
        vfs_node_t              node;
        size_t                  queued_bytes;
        uint32_t                queued_events;
        int32_t                 next_watch_descriptor;
        uint32_t                owner_uid;
        bool                    overflow_queued;
        bool                    closed;
} inotify_context_t;

int sys_inotify_init(void);
int sys_inotify_init1(int flags);
int sys_inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int sys_inotify_rm_watch(int fd, int wd);

void inotify_init(void);

/* VFS event producers. Events are emitted only after the operation commits. */
void     inotify_notify(vfs_node_t node, uint32_t mask);
void     inotify_notify_create(vfs_node_t parent, vfs_node_t node);
void     inotify_notify_delete(vfs_node_t node);
void     inotify_notify_move(vfs_node_t node, const char *old_name, const char *new_name);
void     inotify_notify_unmount(vfs_node_t mount_root);
uint32_t inotify_next_cookie(void);

#endif // INCLUDE_inotify_H_
